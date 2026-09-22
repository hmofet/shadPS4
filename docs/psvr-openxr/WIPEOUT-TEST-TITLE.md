# Test title: WipEout Omega Collection (CUSA05670, v1.07)

VR mode was added in patch 1.05 (March 2018). The base v1.00 disc/PKG has no VR
code path, so the update is mandatory for testing. The EU title ID is CUSA05670; the US
one is CUSA07671. Both carry `PSVR Supported: true` in `param.sfo`.

## Flat-mode baseline (shadPS4 v0.18.0)

- Boots to the main menu, 60 FPS, 1920x1080 A2R10G10B10 flip.
- Required settings: **Readback Linear Images** (otherwise black screen in races) and
  **shader cache** (stutter). See compatibility issue
  [shadps4-game-compatibility#487](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/487).
- Known upstream issues: over-exposed lighting on tracks, occasional menu model
  unload failures, Zone mode crash in HD/Fury on some Windows 11 machines.
- Emulator log: the game probes loose files under `/app0/data/...` before falling back
  to its PSARC archives, so "file does not exist" lines are normal.

## VR libraries the game imports

`libSceHmd`, `libSceHmdSetupDialog`, `libSceVrTracker`, `libSceCamera`,
`libSceAudio3d`, `libSceSocialScreen`. Functions referenced (from error strings):

| Library | Functions |
|---|---|
| Hmd | Initialize(315), Open, Close, GetDeviceInformation, GetDeviceInformationByHandle, GetFieldOfView |
| HmdReprojection | Initialize, SetOutputMinColor, Start, Stop |
| HmdSetupDialog | Initialize, Open, GetResult, Close, Terminate |
| VrTracker | QueryMemory, Init, RegisterDevice, UnregisterDevice, GpuSubmit, GpuWaitAndCpuProcess, GetResult, UpdateMotionSensorData, Recalibrate, GetPlayAreaWarningInfo |
| Camera | Open, Close, SetConfig, SetVideoSync, Start, Stop, GetFrameData |
| Audio3d | PortCreate, PortDestroy, PortSetAttribute, ObjectReserve, ObjectUnreserve, ObjectSetAttribute(s), Terminate |

## Observed boot sequence (flat)

1. `sceHmdInitialize315` → "PSVR headsets are not supported yet"
2. `sceHmdReprojectionInitialize` (stub), `sceHmdReprojectionSetOutputMinColor` (stub)
3. `sceCameraIsAttached` from the render thread
4. `sceSocialScreenInitialize` (generic stub)
5. Game proceeds without a VR menu entry.

Expected VR-mode entry once presence is faked (to be confirmed by M0 instrumentation):
`HmdSetupDialog` → `sceHmdOpen` → `GetFieldOfView` → `VrTrackerQueryMemory/Init/RegisterDevice(HMD, pad)` → `Camera Open/SetConfig/Start` → per frame `GpuSubmit` / `GpuWaitAndCpuProcess` / `GetResult` → `HmdReprojectionStart`.

## Why it is a good first title

- Head tracking only (no Move, no gun), DualShock input, a well-behaved 60 Hz renderer.
- Uses only the simple `Start`/`Stop` reprojection variants (no multilayer).
- Fast to iterate: menu in ~10 s, a race in ~30 s.
- Dedicated VR content (cockpit view, VR ship classes, VR bloom/comfort settings) makes visual regressions obvious.
