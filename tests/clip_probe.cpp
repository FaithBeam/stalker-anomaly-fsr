// Diagnostic for the Wine/macOS mouse drift: replays the engine's mid-game device reset window
// sequence and prints where Win32 thinks the cursor is after SetCursorPos probes. Run alongside a
// macOS-side cursor logger to compare with where the real cursor ended up.
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstdio>

static long long NowMs()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (long long)((t.QuadPart - 116444736000000000ULL) / 10000ULL);
}

static void Pump(int ms)
{
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageW(&msg);
        Sleep(5);
    }
}

static void Displays(const char* phase)
{
    std::printf("%s virtual=(%d,%d %dx%d)\n", phase, GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    DISPLAY_DEVICEW dd = {sizeof(dd)};
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i)
    {
        DEVMODEW cur = {}, reg = {};
        cur.dmSize = reg.dmSize = sizeof(DEVMODEW);
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &cur);
        EnumDisplaySettingsW(dd.DeviceName, ENUM_REGISTRY_SETTINGS, &reg);
        std::printf("%s %ls cur=%lux%lu@(%ld,%ld) reg=%lux%lu\n", phase, dd.DeviceName, cur.dmPelsWidth, cur.dmPelsHeight,
                    cur.dmPosition.x, cur.dmPosition.y, reg.dmPelsWidth, reg.dmPelsHeight);
    }
}

static void Probe(const char* phase, HWND hwnd)
{
    Displays(phase);
    RECT clip;
    GetClipCursor(&clip);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    std::printf("%s window=(%ld,%ld)-(%ld,%ld) clip=(%ld,%ld)-(%ld,%ld)\n", phase, wr.left, wr.top, wr.right, wr.bottom,
                clip.left, clip.top, clip.right, clip.bottom);
    const POINT targets[] = {{1920, 1080}, {1920, -500}, {1920, 5000}, {-500, 1080}, {6000, 1080}};
    for (POINT t : targets)
    {
        SetCursorPos(t.x, t.y);
        Pump(250);
        POINT p;
        GetCursorPos(&p);
        std::printf("%s t=%lld set=(%ld,%ld) win32=(%ld,%ld)\n", phase, NowMs(), t.x, t.y, p.x, p.y);
        std::fflush(stdout);
    }
}

static void ClipToClient(HWND hwnd)
{
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);
}

int main()
{
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT m = mi.rcMonitor;
    std::printf("monitor=(%ld,%ld)-(%ld,%ld) virtual=(%d,%d %dx%d)\n", m.left, m.top, m.right, m.bottom,
                GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ClipProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"clip probe", WS_POPUP | WS_VISIBLE, m.left, m.top,
                                m.right - m.left, m.bottom - m.top, nullptr, nullptr, wc.hInstance, nullptr);
    SetForegroundWindow(hwnd);
    Pump(1500);

    // Like a fresh session: window covers the monitor, game clips the cursor to it.
    ClipToClient(hwnd);
    Pump(300);
    Probe("A_startup", hwnd);

    // Swapchain like the engine's (flip model, game resolution, ALLOW_MODE_SWITCH).
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, nullptr);
    IDXGIFactory2* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 1280; sd.Height = 720; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsd = {}; fsd.RefreshRate = {60, 1}; fsd.Windowed = TRUE;
    IDXGISwapChain1* sc = nullptr;
    HRESULT hr = factory->CreateSwapChainForHwnd(dev, hwnd, &sd, &fsd, nullptr, &sc);
    std::printf("swapchain hr=0x%08lx\n", (unsigned long)hr);
    sc->Present(0, 0);
    // Startup window props, as after CHW::CreateDevice.
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    ClipToClient(hwnd);
    Pump(500);
    Probe("A2_with_swapchain", hwnd);

    // Mid-game reset: CRenderDevice::Reset + CHW::Reset (borderless).
    ShowCursor(TRUE);
    ClipCursor(nullptr);
    hr = sc->SetFullscreenState(FALSE, nullptr);
    std::printf("SetFullscreenState hr=0x%08lx\n", (unsigned long)hr);
    DXGI_MODE_DESC mode = {1280, 720, {60, 1}, DXGI_FORMAT_R8G8B8A8_UNORM};
    hr = sc->ResizeTarget(&mode);
    std::printf("ResizeTarget hr=0x%08lx\n", (unsigned long)hr);
    Pump(200);
    RECT shrunk; GetClientRect(hwnd, &shrunk);
    std::printf("client after ResizeTarget %ldx%ld\n", shrunk.right, shrunk.bottom);
    hr = sc->ResizeBuffers(2, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    std::printf("ResizeBuffers hr=0x%08lx\n", (unsigned long)hr);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP);
    SetWindowPos(hwnd, HWND_NOTOPMOST, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_SHOWWINDOW | SWP_NOCOPYBITS | SWP_DRAWFRAME);
    ShowCursor(FALSE);
    SetForegroundWindow(hwnd);
    ClipToClient(hwnd);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    ShowCursor(FALSE);
    ClipToClient(hwnd);
    sc->Present(0, 0);
    Pump(500);
    Probe("B_after_reset", hwnd);

    // Same reset without the window shrink.
    ClipCursor(nullptr);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_VISIBLE | WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED);
    ClipToClient(hwnd);
    Pump(300);
    Probe("C_noshrink", hwnd);

    ClipCursor(nullptr);
    SetCursorPos(1920, 1080);
    DestroyWindow(hwnd);
    return 0;
}
