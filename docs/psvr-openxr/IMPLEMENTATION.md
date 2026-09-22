# Implementation status

What the `psvr-openxr` branch implements against [DESIGN.md](DESIGN.md), how to use it, and
where it departs from the design. Everything here is off by default: with `psvr_enabled` false
the libraries behave exactly as upstream.

## Turning it on

Per game (recommended), in `custom_configs/<TITLE_ID>.json` under the user folder, or globally in
its `config.json`. The user folder is `%APPDATA%\shadPS4` on Windows, unless a `user` folder
exists in the working directory:

```json
{
  "VR": {
    "psvr_enabled": true,
    "pose_source": "openxr",
    "eye_source": "sbs",
    "fov_mode": "native",
    "mirror": "full",
    "render_scale": 1.0,
    "recenter_key": "Keypad 5"
  }
}
```

The same options are in the in-emulator settings (Experimental, per-game profile). They are read
once when the game starts.

| Key | Values | Meaning |
|---|---|---|
| `psvr_enabled` | `false` / `true` | Report a connected, tracking PSVR to the game |
| `pose_source` | `openxr`, `desk`, `static` | Head tracking from the OpenXR headset, from the numpad, or fixed. `openxr` falls back to `desk` when no runtime or headset is available |
| `eye_source` | `sbs`, `mono` | Left/right halves of the flipped frame to the two eyes, or the whole frame to both |
| `fov_mode` | `native`, `psvr` | FOV reported by `sceHmdGetFieldOfView`: the real headset's, or the original PSVR values |
| `mirror` | `full`, `left`, `right` | What the desktop window shows while the game is in VR mode |
| `render_scale` | 0.5 to 4.0 | Multiplier on the 1920x1080 panel size reported to the game |
| `recenter_key` | SDL scancode name | Recenters the view |

Desk mode keys (numpad): 4/6 yaw, 8/2 pitch, 7/9 roll, 1/3 strafe, +/- forward and back.

Trace logging for M0 is at Debug level so it is present in release builds. Level names must be
lowercase, since spdlog turns an unknown name such as `Debug` into "off". Enable it with the log
filter `Lib.Hmd:debug Lib.VrTracker:debug Lib.Camera:debug Lib.HmdSetupDialog:debug`. Each call
site logs its first 16 calls and then every 600th.

Build option: `ENABLE_OPENXR` (default ON except on macOS) builds the OpenXR loader from
`externals/openxr-sdk` (release 1.1.63) and links it statically. With it OFF, `openxr` falls back
to `desk`.

## Milestones

| # | State | Notes |
|---|---|---|
| M0 Instrumentation | Done in code | Decoded traces for Hmd, HmdSetupDialog, VrTracker, Camera (virtual path). Every `sceHmdReprojection*` entry point logs its six argument registers plus a 64-byte dump of any argument that points at readable guest memory |
| M1 Presence | Done in code | Headset READY with panel size, FOV, eye offsets; setup dialog finishes with OK; virtual camera attached and streaming blank frames; tracker GPU submit cycle completes. VR options in the settings dialog |
| M2 Tracking | Done in code | `sceVrTrackerGetResult` fills HMD, eye and head poses plus velocities from the pose source at the requested prediction time; pad/Move/gun get a fixed pose below the head. Recenter via key, `Recalibrate` and `ResetOrientationRelative` |
| M3 Submit | Done in code | Instance and device created through `XR_KHR_vulkan_enable2`, session on the emulator's queue, per-eye sRGB swapchains, projection layer with the pose and FOV given to the game |
| M4 Polish | Partly | Desktop mirror of one eye and the render scale option are in. Overlay quad layers, social screen, audio device selection and a VR config page beyond the Experimental tab are not |
| M5 Breadth | Not started | Needs M0 traces from real titles first |

"Done in code" means built and reviewed; none of it has been run against a game or a real headset
yet. The first real run is the M0 exit criterion: a WipEout Omega Collection trace with PSVR
enabled.

## Verification so far

| Check | Result |
|---|---|
| Linux build, clang 18, `ENABLE_OPENXR=ON` | Builds; no warnings in the new or changed files |
| Unit tests (`shadps4_settings_test`) | 84 of 84 pass, including 4 VR settings and 6 pose math tests |
| OpenXR runtime smoke test against Monado 21 (simulated HMD, null compositor, lavapipe) | Instance and device through `XR_KHR_vulkan_enable2`, session to FOCUSED, 120 frames with a pose each, sRGB eye swapchains, native FOV and 63 mm IPD read back, recenter moves the head to the origin |
| Windows build, clang-cl 20 (VS 2026), preset `x64-Clang-RelWithDebInfo` | Builds; no warnings in the new or changed files; `shadps4.exe` starts |
| WipEout Omega Collection (CUSA05670 v1.07), no headset connected, desk pose source | Reaches VR mode and runs its VR frame loop at about 30 frames per second; both eye textures captured showing the stereo logo screen (screenshots kept outside the repo: they are game content); with a controller it goes past the title screen |
| SteamVR with a headset | Not done: SteamVR reported no headset (`XR_ERROR_FORM_FACTOR_UNAVAILABLE`) |

The smoke test drives `openxr_runtime.cpp` unchanged, with two shim headers for logging and the
guest clock. Before the session is running the runtime cannot report views, so the FOV query
falls back to the PSVR values; games that ask for the FOV before the session starts get those.

Build notes for Linux with CMake 4.x and Ubuntu's clang 18: configure with
`-DCMAKE_CXX_SCAN_FOR_MODULES=OFF` (CMake otherwise wants `clang-scan-deps`) and
`-DCMAKE_CXX_FLAGS=-Wno-invalid-constexpr` (an upstream `fontft_internal.h` construct that clang 18
rejects). Neither is related to this work.

## First run: what WipEout does

Boot sequence with PSVR enabled, from the M0 trace:

1. `sceHmdInitialize315`, `sceHmdGetDeviceInformation`, `sceHmdReprojectionInitialize`
2. Camera: `Open`, `SetConfig`, `SetVideoSync`, `Start` (virtual camera)
3. `sceHmdOpen`, `GetDeviceInformationByHandle`, `GetFieldOfView`, `SetOutputMinColor`,
   `SetDisplayBuffers`
4. `sceVrTrackerQueryMemory`, `Init`, `RegisterDevice(HMD)`
5. Per frame, on the game's `VRTrackerCmdThread`: `sceCameraGetFrameData`, `GpuSubmit`,
   `GpuWaitAndCpuProcess`, `UpdateMotionSensorData`; on the render thread: `GetResult`, render,
   then `sceHmdReprojectionStart`

Findings that shaped the code:

- **`sceHmdReprojectionStart` is the per-frame submit.** Its first argument begins with two
  pointers to texture descriptors (T#), one per eye. In WipEout both describe one 2D array
  texture, 1344x1512 RGBA8 sRGB, with `base_array` 0 for the left eye and 1 for the right; two
  such textures alternate. The second argument holds the pose the frame was rendered with. The
  eye textures are now read from those descriptors and presented; splitting the flipped frame
  (`eye_source`) remains only for games that do not submit through `Start`.
- **In VR mode the game never flips.** The real library scans out to the headset on every
  vblank, re-showing the last frame when there is no new one, and WipEout's
  `VRTrackerCntrThread` waits on flip events from that (queue `VRTrackerDisplayEventQueue`).
  Without them the render thread blocks on `VRTrackerCntrDoneEvent` right after the first
  `Start`. VideoOut therefore completes each submitted eye frame as a flip of the main port and
  raises a flip event on every other vblank while VR mode is active. Flipping only when the game
  submits paced the game to about 4 frames per second.
- **The camera must accept `SetConfig`.** It returned "not connected" with no host webcam, and
  the game retried open, configure and close forever.

Game-only screenshots (the screenshot hotkey) capture the mirrored eye in VR mode.

## Frame path as built

```
flip ─▶ Presenter::PrepareFrame
          VR::BeginFrame      xrWaitFrame + xrBeginFrame + acquire eye images (VR mode only)
          VrPass::Render      blit each eye out of the flipped frame into an intermediate
                              image, copy it into the eye's swapchain image
          draw_scheduler.Flush
          VR::EndFrame        release images + xrEndFrame, under the queue submit lock
```

"VR mode" starts with any `sceHmdReprojectionStart*` call and ends with `Stop` or `Finalize`.
While it lasts, `sceHmdReprojectionStart` sends the frame's eye textures to
`Presenter::PrepareHmdFrame` through the GPU thread (after the game's rendering), which blits each
eye's array layer into the headset and draws one eye in the window.

The intermediate copy exists because the guest image is already display encoded. Blitting into
an sRGB swapchain image would encode it twice; blitting into an intermediate of the same encoding
and then copying the bits across keeps it exact.

## Departures from the design

- **Only plain `sceHmdReprojectionStart` is decoded.** The other `Start` variants (overlay,
  multilayer, wide-near, 2D VR) enter VR mode but their eye textures are not read yet; those
  games fall back to splitting the flipped frame. The `Start` layout is inferred from one title.
- **`sceVrTrackerGetTime` still returns the current process time.** The design had it return the
  predicted display time. Games use it as "now" and add their own prediction, so returning a future
  time would count the latency twice. Prediction is applied in `GetResult` instead, from the
  requested `prediction_time` (or 22 ms when none is given).
- **No `refresh_hint` option.** The existing per-game Vblank Frequency setting already raises the
  guest vblank; a second knob for the same thing was left out.
- **No controller tracking.** `pad_tracking = controller` (OpenXR action poses) belongs with input
  mapping in PR 5.
- **Unknown layouts left untouched.** `sceHmdGetInertialSensorData` and `sceHmdGetAssyError`
  return OK without writing, and the flip-to-display latencies are zero, until traces show what
  games read from them.

## Coordinate mapping

Both sides are right-handed, +Y up, -Z forward, metres. OpenXR's LOCAL origin is the head's start
pose; PSVR poses are relative to the camera in front of the player. The tracker adds
`VR::CameraDistance` (1.5 m) along +Z to every pose it returns, and nothing else.
