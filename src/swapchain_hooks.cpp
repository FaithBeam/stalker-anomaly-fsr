// Swapchain virtualization.
//
// Once the hooks are in, every time the game (re)sizes its swapchain we hand it a private
// "game buffer" texture at the size it asked for, while the real swapchain buffers are kept at
// the size of the window (borderless) or the native display mode (exclusive fullscreen).
// On Present the game buffer is upscaled into the real buffer with FSR 1, or simply copied when
// the sizes already match. The game never sees the real buffers, so nothing else in the engine
// has to change.
#include "swapchain_hooks.h"

#include "common.h"
#include "fsr_pass.h"
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <vector>

namespace afsr {

Settings& GetSettings()
{
    static Settings settings;
    return settings;
}

namespace {

// Vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-28).
enum Slot : int
{
    kRelease = 2,
    kPresent = 8,
    kGetBuffer = 9,
    kGetDesc = 12,
    kResizeBuffers = 13,
    kResizeTarget = 14,
    kGetDesc1 = 18,
    kPresent1 = 22,
    kSlotCount = 23,
};

using PFN_Release = ULONG(STDMETHODCALLTYPE*)(IUnknown*);
using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_GetBuffer = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using PFN_GetDesc = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using PFN_ResizeTarget = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, const DXGI_MODE_DESC*);
using PFN_GetDesc1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using PFN_CreateDevice = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*,
                                          UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

struct HookedVTable
{
    void** vtable = nullptr;
    void* original[kSlotCount] = {};
};

constexpr int kMaxVTables = 4;
HookedVTable g_vtables[kMaxVTables];
std::atomic<int> g_vtableCount{0};
Lock g_installLock;

const HookedVTable& VTableFor(void* object)
{
    void** vtable = *static_cast<void***>(object);
    const int count = g_vtableCount.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
    {
        if (g_vtables[i].vtable == vtable)
            return g_vtables[i];
    }
    // Someone swapped the object's vtable pointer for a copy of ours; the first entry is the
    // flip-model swapchain class, which is what the game uses.
    return g_vtables[0];
}

template <typename Fn>
Fn Original(void* object, Slot slot)
{
    return reinterpret_cast<Fn>(VTableFor(object).original[slot]);
}

// While we compose and present a frame, calls made on the same thread (by us, or by overlays that
// hook the real Present) must see the real swapchain buffers.
std::atomic<DWORD> g_bypassThread{0};

bool IsBypassed()
{
    return g_bypassThread.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

class BypassScope
{
public:
    BypassScope() : m_previous(g_bypassThread.exchange(GetCurrentThreadId())) {}
    ~BypassScope() { g_bypassThread.store(m_previous); }

private:
    DWORD m_previous;
};

struct SwapChainState
{
    IDXGISwapChain* swapChain = nullptr; // not referenced; used as a key only
    HWND window = nullptr;
    FsrPass pass;

    bool virtualized = false;
    bool loggedUnsupported = false;
    ID3D11Texture2D* gameBuffer = nullptr;
    ID3D11ShaderResourceView* gameSrv = nullptr;
    UINT gameW = 0;
    UINT gameH = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT flags = 0;

    UINT realW = 0;
    UINT realH = 0;
    UINT failedW = 0;
    UINT failedH = 0;
    UINT announcedW = 0; // last output size written to the log
    UINT announcedH = 0;
    ULONGLONG mouseResyncAt = 0; // Wine: tick count at which to resync mouse input after a reset
    int mouseReports = 0;        // Wine: cursor/clip diagnostics still to log after a reset
    ULONGLONG nextMouseReport = 0;

    void ReleaseGameBuffer()
    {
        SafeRelease(gameSrv);
        SafeRelease(gameBuffer);
    }

    ~SwapChainState() { ReleaseGameBuffer(); }
};

Lock g_stateLock;
std::vector<std::unique_ptr<SwapChainState>> g_states;

std::atomic<int> g_hookState{static_cast<int>(HookState::NotInstalled)};
bool g_isWine = false;
char g_status[256] = "not initialized";

SwapChainState* FindState(void* swapChain)
{
    for (auto& state : g_states)
    {
        if (state->swapChain == swapChain)
            return state.get();
    }
    return nullptr;
}

void SetStatus(HookState state, const char* fmt, ...)
{
    g_hookState.store(static_cast<int>(state));
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
    va_end(args);
}

HRESULT OrigGetDesc(IDXGISwapChain* swapChain, DXGI_SWAP_CHAIN_DESC* desc)
{
    return Original<PFN_GetDesc>(swapChain, kGetDesc)(swapChain, desc);
}

// Called after the game resized the swapchain: give it a private buffer of the requested size.
void Virtualize(SwapChainState& state, IDXGISwapChain* swapChain, UINT flags)
{
    state.virtualized = false;
    state.ReleaseGameBuffer();

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(OrigGetDesc(swapChain, &desc)))
        return;

    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))))
    {
        if (!state.loggedUnsupported)
            Log("The game is not using Direct3D 11. FSR needs the DX11 renderer (AnomalyDX11.exe / AnomalyDX11AVX.exe).");
        state.loggedUnsupported = true;
        SetStatus(HookState::WaitingForReset, "unsupported renderer (DX11 required)");
        return;
    }

    bool ready = state.pass.Device() == device || state.pass.Init(device);
    if (ready)
    {
        D3D11_TEXTURE2D_DESC tex = {};
        tex.Width = desc.BufferDesc.Width;
        tex.Height = desc.BufferDesc.Height;
        tex.MipLevels = 1;
        tex.ArraySize = 1;
        tex.Format = desc.BufferDesc.Format;
        tex.SampleDesc.Count = 1;
        tex.Usage = D3D11_USAGE_DEFAULT;
        tex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        ready = SUCCEEDED(device->CreateTexture2D(&tex, nullptr, &state.gameBuffer)) &&
                SUCCEEDED(device->CreateShaderResourceView(state.gameBuffer, nullptr, &state.gameSrv));
        if (!ready)
        {
            Log("Could not create the %ux%u game buffer (format %d); FSR disabled.", tex.Width, tex.Height,
                static_cast<int>(tex.Format));
            state.ReleaseGameBuffer();
        }
    }
    device->Release();

    if (!ready)
    {
        SetStatus(HookState::WaitingForReset, "FSR could not initialize (see log)");
        return;
    }

    state.virtualized = true;
    state.window = desc.OutputWindow;
    state.gameW = desc.BufferDesc.Width;
    state.gameH = desc.BufferDesc.Height;
    state.format = desc.BufferDesc.Format;
    state.flags = flags;
    state.realW = state.gameW;
    state.realH = state.gameH;
    state.failedW = state.failedH = 0;
    state.announcedW = state.announcedH = 0;
    if (g_isWine)
    {
        // Let the engine finish the reset and the Mac window settle (and stay clear of win32u's
        // one second clip-reset window) before touching the cursor clip.
        state.mouseResyncAt = GetTickCount64() + 1500;
        state.mouseReports = 3;
        state.nextMouseReport = GetTickCount64() + 500;
    }
    SetStatus(HookState::Native, "%ux%u, waiting for first frame", state.gameW, state.gameH);
}

HWND RootWindow(HWND window)
{
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

// A borderless game window: no frame, already covering its whole monitor.
bool CoversItsMonitor(HWND window)
{
    HWND root = RootWindow(window);
    if (!root || (GetWindowLongPtrW(root, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)))
        return false;
    RECT rect = {};
    MONITORINFO info = {sizeof(info)};
    if (!GetWindowRect(root, &rect) || !GetMonitorInfoW(MonitorFromWindow(root, MONITOR_DEFAULTTONEAREST), &info))
        return false;
    return EqualRect(&rect, &info.rcMonitor) != FALSE;
}

void LogMouseState(const char* when, HWND window)
{
    HWND root = RootWindow(window);
    RECT rect = {}, client = {}, clip = {};
    POINT cursor = {};
    if (root)
    {
        GetWindowRect(root, &rect);
        GetClientRect(root, &client);
        MapWindowPoints(root, nullptr, reinterpret_cast<POINT*>(&client), 2);
    }
    GetClipCursor(&clip);
    GetCursorPos(&cursor);
    Log("Wine mouse %s: window (%ld,%ld)-(%ld,%ld) client (%ld,%ld)-(%ld,%ld) clip (%ld,%ld)-(%ld,%ld) "
        "cursor (%ld,%ld)%s",
        when, rect.left, rect.top, rect.right, rect.bottom, client.left, client.top, client.right, client.bottom,
        clip.left, clip.top, clip.right, clip.bottom, cursor.x, cursor.y,
        root && GetForegroundWindow() == root ? "" : " (not foreground)");
}

// Under Wine on macOS the cursor clip lives on the Cocoa side as a confinement rectangle tied to the
// window frame, and the Win32 cursor position is tracked separately from the Mac one. A video reset
// in the middle of the game re-applies the clip around window changes, after which the two can stay
// out of step and every mouse movement turns into a constant drift (an app switch fixes it).
// Rebuild the clip from scratch, then replay a deactivate/activate so the engine unacquires and
// re-acquires DirectInput, which re-centers the cursor and re-applies its own exclusive clip.
// Posted (not sent) so it all runs on the window thread, in order.
void ResyncMouse(HWND window)
{
    HWND root = RootWindow(window);
    if (!root || GetForegroundWindow() != root)
    {
        Log("Wine: game window is not in the foreground; skipping the mouse resync.");
        return;
    }
    RECT clip = {};
    const bool clipped = GetClipCursor(&clip) != FALSE;
    PostMessageW(root, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
    ClipCursor(nullptr);
    if (clipped)
        ClipCursor(&clip);
    PostMessageW(root, WM_ACTIVATE, MAKEWPARAM(WA_ACTIVE, 0), 0);
    Log("Wine: resynced the mouse cursor clip and re-acquired input after the video reset.");
}

// Picks the real buffer size for this frame: the window's client area when it is larger than the
// game resolution and FSR is enabled, otherwise the game resolution (plain copy).
void ChooseOutputSize(const SwapChainState& state, UINT& w, UINT& h)
{
    w = state.gameW;
    h = state.gameH;

    RECT client = {};
    if (!state.window || !GetClientRect(state.window, &client) || client.right <= 0 || client.bottom <= 0)
    {
        // Minimized: keep whatever we have to avoid resizing back and forth.
        w = state.realW;
        h = state.realH;
        return;
    }

    if (!GetSettings().enabled.load())
        return;

    const UINT clientW = static_cast<UINT>(client.right);
    const UINT clientH = static_cast<UINT>(client.bottom);
    if (clientW >= state.gameW && clientH >= state.gameH && (clientW > state.gameW || clientH > state.gameH))
    {
        w = clientW;
        h = clientH;
    }
}

// Returns true if the frame was composed (i.e. the swapchain is virtualized).
bool ComposeFrame(IDXGISwapChain* swapChain)
{
    LockGuard guard(g_stateLock);
    SwapChainState* state = FindState(swapChain);
    if (!state || !state->virtualized)
        return false;

    UINT targetW, targetH;
    ChooseOutputSize(*state, targetW, targetH);

    const bool retryingFailedSize = targetW == state->failedW && targetH == state->failedH;
    if ((targetW != state->realW || targetH != state->realH) && !retryingFailedSize)
    {
        state->pass.ReleaseIntermediate();
        state->pass.Flush();
        HRESULT hr = Original<PFN_ResizeBuffers>(swapChain, kResizeBuffers)(swapChain, 0, targetW, targetH,
                                                                            DXGI_FORMAT_UNKNOWN, state->flags);
        if (FAILED(hr))
        {
            Log("Resizing the swapchain to %ux%u failed (0x%08lX); staying at %ux%u.", targetW, targetH,
                static_cast<unsigned long>(hr), state->realW, state->realH);
            state->failedW = targetW;
            state->failedH = targetH;
        }
        else
        {
            state->failedW = state->failedH = 0;
        }
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(Original<PFN_GetBuffer>(swapChain, kGetBuffer)(swapChain, 0, __uuidof(ID3D11Texture2D),
                                                              reinterpret_cast<void**>(&backBuffer))))
        return true;

    D3D11_TEXTURE2D_DESC backDesc = {};
    backBuffer->GetDesc(&backDesc);

    state->realW = backDesc.Width;
    state->realH = backDesc.Height;

    const Settings& settings = GetSettings();
    const bool upscaling = state->realW != state->gameW || state->realH != state->gameH;
    const bool sharpen = settings.sharpening.load();
    const float sharpness = settings.sharpness.load();

    if (state->announcedW != state->realW || state->announcedH != state->realH)
    {
        state->announcedW = state->realW;
        state->announcedH = state->realH;
        if (upscaling)
            Log("Upscaling %ux%u -> %ux%u with FSR 1.", state->gameW, state->gameH, state->realW, state->realH);
        else
            Log("Output %ux%u matches the game resolution; no upscaling.", state->realW, state->realH);
    }

    if (upscaling)
    {
        if (sharpen)
            SetStatus(HookState::Upscaling, "%ux%u -> %ux%u, FSR 1 EASU + RCAS (sharpness %.2f)", state->gameW,
                      state->gameH, state->realW, state->realH, sharpness);
        else
            SetStatus(HookState::Upscaling, "%ux%u -> %ux%u, FSR 1 EASU (no sharpening)", state->gameW,
                      state->gameH, state->realW, state->realH);
    }
    else
    {
        SetStatus(HookState::Native, "%ux%u, no upscaling needed", state->realW, state->realH);
    }

    state->pass.Compose(state->gameBuffer, state->gameSrv, state->gameW, state->gameH, backBuffer, state->realW,
                        state->realH, state->format, sharpen, sharpness);
    backBuffer->Release();

    if (state->mouseReports > 0 || state->mouseResyncAt)
    {
        const ULONGLONG now = GetTickCount64();
        if (state->mouseResyncAt && now >= state->mouseResyncAt)
        {
            state->mouseResyncAt = 0;
            ResyncMouse(state->window);
        }
        if (state->mouseReports > 0 && now >= state->nextMouseReport)
        {
            --state->mouseReports;
            state->nextMouseReport = now + 1500;
            LogMouseState(state->mouseResyncAt ? "before resync" : "after resync", state->window);
        }
    }
    return true;
}

bool GetDesktopMode(IDXGIOutput* output, UINT& w, UINT& h)
{
    DXGI_OUTPUT_DESC desc = {};
    if (FAILED(output->GetDesc(&desc)))
        return false;

    // The registry mode is the desktop resolution, even while exclusive fullscreen has the monitor
    // switched to a lower mode.
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(desc.DeviceName, ENUM_REGISTRY_SETTINGS, &mode) && mode.dmPelsWidth && mode.dmPelsHeight)
    {
        w = mode.dmPelsWidth;
        h = mode.dmPelsHeight;
        return true;
    }

    w = static_cast<UINT>(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
    h = static_cast<UINT>(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
    return w && h;
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

ULONG STDMETHODCALLTYPE Hook_Release(IUnknown* self)
{
    ULONG refs = Original<PFN_Release>(self, kRelease)(self);
    if (refs == 0 && !IsBypassed())
    {
        LockGuard guard(g_stateLock);
        for (auto it = g_states.begin(); it != g_states.end(); ++it)
        {
            if ((*it)->swapChain == static_cast<void*>(self))
            {
                g_states.erase(it);
                SetStatus(HookState::WaitingForReset, "swapchain released");
                break;
            }
        }
    }
    return refs;
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* self, UINT syncInterval, UINT flags)
{
    auto original = Original<PFN_Present>(self, kPresent);
    if (IsBypassed() || (flags & DXGI_PRESENT_TEST))
        return original(self, syncInterval, flags);

    BypassScope bypass;
    ComposeFrame(self);
    return original(self, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* self, UINT syncInterval, UINT flags,
                                        const DXGI_PRESENT_PARAMETERS* params)
{
    auto original = Original<PFN_Present1>(self, kPresent1);
    if (IsBypassed() || (flags & DXGI_PRESENT_TEST))
        return original(self, syncInterval, flags, params);

    BypassScope bypass;
    if (ComposeFrame(self))
    {
        // Dirty/scroll rects are in game-buffer coordinates; present the whole frame instead.
        DXGI_PRESENT_PARAMETERS full = {};
        return original(self, syncInterval, flags, &full);
    }
    return original(self, syncInterval, flags, params);
}

HRESULT STDMETHODCALLTYPE Hook_GetBuffer(IDXGISwapChain* self, UINT buffer, REFIID riid, void** surface)
{
    if (!IsBypassed())
    {
        LockGuard guard(g_stateLock);
        SwapChainState* state = FindState(self);
        if (state && state->virtualized)
            return state->gameBuffer->QueryInterface(riid, surface);
    }
    return Original<PFN_GetBuffer>(self, kGetBuffer)(self, buffer, riid, surface);
}

HRESULT STDMETHODCALLTYPE Hook_GetDesc(IDXGISwapChain* self, DXGI_SWAP_CHAIN_DESC* desc)
{
    HRESULT hr = OrigGetDesc(self, desc);
    if (SUCCEEDED(hr) && desc && !IsBypassed())
    {
        LockGuard guard(g_stateLock);
        SwapChainState* state = FindState(self);
        if (state && state->virtualized)
        {
            desc->BufferDesc.Width = state->gameW;
            desc->BufferDesc.Height = state->gameH;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_GetDesc1(IDXGISwapChain1* self, DXGI_SWAP_CHAIN_DESC1* desc)
{
    HRESULT hr = Original<PFN_GetDesc1>(self, kGetDesc1)(self, desc);
    if (SUCCEEDED(hr) && desc && !IsBypassed())
    {
        LockGuard guard(g_stateLock);
        SwapChainState* state = FindState(self);
        if (state && state->virtualized)
        {
            desc->Width = state->gameW;
            desc->Height = state->gameH;
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* self, UINT count, UINT w, UINT h, DXGI_FORMAT format,
                                             UINT flags)
{
    auto original = Original<PFN_ResizeBuffers>(self, kResizeBuffers);
    if (IsBypassed())
        return original(self, count, w, h, format, flags);

    LockGuard guard(g_stateLock);
    SwapChainState* state = FindState(self);
    if (state)
    {
        state->pass.ReleaseIntermediate();
        state->pass.Flush();
    }

    BypassScope bypass; // DXGI may call hooked methods internally while we hold the lock
    HRESULT hr = original(self, count, w, h, format, flags);
    if (FAILED(hr))
    {
        Log("ResizeBuffers(%u, %ux%u) failed: 0x%08lX", count, w, h, static_cast<unsigned long>(hr));
        return hr;
    }

    if (!state)
    {
        g_states.push_back(std::make_unique<SwapChainState>());
        state = g_states.back().get();
        state->swapChain = self;
    }
    Virtualize(*state, self, flags);
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_ResizeTarget(IDXGISwapChain* self, const DXGI_MODE_DESC* mode)
{
    auto original = Original<PFN_ResizeTarget>(self, kResizeTarget);
    if (IsBypassed() || !mode || !mode->Width || !mode->Height)
        return original(self, mode);

    BOOL fullscreen = FALSE;
    IDXGIOutput* output = nullptr;
    if (FAILED(self->GetFullscreenState(&fullscreen, &output)) || !fullscreen || !output)
    {
        SafeRelease(output);

        // Borderless under Wine: the engine shrinks the window to the game resolution during a
        // reset and then stretches it back over the monitor. On macOS the shrunken window gets
        // pushed below the menu bar, and the round trip is what leaves the mouse drifting.
        // The window ends up exactly where it already is, so skip the detour.
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (g_isWine && !fullscreen && SUCCEEDED(OrigGetDesc(self, &desc)) && CoversItsMonitor(desc.OutputWindow))
        {
            Log("Borderless window already covers the monitor; not shrinking it to %ux%u.", mode->Width,
                mode->Height);
            return S_OK;
        }
        return original(self, mode);
    }

    if (!GetSettings().enabled.load())
    {
        output->Release();
        return original(self, mode);
    }

    // Exclusive fullscreen: keep the monitor at its native mode and let FSR do the scaling instead
    // of switching the display to the lower game resolution.
    HRESULT hr;
    UINT nativeW = 0, nativeH = 0;
    if (GetDesktopMode(output, nativeW, nativeH) && mode->Width <= nativeW && mode->Height <= nativeH &&
        (mode->Width < nativeW || mode->Height < nativeH))
    {
        DXGI_MODE_DESC wanted = *mode;
        wanted.Width = nativeW;
        wanted.Height = nativeH;
        wanted.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
        wanted.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

        DXGI_MODE_DESC closest = {};
        if (SUCCEEDED(output->FindClosestMatchingMode(&wanted, &closest, nullptr)) && closest.Width == nativeW &&
            closest.Height == nativeH)
            wanted = closest;
        else
            wanted.RefreshRate.Numerator = wanted.RefreshRate.Denominator = 0;

        Log("Exclusive fullscreen: keeping the display at %ux%u instead of switching to %ux%u.", nativeW, nativeH,
            mode->Width, mode->Height);
        hr = original(self, &wanted);
    }
    else
    {
        hr = original(self, mode);
    }

    output->Release();
    return hr;
}

// ---------------------------------------------------------------------------------------------
// Installation
// ---------------------------------------------------------------------------------------------

bool WriteVTableEntry(void** entry, void* value)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(entry, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    *entry = value;
    VirtualProtect(entry, sizeof(void*), oldProtect, &oldProtect);
    return true;
}

void PatchSwapChainVTable(IDXGISwapChain* swapChain)
{
    void** vtable = *reinterpret_cast<void***>(swapChain);
    const int count = g_vtableCount.load();
    for (int i = 0; i < count; ++i)
    {
        if (g_vtables[i].vtable == vtable)
            return;
    }
    if (count == kMaxVTables)
        return;

    // Only touch the IDXGISwapChain1 slots if this vtable really is an IDXGISwapChain1 vtable.
    bool hasSwapChain1 = false;
    IDXGISwapChain1* swapChain1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&swapChain1))))
    {
        hasSwapChain1 = static_cast<void*>(swapChain1) == static_cast<void*>(swapChain);
        swapChain1->Release();
    }

    struct HookEntry
    {
        Slot slot;
        void* hook;
        bool swapChain1Only;
    };
    const HookEntry hooks[] = {
        {kRelease, reinterpret_cast<void*>(&Hook_Release), false},
        {kPresent, reinterpret_cast<void*>(&Hook_Present), false},
        {kGetBuffer, reinterpret_cast<void*>(&Hook_GetBuffer), false},
        {kGetDesc, reinterpret_cast<void*>(&Hook_GetDesc), false},
        {kResizeBuffers, reinterpret_cast<void*>(&Hook_ResizeBuffers), false},
        {kResizeTarget, reinterpret_cast<void*>(&Hook_ResizeTarget), false},
        {kGetDesc1, reinterpret_cast<void*>(&Hook_GetDesc1), true},
        {kPresent1, reinterpret_cast<void*>(&Hook_Present1), true},
    };

    // Record the originals and publish the entry before patching, so a hook that fires on another
    // thread the moment it is written can always find its original.
    HookedVTable& entry = g_vtables[count];
    entry.vtable = vtable;
    for (const HookEntry& hook : hooks)
    {
        if (!hook.swapChain1Only || hasSwapChain1)
            entry.original[hook.slot] = vtable[hook.slot];
    }
    g_vtableCount.store(count + 1, std::memory_order_release);

    for (const HookEntry& hook : hooks)
    {
        if (entry.original[hook.slot] && !WriteVTableEntry(&vtable[hook.slot], hook.hook))
            Log("Failed to patch swapchain vtable slot %d.", static_cast<int>(hook.slot));
    }
}

HWND CreateDummyWindow()
{
    static const wchar_t* kClassName = L"AnomalyFsrDummyWindow";
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered)
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = instance;
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, instance, nullptr);
}

// Creates throwaway swapchains (flip model like the modded exes, and the legacy blit model) to
// find DXGI's swapchain vtable(s).
bool PatchFromDummySwapChains()
{
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    auto createDevice = GetProc<PFN_CreateDevice>(d3d11, "D3D11CreateDevice");
    if (!createDevice)
    {
        Log("d3d11.dll is not available.");
        return false;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ID3D11Device* device = nullptr;
    HRESULT hr = createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &device,
                              nullptr, nullptr);
    if (FAILED(hr))
        hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &device, nullptr,
                          nullptr);
    if (FAILED(hr))
    {
        Log("Could not create a temporary D3D11 device (0x%08lX).", static_cast<unsigned long>(hr));
        return false;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory* factory = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)))
        adapter->GetParent(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);

    if (factory)
    {
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory2))))
        {
            const DXGI_SWAP_EFFECT effects[] = {DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
            for (DXGI_SWAP_EFFECT effect : effects)
            {
                HWND window = CreateDummyWindow();
                DXGI_SWAP_CHAIN_DESC1 desc = {};
                desc.Width = 64;
                desc.Height = 64;
                desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SwapEffect = effect;
                IDXGISwapChain1* swapChain = nullptr;
                hr = window ? factory2->CreateSwapChainForHwnd(device, window, &desc, nullptr, nullptr, &swapChain)
                            : E_FAIL;
                if (SUCCEEDED(hr))
                    PatchSwapChainVTable(swapChain);
                SafeRelease(swapChain);
                if (window)
                    DestroyWindow(window);
                if (SUCCEEDED(hr))
                    break;
            }
            factory2->Release();
        }

        HWND window = CreateDummyWindow();
        if (window)
        {
            DXGI_SWAP_CHAIN_DESC desc = {};
            desc.BufferDesc.Width = 64;
            desc.BufferDesc.Height = 64;
            desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 1;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            IDXGISwapChain* swapChain = nullptr;
            if (SUCCEEDED(factory->CreateSwapChain(device, &desc, &swapChain)))
                PatchSwapChainVTable(swapChain);
            SafeRelease(swapChain);
            DestroyWindow(window);
        }
        factory->Release();
    }

    device->Release();
    return g_vtableCount.load() > 0;
}

} // namespace

bool InstallHooks()
{
    LockGuard guard(g_installLock);
    if (g_vtableCount.load() > 0)
        return true;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_isWine = ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
    if (g_isWine)
        Log("Running under Wine: mouse input will be resynced after video resets.");

    if (!PatchFromDummySwapChains())
    {
        Log("Failed to hook DXGI; FSR is not available.");
        LockGuard stateGuard(g_stateLock);
        SetStatus(HookState::NotInstalled, "failed to hook DXGI");
        return false;
    }

    Log("Hooked %d DXGI swapchain vtable(s).", g_vtableCount.load());
    LockGuard stateGuard(g_stateLock);
    SetStatus(HookState::WaitingForReset, "waiting for the game to reset its video device");
    return true;
}

HookState GetHookState()
{
    return static_cast<HookState>(g_hookState.load());
}

void GetStatusText(char* buffer, size_t size)
{
    LockGuard guard(g_stateLock);
    snprintf(buffer, size, "%s", g_status);
}

} // namespace afsr
