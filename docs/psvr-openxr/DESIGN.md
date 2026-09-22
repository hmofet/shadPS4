# Design: PSVR support in shadPS4 via OpenXR

## 1. Principle

Do not emulate the PSVR hardware. On a real PS4 the camera, LED constellation
tracking, the processing unit, and the lens-distortion pass all live inside system
libraries that shadPS4 already replaces with high-level (HLE) implementations:
`libSceHmd`, `libSceHmdReprojection`, `libSceVrTracker`, `libSceCamera`,
`libSceHmdSetupDialog`. Implement those libraries against **OpenXR**. The guest sees a
connected, tracking PSVR; the host sees an ordinary OpenXR application, so every
runtime (SteamVR incl. the PSVR2 adapter, Oculus/Link, Virtual Desktop) works.

Current state of upstream (v0.18.0): all five libraries exist with correct struct
layouts and NIDs, but report "not detected" / return stub errors. The scaffolding is
in `src/core/libraries/{hmd,vr_tracker,camera}` and `hmd/hmd_reprojection.cpp`.

## 2. Host-side component: `VrRuntime`

New module `src/vr/` (name tentative):

- `openxr_runtime.{h,cpp}`: instance/session lifetime, `XR_KHR_vulkan_enable2`
  sharing the emulator's `VkInstance`/`VkDevice`/queue, reference space (LOCAL, with
  a user recenter offset), swapchains per eye, `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`.
- `pose_source.h`: `GetViews(predicted_time) → {left, right, head}` and
  `GetHeadVelocity()`; consumed by the VrTracker HLE.
- `layer_submit.h`: accepts guest images (from the texture cache) + per-eye FOV + pose,
  blits into the current swapchain images, ends the frame.
- Config (`config.toml` / `config.json`): `vr.enabled`, `vr.mirror` (left/right/social
  screen/off), `vr.render_scale`, `vr.refresh_hint`, `vr.recenter_key`.

The runtime is optional at build time (`ENABLE_OPENXR`, default on where the loader is
available) and at run time (a missing runtime keeps today's behaviour).

## 3. Guest-side libraries

### 3.1 libSceHmd
| Function | Behaviour with VR enabled |
|---|---|
| `sceHmdInitialize` / `315` | OK, no warning; create runtime lazily |
| `sceHmdOpen` | return handle (already does) |
| `sceHmdGetDeviceInformation[ByHandle]` | status READY, panel 1920x1080, `hmu_mount = 1`, latencies from PSVR (90/120 Hz) |
| `sceHmdGetFieldOfView` | tangents of the **real** headset's OpenXR views (symmetrised if the game assumes PSVR symmetry). PSVR reference values are kept as a fallback preset |
| `sceHmdGet2DEyeOffset` | from OpenXR view poses (half IPD) |
| `sceHmdGetInertialSensorData` | synthesised from head pose delta (angular velocity) |
| `sceHmdInternalGetDeviceStatus`, `GetIPD`, `GetHmuOpticalParam`, `GetVirtualDisplay*` | consistent with the above |

### 3.2 libSceHmdSetupDialog
State machine: Initialize → Open → status FINISHED, result OK on the first `GetResult` → Close → Terminate. No UI.

### 3.3 libSceCamera
`IsAttached = 1`; Open/SetConfig/Start/SetVideoSync/Stop OK. `GetFrameData` returns a
valid frame descriptor pointing at a zeroed dual 640x360 buffer (allocated from the
guest memory the game handed to the tracker), monotonic frame counter at 60 Hz,
timestamps from the guest clock. Games only forward these frames to the tracker.

### 3.4 libSceVrTracker
- `QueryMemory`, `Init`, `RegisterDevice[2]`, `UnregisterDevice`, `Recalibrate`, `SetDurationUntilStatusNotTracking`: OK, record device handles (HMD, pad, Move, gun).
- `GpuSubmit`: mark "submitted", return OK. `GpuWait`, `GpuWaitAndCpuProcess`, `CpuProcess`, `NotifyEndOfCpuProcess`: OK when submitted, else the existing error.
- `UpdateMotionSensorData`: accept; used to correlate the game's frame numbers.
- `GetTime`: predicted display time from the last `xrWaitFrame`, converted to the guest tick base.
- `GetResult`: fill `OrbisVrTrackerResultData`: `connected = 1`, status TRACKING, position/orientation quality HIGH, `led_color` per device, timestamps; `hmd_info.{device,head,left_eye,right_eye}_pose` from `xrLocateViews` at `param->prediction_time`; velocities/accelerations from `xrLocateSpace` velocity; `camera_orientation` identity. Pad pose: fixed offset below the head, or an OpenXR controller pose if `vr.pad_tracking = controller`.
- Coordinate system: both sides are right-handed, Y-up, metres. PSVR's origin is the camera; OpenXR LOCAL origin is the headset's start pose. Apply a fixed forward offset (~1.5 m) so games that assume the camera is in front behave.
- `ResetOrientationRelative` / `Recalibrate` → recenter.

### 3.5 libSceHmdReprojection — the output path
Sony's library takes the game's stereo render target(s) plus the pose used to render,
performs late reprojection and lens distortion, and writes the HMD panel image. We skip
distortion entirely (the OpenXR runtime does its own) and treat the library as the
"present to headset" call:

1. `Initialize`, `Query*Buff*` (already answered), `SetOutputMinColor`, `SetCallback`,
   `SetUserEvent*`: record.
2. `SetDisplayBuffers` / `AddDisplayBuffer`: record the guest addresses, format, size
   and layout. `Start` / `StartWithOverlay` / `StartMultilayer*` / `Start2dVr` /
   `StartWideNear*`: record the variant and enable the VR flip path.
3. On each VideoOut flip while VR is active: look up the eye images in the texture
   cache (same path as screenshots/readbacks), blit each eye into its OpenXR swapchain
   image, submit a projection layer whose pose is the one handed to the game for that
   frame (`user_frame_number` correlation) and whose FOV matches the tangents we
   returned. Overlay variants add a quad layer for the 2D HUD. `Start2dVr` presents a
   virtual screen (cinematic mode) as a quad layer.
4. `Stop` / `Finalize`: leave VR flip, drop swapchains.

The exact buffer packing (side-by-side vs. two buffers, per-eye resolution, tiling,
layer count) differs per title and is unknown until a game enters VR mode. The first
deliverable is therefore **instrumentation**: log every reprojection and tracker call
with decoded arguments, behind `Lib_Hmd` trace logging.

### 3.6 Social screen and mirror
`libSceSocialScreen` is a stub today. Keep it that way initially; the desktop window
presents one eye (`vr.mirror`). Later: honour the game's social-screen buffer.

### 3.7 Audio
`libSceAudio3d` is already HLE'd and produces a binaural stereo mix; no VR-specific
work beyond selecting the headset's audio device.

### 3.8 Input
DualShock 4 path unchanged. Later: OpenXR action bindings that map Touch/Sense
controllers onto pad buttons, and a Move emulation profile.

## 4. Frame flow

```
game thread:  VrTrackerGetResult(pred_t) ──▶ render both eyes ──▶ HmdReprojectionStart/flip
                    ▲                                                   │
                    │ poses @ pred_t                                    ▼
OpenXR:       xrWaitFrame ──▶ xrLocateViews ──▶ (game renders) ──▶ blit eyes ──▶ xrEndFrame
```

PSVR titles run at 60, 90 or 120 Hz with the console reprojecting to the panel's
120 Hz. We keep the guest vblank at the game's native rate and let the OpenXR runtime
handle reprojection/ASW; `vr.refresh_hint` can raise vblank for titles that tolerate it.

## 5. Milestones

| # | Name | Exit criterion (WipEout Omega Collection) |
|---|---|---|
| M0 | Instrumentation | Full decoded trace of the VR boot path and of VR-mode entry |
| M1 | Presence | VR option appears in Options; game enters VR mode and renders stereo into the desktop window |
| M2 | Tracking | Head movement drives the view (window mirror), recenter works |
| M3 | Submit | Image in the headset through SteamVR; correct scale and IPD |
| M4 | Polish | HUD overlay layer, social screen, config UI, audio device, stability across a full race |
| M5 | Breadth | Rez Infinite, Astro Bot Rescue Mission, a Move title; then PR preparation |

## 6. Risks

- Vulkan interop: pulling the guest's tiled render target out of the texture cache
  at the right moment without stalling the GPU pipeline. Mitigation: reuse the
  screenshot/readback path first, optimise later.
- Titles that wait on reprojection user events or callbacks (`SetUserEventStart/End`, `SetCallback`) need those signalled with plausible timing.
- Games checking PS4 Pro / "neo mode" for higher VR resolution: expose `neo_mode` as today.
- PSVR-specific symmetric FOV assumptions in some games; keep a "PSVR preset" FOV.
- OpenXR loader availability on Linux/macOS builds: compile-time optional.

## 7. Build notes

Windows: Visual Studio 2022, CMake, Qt not required for the CLI target. Add the
OpenXR SDK loader as an external (vcpkg or `externals/`). SteamVR is the active runtime
on the dev machine; Oculus and Virtual Desktop runtimes are installed for testing.
