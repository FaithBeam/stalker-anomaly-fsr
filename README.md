# Anomaly FSR 1

AMD FidelityFX Super Resolution 1.0 for S.T.A.L.K.E.R. Anomaly (DX11 renderer).

When the game resolution you pick in **Options > Video** is lower than your display, Windows or the monitor
normally stretches the image with plain bilinear filtering. This mod does that upscale with FSR 1 instead:
**EASU** (edge-adaptive upscaling) followed by optional **RCAS** sharpening. At native resolution it does nothing.

| Screen mode | What gets upscaled |
|---|---|
| Borderless | game resolution → window size (your monitor) |
| Fullscreen | game resolution → native display mode (the monitor stays at native, it no longer switches down) |
| Windowed | nothing; the window is already the size of the game resolution |

## Requirements

- STALKER Anomaly 1.5.x, **DX11** renderer (`AnomalyDX11.exe` / `AnomalyDX11AVX.exe`)
- **Modded exes** ([themrdemonized/xray-monolith](https://github.com/themrdemonized/xray-monolith)). They build
  LuaJIT with `ffi`, which the mod uses to load its DLL. GAMMA already includes them.
- Windows 10 or 11
- Optional: Mod Configuration Menu (MCM) for in-game settings

## Install with Mod Organizer 2

1. In MO2, click **Install a new mod from an archive** and select `Anomaly-FSR1-<version>.zip`.
   The archive has `bin/` and `gamedata/` at the top level, which MO2's Anomaly support maps onto the game folder.
2. Enable the mod in the left pane. Load order doesn't matter: the mod only adds new files.
3. Start the game from MO2 with the DX11 executable, as usual.
4. In **Options > Video**, choose **Borderless** or **Fullscreen** and a resolution below your monitor's,
   e.g. 1706×960 or 1920×1080 on a 2560×1440 monitor.
5. Load a save. On the first frame in game, the mod runs one `vid_restart` (the screen blinks once) and FSR is active
   from then on, including back in the main menu.

The DLL is loaded at runtime from the virtual `bin` folder, so it works through MO2's VFS. You don't need Root
Builder or to copy anything into the game folder. Manual install: extract the archive into the Anomaly folder.

## Settings

With MCM: **Mod Configuration Menu > FSR 1 Upscaling**

| Option | Default | |
|---|---|---|
| Enable FSR upscaling | on | Turning it off goes back to the normal stretched output (one `vid_restart`) |
| RCAS sharpening | on | FSR's sharpening pass after the upscale |
| Sharpness reduction (stops) | 0.25 | 0 = sharpest, each +1.0 halves the sharpening |

Without MCM, edit the `defaults` table at the top of `gamedata/scripts/anomaly_fsr.script`.

**Choosing a resolution.** AMD's FSR 1 quality modes as a fraction of the display resolution:
Ultra Quality 77%, Quality 67%, Balanced 59%, Performance 50%.
On 2560×1440: 1970×1108, 1706×960, 1506×847, 1280×720. Only resolutions in the game's list can be picked.

## Checking it works

Everything is logged with an `[Anomaly FSR]` prefix to the console and `appdata/logs/xray_<user>.log`:

```
[Anomaly FSR] Loaded C:\...\bin\anomaly_fsr.dll v1.0.0
[Anomaly FSR] Hooked 1 DXGI swapchain vtable(s).
[Anomaly FSR] Resetting the video device to apply FSR
[Anomaly FSR] Upscaling 1706x960 -> 2560x1440 with FSR 1.
```

Other scripts can read the current state with `anomaly_fsr.status()`, e.g.
`1706x960 -> 2560x1440, FSR 1 EASU + RCAS (sharpness 0.25)`.

## Limitations

- The **first** main menu after launching the game isn't upscaled. Anomaly only runs mod scripts once a save loads.
- The HUD and menus are drawn at the game resolution, so they are upscaled too (with FSR, not bilinear).
- DX8/DX9/DX10 renderers aren't supported. The mod logs this and stays inactive.
- On vanilla exes without `ffi`, the script falls back to `package.loadlib` (default settings, no MCM). If neither
  exists, it logs an error and does nothing.
- macOS (CrossOver / Sikarugir / Whisky with D3DMetal) works. D3DMetal lacks `CreateDeviceContextState`, so the DLL
  saves and restores the pipeline state it touches by hand instead (logged once). Under Wine, a video reset in the
  middle of the game can leave the Mac cursor and Wine's cursor clip out of step (the view drifts when the mouse
  moves, until you Cmd-Tab out and back). So under Wine the DLL does not let the reset shrink a borderless window
  that already covers the monitor, and 1.5 s after each reset it re-applies the cursor clip and re-acquires mouse
  input. It also logs the window, clip and cursor rectangles around that resync (`Wine mouse ...` lines), which
  helps if the drift ever comes back.
- ReShade and other tools that wrap the swapchain are untested. Overlays like Steam and RTSS draw on top of the
  final, upscaled image.

## How it works

`gamedata/scripts/anomaly_fsr.script` loads `bin/anomaly_fsr.dll` with LuaJIT `ffi.load` in `on_game_start`.
The DLL patches DXGI's swapchain vtable (`Present`, `ResizeBuffers`, `GetBuffer`, `ResizeTarget`, …):

1. When the engine resizes its swapchain (`CHW::Reset`), the DLL hands it a private texture at the game
   resolution through `GetBuffer`. The engine renders into that texture as if it were the back buffer.
2. The real swapchain buffers are resized to the window client area. In exclusive fullscreen, `ResizeTarget`
   keeps the display at its native mode.
3. On `Present`, the private texture is upscaled into the real back buffer with FSR 1 EASU, then RCAS. When the
   sizes already match, it's a plain copy. The passes run inside a separate `ID3DDeviceContextState`, so the
   engine's cached pipeline state is never disturbed.

The FSR shaders are AMD's unmodified `ffx_a.h` / `ffx_fsr1.h` (FP32 path), embedded in the DLL and compiled at
startup with the system `d3dcompiler_47.dll`.

## Building

MinGW-w64 cross build (macOS/Linux), produces `dist/Anomaly-FSR1-<version>.zip`:

```sh
brew install mingw-w64 cmake    # or your distro's mingw-w64 package
tools/package.sh
```

Visual Studio 2019+ on Windows:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Then copy `build/Release/anomaly_fsr.dll` to `mod/bin/` and zip `mod/` (with `bin/` and `gamedata/` at the root).

`fsr_selftest.exe path\to\anomaly_fsr.dll` (built alongside the DLL; runs on Windows or under Wine) recreates the
engine's swapchain and reset sequence. It checks the upscaled output, the reported swapchain size and that pipeline
state is preserved.

## Credits

- AMD FidelityFX Super Resolution 1.0, © 2021 Advanced Micro Devices, Inc., MIT license
  (`third_party/fsr1/LICENSE.txt`, included in the release archive).
