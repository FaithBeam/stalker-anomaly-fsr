#include "fsr_pass.h"

#include "fsr_constants.h"
#include "log.h"

#include "embedded_shaders.h"

#include <d3dcompiler.h>

#include <cstring>

namespace afsr {

namespace {

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                        LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

const EmbeddedFile* FindEmbedded(const char* name)
{
    for (const EmbeddedFile& file : kEmbeddedFiles)
    {
        if (_stricmp(file.name, name) == 0)
            return &file;
    }
    return nullptr;
}

// Resolves #include "ffx_a.h" / "ffx_fsr1.h" from the copies embedded in the DLL.
class EmbeddedInclude final : public ID3DInclude
{
public:
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID, LPCVOID* data, UINT* bytes) override
    {
        const EmbeddedFile* file = FindEmbedded(fileName);
        if (!file)
            return E_FAIL;
        *data = file->data;
        *bytes = static_cast<UINT>(file->size);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }
};

PFN_D3DCompile LoadCompiler()
{
    static PFN_D3DCompile compile = [] {
        HMODULE module = LoadLibraryW(L"d3dcompiler_47.dll");
        return GetProc<PFN_D3DCompile>(module, "D3DCompile");
    }();
    return compile;
}

ID3DBlob* Compile(PFN_D3DCompile compile, const char* entry, const char* target, const char* passDefine)
{
    const EmbeddedFile* source = FindEmbedded("fsr.hlsl");
    D3D_SHADER_MACRO defines[] = {{passDefine, "1"}, {nullptr, nullptr}};
    EmbeddedInclude includes;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;

    HRESULT hr = compile(source->data, source->size, "fsr.hlsl", passDefine ? defines : nullptr, &includes,
                         entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        Log("Shader %s failed to compile (0x%08lX): %s", entry, static_cast<unsigned long>(hr),
            errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler output");
        SafeRelease(code);
    }
    SafeRelease(errors);
    return code;
}

// Everything Compose() changes on the immediate context, captured before and restored after the
// FSR passes when no ID3DDeviceContextState is available. X-Ray caches bound state, so it must
// come back exactly as it was.
class StateBackup
{
public:
    StateBackup() = default;
    StateBackup(const StateBackup&) = delete;
    StateBackup& operator=(const StateBackup&) = delete;
    ~StateBackup() { ReleaseAll(); }

    void Capture(ID3D11DeviceContext* ctx)
    {
        ctx->IAGetPrimitiveTopology(&m_topology);
        ctx->IAGetInputLayout(&m_inputLayout);
        ctx->VSGetShader(&m_vs, m_vsInstances, &(m_vsInstanceCount = kMaxInstances));
        ctx->HSGetShader(&m_hs, m_hsInstances, &(m_hsInstanceCount = kMaxInstances));
        ctx->DSGetShader(&m_ds, m_dsInstances, &(m_dsInstanceCount = kMaxInstances));
        ctx->GSGetShader(&m_gs, m_gsInstances, &(m_gsInstanceCount = kMaxInstances));
        ctx->PSGetShader(&m_ps, m_psInstances, &(m_psInstanceCount = kMaxInstances));
        ctx->PSGetConstantBuffers(0, 1, &m_psConstants);
        ctx->PSGetShaderResources(0, 1, &m_psSrv);
        ctx->PSGetSamplers(0, 1, &m_psSampler);
        ctx->RSGetState(&m_rasterizer);
        m_viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        ctx->RSGetViewports(&m_viewportCount, m_viewports);
        ctx->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, m_rtvs, &m_dsv, 0,
                                                       D3D11_PS_CS_UAV_REGISTER_COUNT, m_uavs);
        ctx->OMGetBlendState(&m_blend, m_blendFactor, &m_sampleMask);
        ctx->OMGetDepthStencilState(&m_depthStencil, &m_stencilRef);

        // Shaders we don't use must not run during our draws.
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
    }

    void Restore(ID3D11DeviceContext* ctx)
    {
        ctx->IASetPrimitiveTopology(m_topology);
        ctx->IASetInputLayout(m_inputLayout);
        ctx->VSSetShader(m_vs, m_vsInstances, m_vsInstanceCount);
        ctx->HSSetShader(m_hs, m_hsInstances, m_hsInstanceCount);
        ctx->DSSetShader(m_ds, m_dsInstances, m_dsInstanceCount);
        ctx->GSSetShader(m_gs, m_gsInstances, m_gsInstanceCount);
        ctx->PSSetShader(m_ps, m_psInstances, m_psInstanceCount);
        ctx->PSSetConstantBuffers(0, 1, &m_psConstants);
        ctx->PSSetSamplers(0, 1, &m_psSampler);
        ctx->RSSetState(m_rasterizer);
        ctx->RSSetViewports(m_viewportCount, m_viewports);
        ctx->OMSetBlendState(m_blend, m_blendFactor, m_sampleMask);
        ctx->OMSetDepthStencilState(m_depthStencil, m_stencilRef);

        // Render targets before the shader resource: binding an RTV unbinds any SRV of the same
        // resource, so the original SRV has to go back last.
        UINT rtvCount = 0;
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        {
            if (m_rtvs[i])
                rtvCount = i + 1;
        }
        UINT firstUav = D3D11_PS_CS_UAV_REGISTER_COUNT;
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i)
        {
            if (m_uavs[i])
            {
                firstUav = i;
                break;
            }
        }
        if (firstUav < D3D11_PS_CS_UAV_REGISTER_COUNT)
        {
            UINT keepCounts[D3D11_PS_CS_UAV_REGISTER_COUNT];
            for (UINT& count : keepCounts)
                count = static_cast<UINT>(-1);
            ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, m_rtvs, m_dsv, firstUav,
                                                           D3D11_PS_CS_UAV_REGISTER_COUNT - firstUav,
                                                           m_uavs + firstUav, keepCounts);
        }
        else
        {
            ctx->OMSetRenderTargets(rtvCount, m_rtvs, m_dsv);
        }
        ctx->PSSetShaderResources(0, 1, &m_psSrv);

        ReleaseAll();
    }

private:
    static constexpr UINT kMaxInstances = 256;

    template <typename T, size_t N>
    static void ReleaseArray(T* (&items)[N], UINT count = N)
    {
        for (UINT i = 0; i < count && i < N; ++i)
            SafeRelease(items[i]);
    }

    void ReleaseAll()
    {
        SafeRelease(m_inputLayout);
        SafeRelease(m_vs);
        SafeRelease(m_hs);
        SafeRelease(m_ds);
        SafeRelease(m_gs);
        SafeRelease(m_ps);
        ReleaseArray(m_vsInstances, m_vsInstanceCount);
        ReleaseArray(m_hsInstances, m_hsInstanceCount);
        ReleaseArray(m_dsInstances, m_dsInstanceCount);
        ReleaseArray(m_gsInstances, m_gsInstanceCount);
        ReleaseArray(m_psInstances, m_psInstanceCount);
        m_vsInstanceCount = m_hsInstanceCount = m_dsInstanceCount = m_gsInstanceCount = m_psInstanceCount = 0;
        SafeRelease(m_psConstants);
        SafeRelease(m_psSrv);
        SafeRelease(m_psSampler);
        SafeRelease(m_rasterizer);
        ReleaseArray(m_rtvs);
        SafeRelease(m_dsv);
        ReleaseArray(m_uavs);
        SafeRelease(m_blend);
        SafeRelease(m_depthStencil);
    }

    D3D11_PRIMITIVE_TOPOLOGY m_topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11InputLayout* m_inputLayout = nullptr;
    ID3D11VertexShader* m_vs = nullptr;
    ID3D11HullShader* m_hs = nullptr;
    ID3D11DomainShader* m_ds = nullptr;
    ID3D11GeometryShader* m_gs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11ClassInstance* m_vsInstances[kMaxInstances] = {};
    ID3D11ClassInstance* m_hsInstances[kMaxInstances] = {};
    ID3D11ClassInstance* m_dsInstances[kMaxInstances] = {};
    ID3D11ClassInstance* m_gsInstances[kMaxInstances] = {};
    ID3D11ClassInstance* m_psInstances[kMaxInstances] = {};
    UINT m_vsInstanceCount = 0;
    UINT m_hsInstanceCount = 0;
    UINT m_dsInstanceCount = 0;
    UINT m_gsInstanceCount = 0;
    UINT m_psInstanceCount = 0;
    ID3D11Buffer* m_psConstants = nullptr;
    ID3D11ShaderResourceView* m_psSrv = nullptr;
    ID3D11SamplerState* m_psSampler = nullptr;
    ID3D11RasterizerState* m_rasterizer = nullptr;
    UINT m_viewportCount = 0;
    D3D11_VIEWPORT m_viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RenderTargetView* m_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* m_dsv = nullptr;
    ID3D11UnorderedAccessView* m_uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
    ID3D11BlendState* m_blend = nullptr;
    FLOAT m_blendFactor[4] = {};
    UINT m_sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* m_depthStencil = nullptr;
    UINT m_stencilRef = 0;
};

} // namespace

bool FsrPass::Init(ID3D11Device* device)
{
    Shutdown();

    m_device = device;
    m_device->AddRef();
    device->GetImmediateContext(&m_context);

    // Preferred: a private context state object, swapped in and out around our passes. Some D3D11
    // implementations (e.g. D3DMetal under Wine/CrossOver) don't support it; then the state we
    // touch is saved and restored by hand instead.
    ID3D11Device1* device1 = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
    if (SUCCEEDED(hr))
        hr = m_context->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&m_context1));
    if (SUCCEEDED(hr))
    {
        D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
        UINT flags = (device->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                         ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                         : 0;
        hr = device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                               nullptr, &m_state);
    }
    SafeRelease(device1);
    if (FAILED(hr))
    {
        Log("Device context state objects unavailable (0x%08lX); saving and restoring pipeline state manually.",
            static_cast<unsigned long>(hr));
        SafeRelease(m_state);
        SafeRelease(m_context1);
    }

    if (!CompileShaders())
    {
        Shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;

    D3D11_RASTERIZER_DESC rasterizer = {};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC depth = {};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;

    D3D11_BUFFER_DESC constants = {};
    constants.ByteWidth = 16 * sizeof(uint32_t);
    constants.Usage = D3D11_USAGE_DEFAULT;
    constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    if (FAILED(device->CreateSamplerState(&sampler, &m_sampler)) ||
        FAILED(device->CreateRasterizerState(&rasterizer, &m_rasterizer)) ||
        FAILED(device->CreateDepthStencilState(&depth, &m_depthState)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &m_easuConstants)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &m_rcasConstants)))
    {
        Log("Failed to create FSR pipeline objects; FSR disabled.");
        Shutdown();
        return false;
    }

    std::memset(m_easuCache, 0, sizeof(m_easuCache));
    std::memset(m_rcasCache, 0, sizeof(m_rcasCache));
    return true;
}

bool FsrPass::CompileShaders()
{
    PFN_D3DCompile compile = LoadCompiler();
    if (!compile)
    {
        Log("d3dcompiler_47.dll not found; FSR disabled.");
        return false;
    }

    ID3DBlob* vs = Compile(compile, "FullscreenVS", "vs_5_0", nullptr);
    ID3DBlob* easu = Compile(compile, "EasuPS", "ps_5_0", "FSR_PASS_EASU");
    ID3DBlob* rcas = Compile(compile, "RcasPS", "ps_5_0", "FSR_PASS_RCAS");

    bool ok = vs && easu && rcas &&
              SUCCEEDED(m_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vs)) &&
              SUCCEEDED(m_device->CreatePixelShader(easu->GetBufferPointer(), easu->GetBufferSize(), nullptr, &m_easuPs)) &&
              SUCCEEDED(m_device->CreatePixelShader(rcas->GetBufferPointer(), rcas->GetBufferSize(), nullptr, &m_rcasPs));

    SafeRelease(vs);
    SafeRelease(easu);
    SafeRelease(rcas);
    return ok;
}

void FsrPass::Shutdown()
{
    ReleaseIntermediate();
    SafeRelease(m_rcasConstants);
    SafeRelease(m_easuConstants);
    SafeRelease(m_depthState);
    SafeRelease(m_rasterizer);
    SafeRelease(m_sampler);
    SafeRelease(m_rcasPs);
    SafeRelease(m_easuPs);
    SafeRelease(m_vs);
    SafeRelease(m_state);
    SafeRelease(m_context1);
    SafeRelease(m_context);
    SafeRelease(m_device);
}

void FsrPass::ReleaseIntermediate()
{
    SafeRelease(m_intermediateSrv);
    SafeRelease(m_intermediateRtv);
    SafeRelease(m_intermediate);
    m_intermediateW = m_intermediateH = 0;
    m_intermediateFormat = DXGI_FORMAT_UNKNOWN;
}

bool FsrPass::EnsureIntermediate(UINT w, UINT h, DXGI_FORMAT format)
{
    if (m_intermediate && m_intermediateW == w && m_intermediateH == h && m_intermediateFormat == format)
        return true;

    ReleaseIntermediate();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_device->CreateTexture2D(&desc, nullptr, &m_intermediate)) ||
        FAILED(m_device->CreateRenderTargetView(m_intermediate, nullptr, &m_intermediateRtv)) ||
        FAILED(m_device->CreateShaderResourceView(m_intermediate, nullptr, &m_intermediateSrv)))
    {
        Log("Failed to create %ux%u sharpening target.", w, h);
        ReleaseIntermediate();
        return false;
    }

    m_intermediateW = w;
    m_intermediateH = h;
    m_intermediateFormat = format;
    return true;
}

void FsrPass::UpdateConstants(ID3D11Buffer* buffer, uint32_t* cache, const uint32_t* values)
{
    if (std::memcmp(cache, values, 16 * sizeof(uint32_t)) != 0)
    {
        std::memcpy(cache, values, 16 * sizeof(uint32_t));
        m_context->UpdateSubresource(buffer, 0, nullptr, values, 0, 0);
    }
}

void FsrPass::Draw(ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps,
                   ID3D11Buffer* constants, UINT w, UINT h)
{
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h), 0.0f, 1.0f};
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->RSSetViewports(1, &viewport);
    m_context->PSSetShader(ps, nullptr, 0);
    m_context->PSSetConstantBuffers(0, 1, &constants);
    m_context->PSSetShaderResources(0, 1, &srv);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
}

bool FsrPass::Compose(ID3D11Texture2D* src, ID3D11ShaderResourceView* srcSrv, UINT srcW, UINT srcH,
                      ID3D11Texture2D* dst, UINT dstW, UINT dstH, DXGI_FORMAT format, bool sharpen,
                      float sharpness)
{
    if (!IsReady())
        return false;

    ID3D11RenderTargetView* dstRtv = nullptr;
    const bool upscale = srcW != dstW || srcH != dstH;
    if (upscale)
    {
        if (FAILED(m_device->CreateRenderTargetView(dst, nullptr, &dstRtv)))
            return false;
        if (sharpen && !EnsureIntermediate(dstW, dstH, format))
            sharpen = false;
    }

    if (!upscale)
    {
        m_context->CopyResource(dst, src);
        return true;
    }

    ID3DDeviceContextState* gameState = nullptr;
    StateBackup backup;
    if (m_state)
        m_context1->SwapDeviceContextState(m_state, &gameState);
    else
        backup.Capture(m_context);

    {
        uint32_t easu[16];
        ComputeEasuConstants(easu, static_cast<float>(srcW), static_cast<float>(srcH), static_cast<float>(dstW),
                             static_cast<float>(dstH));
        UpdateConstants(m_easuConstants, m_easuCache, easu);

        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_vs, nullptr, 0);
        m_context->RSSetState(m_rasterizer);
        m_context->OMSetDepthStencilState(m_depthState, 0);
        m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        m_context->PSSetSamplers(0, 1, &m_sampler);

        if (sharpen)
        {
            uint32_t rcas[16] = {};
            ComputeRcasConstants(rcas, sharpness);
            UpdateConstants(m_rcasConstants, m_rcasCache, rcas);

            Draw(m_intermediateRtv, srcSrv, m_easuPs, m_easuConstants, dstW, dstH);
            Draw(dstRtv, m_intermediateSrv, m_rcasPs, m_rcasConstants, dstW, dstH);
        }
        else
        {
            Draw(dstRtv, srcSrv, m_easuPs, m_easuConstants, dstW, dstH);
        }

        // Leave nothing bound in our state object: bound views would hold references to the
        // swapchain buffer and make the next ResizeBuffers fail.
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
    }

    if (m_state)
    {
        m_context1->SwapDeviceContextState(gameState, nullptr);
        SafeRelease(gameState);
    }
    else
    {
        backup.Restore(m_context);
    }
    SafeRelease(dstRtv);
    return true;
}

} // namespace afsr
