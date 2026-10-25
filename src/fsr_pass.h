#pragma once

#include "common.h"

namespace afsr {

// Owns the D3D11 objects needed to run FSR 1 (EASU + optional RCAS) on the game's device.
// Rendering happens inside a private ID3DDeviceContextState when the driver supports it, otherwise
// the touched state is saved and restored, so the game's pipeline state (which X-Ray caches
// aggressively) is exactly as it was when we hand the context back.
class FsrPass
{
public:
    FsrPass() = default;
    ~FsrPass() { Shutdown(); }
    FsrPass(const FsrPass&) = delete;
    FsrPass& operator=(const FsrPass&) = delete;

    bool Init(ID3D11Device* device);
    void Shutdown();
    bool IsReady() const { return m_vs != nullptr; }
    ID3D11Device* Device() const { return m_device; }

    // Writes the game image (src, srcW x srcH) into the swapchain buffer (dst, dstW x dstH).
    // Equal sizes are a plain copy; otherwise EASU upscales and RCAS optionally sharpens.
    bool Compose(ID3D11Texture2D* src, ID3D11ShaderResourceView* srcSrv, UINT srcW, UINT srcH,
                 ID3D11Texture2D* dst, UINT dstW, UINT dstH, DXGI_FORMAT format,
                 bool sharpen, float sharpness);

    // Drops the display-resolution intermediate (e.g. before the swapchain is resized).
    void ReleaseIntermediate();

    // Flushes the immediate context so released views are destroyed before a swapchain resize.
    void Flush()
    {
        if (m_context)
            m_context->Flush();
    }

private:
    bool CompileShaders();
    bool EnsureIntermediate(UINT w, UINT h, DXGI_FORMAT format);
    void UpdateConstants(ID3D11Buffer* buffer, uint32_t* cache, const uint32_t* values);
    void Draw(ID3D11RenderTargetView* rtv, ID3D11ShaderResourceView* srv, ID3D11PixelShader* ps,
              ID3D11Buffer* constants, UINT w, UINT h);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    ID3D11DeviceContext1* m_context1 = nullptr; // only with m_state
    ID3DDeviceContextState* m_state = nullptr;  // optional; null = manual state backup

    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_easuPs = nullptr;
    ID3D11PixelShader* m_rcasPs = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    ID3D11RasterizerState* m_rasterizer = nullptr;
    ID3D11DepthStencilState* m_depthState = nullptr;
    ID3D11Buffer* m_easuConstants = nullptr;
    ID3D11Buffer* m_rcasConstants = nullptr;
    uint32_t m_easuCache[16] = {};
    uint32_t m_rcasCache[16] = {};

    ID3D11Texture2D* m_intermediate = nullptr;
    ID3D11RenderTargetView* m_intermediateRtv = nullptr;
    ID3D11ShaderResourceView* m_intermediateSrv = nullptr;
    UINT m_intermediateW = 0;
    UINT m_intermediateH = 0;
    DXGI_FORMAT m_intermediateFormat = DXGI_FORMAT_UNKNOWN;
};

} // namespace afsr
