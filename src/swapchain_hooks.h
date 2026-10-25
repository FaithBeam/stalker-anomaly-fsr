#pragma once

#include <cstddef>

namespace afsr {

enum class HookState : int
{
    NotInstalled = 0,  // afsr_init has not run (or failed)
    WaitingForReset = 1, // hooks are in, but the game has not resized its swapchain since (needs vid_restart)
    Native = 2,        // game buffer is managed, no upscaling needed (resolution matches the window/display)
    Upscaling = 3,     // FSR 1 is upscaling the game image every frame
};

// Patches the DXGI swapchain vtable (Present/GetBuffer/ResizeBuffers/...). Safe to call repeatedly.
bool InstallHooks();

HookState GetHookState();

// Human readable summary, e.g. "1600x900 -> 2560x1440, FSR 1 EASU + RCAS (sharpness 0.25)".
void GetStatusText(char* buffer, size_t size);

} // namespace afsr
