// End-to-end self test for anomaly_fsr.dll, mimicking the xray-monolith DX11 renderer:
// flip-model swapchain from CreateSwapChainForHwnd, a device reset (ResizeBuffers + GetBuffer),
// then Present. Checks the upscaled output, the game-visible swapchain description, and that the
// game's pipeline state survives the FSR passes untouched.
//
//   fsr_selftest.exe path\to\anomaly_fsr.dll
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_failures = 0;
std::vector<WORD> g_activations; // LOWORD(wParam) of every WM_ACTIVATE the window received

LRESULT CALLBACK TestWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATE)
        g_activations.push_back(LOWORD(wParam));
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Check(bool ok, const char* what)
{
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++g_failures;
}

template <typename Fn>
Fn GetProc(HMODULE module, const char* name)
{
    return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

using PFN_GetBuffer = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);

PFN_GetBuffer g_realGetBuffer = nullptr;
PFN_Present g_realPresent = nullptr;
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;

// What the real back buffer looked like right before the real Present.
struct Capture
{
    UINT width = 0;
    UINT height = 0;
    std::vector<uint32_t> pixels;
    uint32_t At(UINT x, UINT y) const { return pixels.empty() ? 0 : pixels[y * width + x]; }
} g_capture;

// Sits between anomaly_fsr's Present hook and DXGI's Present.
HRESULT STDMETHODCALLTYPE InspectPresent(IDXGISwapChain* self, UINT sync, UINT flags)
{
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_realGetBuffer(self, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))))
    {
        D3D11_TEXTURE2D_DESC desc;
        back->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ID3D11Texture2D* staging = nullptr;
        if (SUCCEEDED(g_device->CreateTexture2D(&desc, nullptr, &staging)))
        {
            g_context->CopyResource(staging, back);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
            {
                g_capture.width = desc.Width;
                g_capture.height = desc.Height;
                g_capture.pixels.resize(size_t(desc.Width) * desc.Height);
                for (UINT y = 0; y < desc.Height; ++y)
                    std::memcpy(&g_capture.pixels[size_t(y) * desc.Width],
                                static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                                desc.Width * 4);
                g_context->Unmap(staging, 0);
            }
            staging->Release();
        }
        back->Release();
    }
    return g_realPresent(self, sync, flags);
}

void PatchSlot(void** vtable, int slot, void* hook)
{
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

int Channel(uint32_t rgba, int shift) { return (rgba >> shift) & 0xFF; }

bool IsRed(uint32_t c) { return Channel(c, 0) > 200 && Channel(c, 8) < 40 && Channel(c, 16) < 40; }
bool IsBlue(uint32_t c) { return Channel(c, 0) < 40 && Channel(c, 8) < 40 && Channel(c, 16) > 200; }

void PrintLog(HMODULE dll)
{
    auto pollLog = GetProc<const char* (*)()>(dll, "afsr_poll_log");
    const char* text = pollLog();
    if (*text)
        std::printf("--- dll log ---\n%s---------------\n", text);
}

// Game-side pipeline state that must survive the FSR passes.
struct Sentinel
{
    D3D11_VIEWPORT viewport;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView* rtv;
    ID3D11BlendState* blend;
    ID3D11SamplerState* sampler;
    D3D11_PRIMITIVE_TOPOLOGY topology;
};

void Apply(const Sentinel& s)
{
    g_context->RSSetViewports(1, &s.viewport);
    g_context->OMSetRenderTargets(1, &s.rtv, nullptr);
    g_context->PSSetShaderResources(0, 1, &s.srv);
    g_context->PSSetSamplers(0, 1, &s.sampler);
    const float factor[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    g_context->OMSetBlendState(s.blend, factor, 0x0F0F0F0F);
    g_context->IASetPrimitiveTopology(s.topology);
}

bool Matches(const Sentinel& s)
{
    UINT count = 1;
    D3D11_VIEWPORT vp = {};
    g_context->RSGetViewports(&count, &vp);
    ID3D11RenderTargetView* rtv = nullptr;
    g_context->OMGetRenderTargets(1, &rtv, nullptr);
    ID3D11ShaderResourceView* srv = nullptr;
    g_context->PSGetShaderResources(0, 1, &srv);
    ID3D11SamplerState* sampler = nullptr;
    g_context->PSGetSamplers(0, 1, &sampler);
    ID3D11BlendState* blend = nullptr;
    float factor[4];
    UINT mask = 0;
    g_context->OMGetBlendState(&blend, factor, &mask);
    D3D11_PRIMITIVE_TOPOLOGY topology;
    g_context->IAGetPrimitiveTopology(&topology);

    bool ok = count == 1 && std::memcmp(&vp, &s.viewport, sizeof(vp)) == 0 && rtv == s.rtv && srv == s.srv &&
              sampler == s.sampler && blend == s.blend && mask == 0x0F0F0F0F && factor[2] == 0.75f &&
              topology == s.topology;
    for (IUnknown* p : {static_cast<IUnknown*>(rtv), static_cast<IUnknown*>(srv), static_cast<IUnknown*>(sampler),
                        static_cast<IUnknown*>(blend)})
        if (p)
            p->Release();
    return ok;
}

} // namespace

int main(int argc, char** argv)
{
    const char* dllPath = argc > 1 ? argv[1] : "anomaly_fsr.dll";
    const UINT kGameW = 640, kGameH = 360, kWindowW = 1280, kWindowH = 720;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FsrSelfTest";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"anomaly_fsr self test", WS_POPUP | WS_VISIBLE, 0, 0,
                                  kWindowW, kWindowH, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(window);
    PumpMessages();
    RECT client;
    GetClientRect(window, &client);
    std::printf("window client %ldx%ld\n", client.right, client.bottom);

    // Device + swapchain exactly like CHW::CreateDevice (USE_DX11) in xray-monolith.
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   &level, 1, D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    Check(SUCCEEDED(hr), "D3D11CreateDevice");
    if (FAILED(hr))
        return 1;

    IDXGIFactory2* factory = nullptr;
    auto createFactory2 = GetProc<HRESULT(WINAPI*)(UINT, REFIID, void**)>(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory2");
    hr = createFactory2(0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    Check(SUCCEEDED(hr), "CreateDXGIFactory2");

    IDXGIAdapter* adapter = nullptr;
    factory->EnumAdapters(0, &adapter);
    DXGI_ADAPTER_DESC adapterDesc = {};
    adapter->GetDesc(&adapterDesc);
    adapter->Release();
    std::printf("adapter: %ls\n", adapterDesc.Description);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = kGameW;
    sd.Height = kGameH;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fs = {};
    fs.RefreshRate = {60, 1};
    fs.Windowed = TRUE;
    IDXGISwapChain1* swapChain = nullptr;
    hr = factory->CreateSwapChainForHwnd(g_device, window, &sd, &fs, nullptr, &swapChain);
    Check(SUCCEEDED(hr), "CreateSwapChainForHwnd");
    if (FAILED(hr))
        return 1;

    // The game holds a view of the real buffer before our DLL is loaded (as when Lua loads it).
    ID3D11Texture2D* realBefore = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&realBefore));
    ID3D11RenderTargetView* gameRtv = nullptr;
    g_device->CreateRenderTargetView(realBefore, nullptr, &gameRtv);
    realBefore->Release();

    // Install the inspector below anomaly_fsr's hook.
    void** vtable = *reinterpret_cast<void***>(swapChain);
    g_realGetBuffer = reinterpret_cast<PFN_GetBuffer>(vtable[9]);
    g_realPresent = reinterpret_cast<PFN_Present>(vtable[8]);
    PatchSlot(vtable, 8, reinterpret_cast<void*>(&InspectPresent));

    HMODULE dll = LoadLibraryA(dllPath);
    Check(dll != nullptr, "LoadLibrary(anomaly_fsr.dll)");
    if (!dll)
        return 1;
    auto init = GetProc<int (*)()>(dll, "afsr_init");
    auto state = GetProc<int (*)()>(dll, "afsr_state");
    auto status = GetProc<const char* (*)()>(dll, "afsr_status");
    auto setEnabled = GetProc<void (*)(int)>(dll, "afsr_set_enabled");
    auto setSharpening = GetProc<void (*)(int)>(dll, "afsr_set_sharpening");

    Check(init() == 1, "afsr_init");
    Check(state() == 1, "state = waiting for reset");
    PrintLog(dll);

    // Frames before the reset pass straight through.
    swapChain->Present(0, 0);
    Check(g_capture.width == kGameW, "pre-reset frame is untouched");

    // vid_restart -> CHW::Reset: release views, ResizeBuffers, GetBuffer + CreateRenderTargetView.
    gameRtv->Release();
    hr = swapChain->ResizeBuffers(2, kGameW, kGameH, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    Check(SUCCEEDED(hr), "ResizeBuffers (game reset)");
    PrintLog(dll);

    ID3D11Texture2D* gameBuffer = nullptr;
    swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&gameBuffer));
    ID3D11Texture2D* realBuffer = nullptr;
    g_realGetBuffer(swapChain, 0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&realBuffer));
    Check(gameBuffer && gameBuffer != realBuffer, "GetBuffer returns the FSR-managed game buffer");
    realBuffer->Release();

    D3D11_TEXTURE2D_DESC gameDesc = {};
    gameBuffer->GetDesc(&gameDesc);
    Check(gameDesc.Width == kGameW && gameDesc.Height == kGameH, "game buffer is 640x360");

    g_device->CreateRenderTargetView(gameBuffer, nullptr, &gameRtv);

    // Test image: red left half, blue right half.
    std::vector<uint32_t> image(kGameW * kGameH);
    for (UINT y = 0; y < kGameH; ++y)
        for (UINT x = 0; x < kGameW; ++x)
            image[y * kGameW + x] = x < kGameW / 2 ? 0xFF0000FF : 0xFFFF0000;
    g_context->UpdateSubresource(gameBuffer, 0, nullptr, image.data(), kGameW * 4, 0);

    // Game state the FSR passes must not disturb.
    D3D11_TEXTURE2D_DESC dummyDesc = {4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                                      D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ID3D11Texture2D* dummy = nullptr;
    g_device->CreateTexture2D(&dummyDesc, nullptr, &dummy);
    Sentinel sentinel = {};
    sentinel.viewport = {1, 2, 300, 200, 0.1f, 0.9f};
    g_device->CreateShaderResourceView(dummy, nullptr, &sentinel.srv);
    sentinel.rtv = gameRtv;
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&blendDesc, &sentinel.blend);
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&samplerDesc, &sentinel.sampler);
    sentinel.topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;

    for (int frame = 0; frame < 3; ++frame)
    {
        Apply(sentinel);
        swapChain->Present(0, 0);
        if (frame == 2)
            Check(Matches(sentinel), "game pipeline state preserved across Present");
    }
    PrintLog(dll);
    std::printf("status: %s\n", status());

    // Under Wine the DLL resyncs the mouse 1.5 s after the reset: it re-applies the cursor clip and
    // replays a deactivate/activate so DirectInput re-acquires.
    const bool wine = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    PumpMessages();
    g_activations.clear();
    RECT clipBefore = {};
    GetClipCursor(&clipBefore);
    Sleep(1600);
    swapChain->Present(0, 0);
    PumpMessages();
    RECT clipAfter = {};
    GetClipCursor(&clipAfter);
    PrintLog(dll);
    Check(EqualRect(&clipBefore, &clipAfter) != FALSE, "cursor clip unchanged by the mouse resync");
    if (!wine)
        Check(g_activations.empty(), "no input re-acquire on Windows");
    else if (GetForegroundWindow() != window)
        std::printf("[SKIP] input re-acquire (test window is not in the foreground)\n");
    else
        Check(g_activations.size() == 2 && g_activations[0] == WA_INACTIVE && g_activations[1] == WA_ACTIVE,
              "Wine: deactivate/activate posted after the reset");

    Check(state() == 3, "state = upscaling");
    Check(g_capture.width == kWindowW && g_capture.height == kWindowH, "real back buffer is 1280x720");
    Check(IsRed(g_capture.At(100, 360)) && IsRed(g_capture.At(600, 100)), "left half upscaled red");
    Check(IsBlue(g_capture.At(1180, 360)) && IsBlue(g_capture.At(700, 600)), "right half upscaled blue");

    DXGI_SWAP_CHAIN_DESC visible = {};
    swapChain->GetDesc(&visible);
    Check(visible.BufferDesc.Width == kGameW && visible.BufferDesc.Height == kGameH, "GetDesc reports game size");
    DXGI_SWAP_CHAIN_DESC1 visible1 = {};
    swapChain->GetDesc1(&visible1);
    Check(visible1.Width == kGameW && visible1.Height == kGameH, "GetDesc1 reports game size");

    // EASU only.
    setSharpening(0);
    swapChain->Present(0, 0);
    Check(IsRed(g_capture.At(100, 360)) && IsBlue(g_capture.At(1180, 360)), "EASU without RCAS");
    setSharpening(1);

    // FSR disabled: the real buffer goes back to the game size (plain copy).
    setEnabled(0);
    swapChain->Present(0, 0);
    Check(g_capture.width == kGameW && g_capture.height == kGameH, "disabled: real buffer back to 640x360");
    Check(IsRed(g_capture.At(10, 10)) && IsBlue(g_capture.At(630, 350)), "disabled: image copied");
    setEnabled(1);
    swapChain->Present(0, 0);
    Check(g_capture.width == kWindowW, "re-enabled: upscaling again");

    // Another game reset at native resolution: no upscaling.
    gameRtv->Release();
    gameBuffer->Release();
    hr = swapChain->ResizeBuffers(2, kWindowW, kWindowH, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    Check(SUCCEEDED(hr), "ResizeBuffers to native");
    swapChain->Present(0, 0);
    Check(state() == 2 && g_capture.width == kWindowW, "native resolution: copy only");
    PrintLog(dll);
    std::printf("status: %s\n", status());

    swapChain->Release();

    // Borderless reset: the engine calls ResizeTarget with the game mode on a window that covers the
    // monitor. Under Wine the DLL leaves that window alone; elsewhere it goes through unchanged.
    {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT m = mi.rcMonitor;
        HWND full = CreateWindowExW(0, wc.lpszClassName, L"anomaly_fsr borderless test", WS_POPUP | WS_VISIBLE, m.left,
                                    m.top, m.right - m.left, m.bottom - m.top, nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowPos(full, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
        PumpMessages();
        IDXGISwapChain1* borderless = nullptr;
        hr = factory->CreateSwapChainForHwnd(g_device, full, &sd, &fs, nullptr, &borderless);
        Check(SUCCEEDED(hr), "borderless: CreateSwapChainForHwnd");
        RECT before = {}, after = {};
        GetWindowRect(full, &before);
        if (SUCCEEDED(hr))
        {
            DXGI_MODE_DESC mode = {kGameW, kGameH, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
            hr = borderless->ResizeTarget(&mode);
            PumpMessages();
            GetWindowRect(full, &after);
            PrintLog(dll);
            Check(SUCCEEDED(hr), "borderless: ResizeTarget");
            if (!EqualRect(&before, &m))
                std::printf("[SKIP] borderless window does not cover the monitor (%ld,%ld)-(%ld,%ld)\n", before.left,
                            before.top, before.right, before.bottom);
            else if (wine)
                Check(EqualRect(&before, &after) != FALSE, "Wine: borderless window not shrunk by ResizeTarget");
            else
                Check(after.right - after.left == static_cast<LONG>(kGameW), "Windows: ResizeTarget resizes");
            borderless->Release();
        }
        DestroyWindow(full);
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
