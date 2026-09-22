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
| `render_scale` | 0.5 to 4.0 | Multiplier on the 1920x1080 panel size reported to the game. WipEout sizes each eye at 1.4x half the panel, so 1.4 gives eye textures the size of a Quest 3 view |
| `recenter_key` | SDL scancode name | Recenters the view |
| `hmd_refresh_hz` | 120 (default), 90, or 0 | Vblank rate while the game is in VR mode, as the PSVR panel drives it on a console; 0 keeps the GPU vblank frequency setting |
| `controllers` | `true` / `false` | The headset's controllers act as the DualShock 4: sticks, triggers as R2/L2, grips as R1/L1, A/B as Cross/Circle, X/Y as Square/Triangle, stick clicks as L3/R3, menu as Options; the pad, Move or gun the tracker reports follows the right hand's grip |

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
| M3 Submit | Works in a headset; comfort and input done, race frame rate is the open item | Instance and device created through `XR_KHR_vulkan_enable2`, session on the emulator's queue, per-eye sRGB swapchains, projection layer with the pose and FOV given to the game |
| M4 Polish | Partly | Desktop mirror of one eye and the render scale option are in. Overlay quad layers, social screen, audio device selection and a VR config page beyond the Experimental tab are not |
| M5 Breadth | Not started | Needs M0 traces from real titles first |

"Done in code" means built and reviewed. M0 to M3 have since run with WipEout Omega Collection,
on its own and in a Quest 3; see "First run" and "Headset test" below.

## Verification so far

| Check | Result |
|---|---|
| Linux build, clang 18, `ENABLE_OPENXR=ON` | Builds; no warnings in the new or changed files |
| Unit tests (`shadps4_settings_test`) | 84 of 84 pass, including 4 VR settings and 6 pose math tests |
| OpenXR runtime smoke test against Monado 21 (simulated HMD, null compositor, lavapipe) | Instance and device through `XR_KHR_vulkan_enable2`, session to FOCUSED, 120 frames with a pose each, sRGB eye swapchains, native FOV and 63 mm IPD read back, recenter moves the head to the origin |
| Windows build, clang-cl 20 (VS 2026), preset `x64-Clang-RelWithDebInfo` | Builds; no warnings in the new or changed files; `shadps4.exe` starts |
| WipEout Omega Collection (CUSA05670 v1.07), no headset connected, desk pose source | Reaches VR mode and runs its VR frame loop at about 30 frames per second; both eye textures captured showing the stereo logo screen (screenshots kept outside the repo: they are game content); with a controller it goes past the title screen |
| Meta Quest 3 over Link, Meta OpenXR runtime 1.207 | Two sessions. Comfort, Touch controllers and sound all work; menus run at 60 fps and look sharp. Races drop to 20-35 fps and the game halves its own resolution: see "Headset test (2026-09-22)" |
| SteamVR with a headset | Works (2026-09-22, Quest 3 over Air Link): `SteamVR/OpenXR 2.17.10`, system `SteamVR/OpenXR : oculus`, session to FOCUSED, eye swapchains 1996x2156, `display refresh 120 Hz`, controllers mapped. Only one runtime can hold the headset: while SteamVR runs, the Meta runtime returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE` and the game silently falls back to desk tracking, so close SteamVR before running on the Meta runtime. SteamVR over Air Link also stacks two compositors and is choppy; it is not a useful frame-rate measurement |

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

## Headset test (2026-09-21, Quest 3 over Link)

Setup: Meta Quest 3 on Link, WipEout CUSA05670 v1.07, PC gamepad. SteamVR is the system's active
OpenXR runtime but sees no Link headset, so the game is launched with the Meta runtime selected
for that process:

```powershell
$env:XR_RUNTIME_JSON = "C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json"
& ".\Build\x64-Clang-RelWithDebInfo\shadps4.exe" -g "<games>\CUSA05670\eboot.bin"
```

What worked: the session reaches FOCUSED, the runtime asks for 1872x2016 per eye, the game's eye
textures show in the headset, and head tracking drives the view. The PC gamepad works once
background controller input is on, which PSVR mode now forces (the window is never focused while
the player is in the headset).

Observed problems, with the likely causes to check first:

| Problem | Likely causes |
|---|---|
| Very uncomfortable to play | (1) **Pose mismatch.** Each projection layer was submitted with the latest head pose handed to the game, not the pose that frame was rendered with. Fixed: see "Review and fixes" below. (2) **About 30 fps**: the game was paced to exactly half the 60 Hz vblank. Fixed by running the vblank at the PSVR panel rate in VR mode. (3) **FOV and eye mapping**: now verified from the projection the game passes with each frame, and taken from there |
| Menus look unstable | The same pose mismatch |
| Very low resolution and blocky in races | The game renders 1344x1512 per eye, which is 1.4x half the reported 1920x1080 panel, so `render_scale` controls it: 1.4 gives 1882x2117 eyes, about the Quest 3's 1872x2016 |
| No sound in the headset session | Sound played in the earlier desk-mode runs (through the Link audio device, the Windows default), but not with the OpenXR session active. Unexplained; see the checks in "Review and fixes" |
| Quest Touch controllers do nothing | Not implemented: mapping OpenXR controller actions onto the PS4 pad is the plan's input step (PR 5) |

Operational notes:

- With Link down, the Meta runtime returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE` and the game
  silently falls back to desk tracking. The tell is a head pose stuck at exactly (0, 0, 1.5) with
  identity rotation in the `sceVrTrackerGetResult` trace. The headset is only looked for at
  startup, so start Link first.
- A second shadPS4 instance fails with "Insufficient system resources" (the address space is
  taken) and overwrites the start of the shared log.

## Frame path as built

```
game render thread
  sceHmdReprojectionStart(a0, a1)
      read the eye T#s, the per-eye projection (a0) and the render pose (a1)
      VR::ResolveRenderViews   match the pose to the tracker sample it came from -> eye poses
      VideoOut::SubmitHmdFlip  accept or drop (host more than 2 frames behind)
      VR::OnHmdFrameAccepted   frame-rate log; xrWaitFrame (paces the game thread, not the GPU)
      liverpool->SendCommand(PrepareHmdFrame)

GPU thread
  Presenter::PrepareHmdFrame
      VR::BeginFrame           takes the waited frame state; xrBeginFrame + acquire eye images
      VrPass::Render           blit each eye's array layer into an intermediate image, copy it
                               into the eye's swapchain image
      draw_scheduler.Flush
      VR::EndFrame(views)      release images + xrEndFrame with the frame's own poses and FOV,
                               under the queue submit lock

present thread (vblank, 120 Hz in VR mode)
  Flip                         present the mirror, raise the flip event
  SignalReprojectionFlip       flip event on vblanks with no new frame
```

"VR mode" starts with any `sceHmdReprojectionStart*` call and ends with `Stop` or `Finalize`.
Games that do not go through `Start` (or whose `Start` layout is not decoded) still get the
flipped frame split into two eyes in `Presenter::PrepareFrame`; there `BeginFrame` does its own
`xrWaitFrame` and the layer pose is the last one the tracker gave the game.

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

## Review and fixes (2026-09-22)

A review of the branch against the headset test found the causes of the comfort and frame-rate
problems in the code and in the M0 trace, and fixed them ahead of the next headset session.

### What the trace showed

**`sceHmdReprojectionStart` carries the render pose and projection.** Its second argument is
the head pose the frame was rendered with, in PSVR camera space: position xyz, orientation xyzw,
then a u64 timestamp at +0x20. In the trace it equals the pose `sceVrTrackerGetResult` had just
returned (including the 1.5 m camera offset). The first argument holds, at +0x18 and +0x28, one
quadruple per eye: `1/(tan_l+tan_r)`, `1/(tan_u+tan_d)`, `tan_l/(tan_l+tan_r)`,
`tan_u/(tan_u+tan_d)`, which is the projection scale and centre a game derives from
`sceHmdGetFieldOfView`. With the Quest 3 FOV reported (out 1.3764, in 0.8391, top 0.9657, bottom
1.4281) the left eye's quadruple was (0.4514, 0.4178, 0.6213, 0.4034) and the right eye's centre x
was 0.3787, which match exactly. So WipEout renders with the asymmetric native FOV, including the
vertical asymmetry, and the layer FOV was already the right one.

**The layer pose was wrong.** `EndFrame` submitted `render_sample`, the latest HMD sample given
to the game, which the render thread overwrites for the next frame before the GPU thread ends
the current one. The runtime therefore reprojected each frame from a pose about a frame ahead of
the one it was rendered with, in the direction of head motion: the world swims and the menus
wobble. Now the pose from `Start` is matched against the last 32 samples given to the game
(`VR::ResolveRenderViews`), so the layer gets the exact eye poses the game rendered from, and the
FOV from the `Start` projection. Decoding lives in `core/vr/hmd_frame.h` (unit tested); a layout
that does not decode falls back to the previous behaviour with a one-time warning.

**The game ran at exactly 30.0 fps.** `sceVrTrackerGetResult` was called at 30.0 Hz over every
20 s window of the log while the emulated vblank ran at 60 Hz, which is pacing, not load: the
game waits two flips per frame because a PSVR panel is scanned out at 120 Hz. The present
thread now runs the vblank at `hmd_refresh_hz` (120) while VR mode is active and returns to the
configured rate when it ends. Present mode Mailbox keeps the desktop mirror from throttling it.
The game's submit rate is logged every 5 s (`VR: game submits N frames/s`).

**`xrWaitFrame` blocked the GPU thread.** `BeginFrame` waited for the runtime's next frame slot
on the GPU command processor thread, delaying the game's next frame by up to a headset frame.
The wait now happens in `OnHmdFrameAccepted` on the game's render thread, right after the frame
is accepted, which is where the real library's scan-out would have blocked; the GPU thread then
begins the frame with the recorded frame state. The OpenXR spec allows this: `xrWaitFrame` may
be called from any thread and blocks only until the previous frame's `xrBeginFrame`.

**A flip during VR mode showed a black headset frame.** `PrepareFrame` began an OpenXR frame
for every flip even while `PrepareHmdFrame` was providing them, and ended it with no layers. It
now leaves the headset alone while `Start` frames are arriving.

**Eye resolution follows the reported panel size.** 1344x1512 is 1.4x half of 1920x1080, so
`render_scale` sizes the game's eye targets; 1.4 matches the Quest 3 and costs the same GPU time
as any 1.4x panel.

### Not fixed yet

- **Races run at 20 to 35 fps in the emulator, and that causes both remaining problems.** The
  menus hold 60.0 frames/s; a race drops to 20-35 and the game's dynamic resolution halves the
  eye (the screenshot hotkey saves the eye as the game rendered it: 1882x2117 in menus,
  944x1056 in a race, which is the blockiness). The GPU is not the limit: 17% busy on an
  RX 7800 XT, low CPU, so the emulator is stalling.

  **The stall is the image readback.** With `Readback Linear Images` off, the same race holds
  60.0 frames/s - and the game renders a black screen, which is why that setting is on. With it
  on, each `ProcessDownloadImages` waits for the GPU, and the wait drains everything recorded
  before it, so the command processor and the GPU never overlap. Measured per batch: four tiny
  images (8x8, 6x3, 2x2; 4 KiB in all) and 0.4 to 2.5 ms of waiting, several times a frame.
  The data volume is nothing; the pipeline drain is everything. Batching a batch's copies into
  one wait is committed, and is not enough on its own.

  Next things to try, cheapest first:
  1. Those images qualify only through the `width <= 8` clause in `FindTexture` and
     `FindRenderTarget`, not through the linear-image rule that fixes the black screen. Drop
     the tiny-image clause and check the game still draws: if it does, the stalls go with it.
  2. Write the downloaded bytes from `Scheduler::DeferPriorityOperation` instead of waiting
     (the `sync = false` path already there). Correct only if the EOP fence the guest waits on
     is also deferred until the GPU reaches that point; today it is written when the packet is
     parsed.
  3. Do the copy on its own queue so it waits for the image's last write rather than for the
     whole recorded frame.

- **Where the dynamic resolution decision comes from.** WipEout reads the **core clock**
  timestamps (`DataSelect::PerfCounter`, already scaled to 800 MHz - not the global clock,
  whose value shadPS4 writes in nanoseconds). Four timestamps cycle per frame, two per eye, and
  in a race at 30 fps the game measured about 4.9 ms and 5.6 ms per eye inside a 32.8 ms frame.
  shadPS4 writes those timestamps when the command processor *parses* the packet, so what the
  game measures is the emulator's translation cost, not the host GPU's. Once the frame rate is
  fixed this may resolve itself; if not, this is where to look.

## Headset test (2026-09-22, Quest 3 over Link)

Second session, with the review fixes in. Comfort, controllers and sound are done; resolution
and hitches are not.

| Check | Result |
|---|---|
| `Vblank now 120 Hz (VR mode)` | Yes |
| `VR: game submits ... frames/s` | 60.0 in menus (was 30), 45.0 in a race |
| `render views: pose from the game (matched a sample), fov from the game` | Every frame |
| Comfort | "very stable image" in menus and in a race; the pose mismatch is fixed |
| Touch controllers | `OpenXR: controllers active, mapped onto the DualShock 4`, no `no bindings for` lines, and the sticks and buttons drive the menus |
| Eye size at `render_scale` 1.4 | 1882x2117, against the runtime's 1872x2016 request |
| Sound | Fixed, see below |
| Image quality | Menus sharp, races blocky, worse with distance |
| Hitches | Present; playable if they were gone. Traced to the readback stall above |

What the fixes were:

- **Sound.** The game played to the Windows default output device, which was a DualSense's
  speaker, so the headset was silent while the mixer showed shadps4 producing level elsewhere.
  The Meta runtime names the headset's endpoint through `XR_OCULUS_audio_device_guid`; in PSVR
  mode every audio port but the pad speaker now opens that device while the output device
  setting is left at "Default Device" (main, Bgm and the Audio3d sink all did open it). A
  device chosen in the settings still wins.
- **The desktop mirror paced the headset.** AMD's Windows driver does not offer Mailbox, so the
  mirror fell back to Fifo, and the present thread that also drives the 120 Hz VR vblank waited
  for the monitor: 7 ms on the usual 144 Hz screen, 16.7 ms after the window moved to a 60 Hz
  4K monitor. In PSVR mode the fallback is Immediate; tearing in a mirror nobody is looking at
  costs nothing.
- **90 Hz against 60 fps.** 60 fps on a 90 Hz Link display judders, and `xrWaitFrame` pacing
  pinned races to exactly 45.0 fps (45 = 90/2: two 120 Hz vblanks, 16.7 ms, then the next 90 Hz
  slot at 22.2 ms). The session now asks for the highest offered rate that is a multiple of
  60 Hz. Over Link the runtime offers only the rate set in the Meta app, and with the headset
  set to 120 Hz the log reads `display refresh 120 Hz, available 120`.

Measured at the race start, the clearest form of the resolution problem: at the "START RACE"
prompt the eye screenshot is 1882x2117 and crisp; a few seconds into the race the same cockpit
is 944x1056, with the game submitting 22 to 35 frames/s. The game halves its own scene the
moment the emulator stops keeping up.

Three experiments that failed, so they are not worth repeating:

- **Neo (PS4 Pro) mode**, which the game would render at a higher resolution for: shadPS4 aborts
  during the boot sequence in `TextureCache::ResolveOverlap`, "Unreachable code! Encountered
  unresolvable image overlap with equal memory address" - the game puts a larger image at the
  address of a smaller one whose resource count it does not exceed, which is the case that falls
  through to the `UNREACHABLE`. Returning `ExpandImage` there instead gets the game running at
  60 frames/s for a few seconds and then it crashes hard with nothing in the log. Neo mode is
  worth having (it is the version of the game that renders more pixels), but it needs the
  texture cache understood, not the assert patched.

- **Dropping the `width <= 8` clause** that puts tiny images in the readback set: races run at
  60.0 frames/s and render black. Those 8x8 images are exactly what the black screen fix is
  about, so they have to be read back.
- **Writing the readback bytes from `DeferPriorityOperation` instead of waiting**: the emulator
  crashed on its own within a minute of a race. The staging buffer's tick watches make the
  buffer safe, so the fault is elsewhere in doing this off the command processor thread.

Operational notes from this session: the headset's own `adb logcat` VrApi lines report the Link
rate and dropped frames (`FPS=69/90 ... Stale=20`), and `adb shell screencap` shows what the
headset is actually displaying, which is how the blockiness was measured without wearing it.
A Windows session that is locked or disconnected refuses screen capture and key injection, so
driving the game from a tool call needs the session unlocked.

## Coordinate mapping

Both sides are right-handed, +Y up, -Z forward, metres. OpenXR's LOCAL origin is the head's start
pose; PSVR poses are relative to the camera in front of the player. The tracker adds
`VR::CameraDistance` (1.5 m) along +Z to every pose it returns, and nothing else.
