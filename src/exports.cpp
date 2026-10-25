// C interface used by gamedata/scripts/anomaly_fsr.script (through LuaJIT's ffi, or package.loadlib).
#include "common.h"
#include "log.h"
#include "swapchain_hooks.h"

#include <algorithm>

#define AFSR_API extern "C" __declspec(dllexport)

namespace {

// The hooks point into this DLL, so it must never be unloaded (e.g. when Lua garbage-collects the
// ffi library handle).
void PinModule()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&PinModule), &self);
}

} // namespace

AFSR_API const char* afsr_version(void)
{
    return ANOMALY_FSR_VERSION;
}

AFSR_API int afsr_init(void)
{
    static bool announced = false;
    if (!announced)
    {
        announced = true;
        PinModule();
        afsr::Log("Anomaly FSR %s (AMD FidelityFX Super Resolution 1.0).", ANOMALY_FSR_VERSION);
    }
    return afsr::InstallHooks() ? 1 : 0;
}

AFSR_API void afsr_set_enabled(int enabled)
{
    afsr::GetSettings().enabled.store(enabled != 0);
}

AFSR_API void afsr_set_sharpening(int enabled)
{
    afsr::GetSettings().sharpening.store(enabled != 0);
}

AFSR_API void afsr_set_sharpness(float stops)
{
    afsr::GetSettings().sharpness.store(std::clamp(stops, 0.0f, 2.0f));
}

// 0 = not hooked, 1 = waiting for vid_restart, 2 = active without upscaling, 3 = upscaling.
AFSR_API int afsr_state(void)
{
    return static_cast<int>(afsr::GetHookState());
}

AFSR_API const char* afsr_status(void)
{
    static char status[256];
    afsr::GetStatusText(status, sizeof(status));
    return status;
}

AFSR_API const char* afsr_poll_log(void)
{
    return afsr::PollLog();
}

// package.loadlib(path, "luaopen_anomaly_fsr") fallback for builds without the ffi module.
// Signature matches lua_CFunction; it installs the hooks with default settings and returns nothing.
AFSR_API int luaopen_anomaly_fsr(void* /*lua_State*/)
{
    afsr_init();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(instance);
    return TRUE;
}
