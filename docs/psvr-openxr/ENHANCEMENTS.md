# Enhancements beyond PSVR parity

PSVR (2016) is a 1920x1080 RGB panel, ~100° FOV, 90/120 Hz with console-side
reprojection. Modern headsets (PSVR2 2000x2040 per eye, Quest 3 2064x2208 per eye,
Valve Index/Frame class) leave headroom. These are the levers, roughly in order of
value per effort. Game-agnostic ones belong in the bridge; game-specific tuning goes
into per-title config or patches, never into library code.

## 1. Render resolution (bridge, generic)
Games size their eye render targets from `sceHmdGetDeviceInformation` panel size and
from `neo_mode`. Options:
- Report a larger panel (`vr.render_scale`, e.g. 1.5–2.0x) so games that derive their
  target size from it render higher. Some titles hard-code sizes; for those use the
  emulator's internal-resolution scaling on the render targets themselves.
- Expose `neo_mode` (PS4 Pro) by default in VR; many PSVR titles pick larger targets on Pro.

## 2. Field of view (bridge, generic)
Return the real headset's FOV tangents from `sceHmdGetFieldOfView` instead of the
PSVR constants. Games that build their projection from these values get the full FOV
of the headset for free. Guard with `vr.fov_mode = native | psvr` for games that bake
PSVR assumptions into culling or UI placement.

## 3. Refresh rate (bridge, generic + per-title)
`vr.refresh_hint` raises the guest vblank (60 → 90/120). Titles whose simulation is
decoupled from frame rate (WipEout's game time is largely FPS-independent per
compatibility reports) run smoother; titles with frame-locked logic stay at 60 and
rely on runtime reprojection.

## 4. Anti-aliasing and sharpening (emulator, generic)
Supersampling via render scale; the existing RCAS sharpening; FSR already present in
the GPU settings. Temporal solutions are out of scope.

## 5. Late-latching / reprojection quality (bridge, generic)
Submit layers with the exact pose used for rendering and let the runtime reproject.
Optionally sample the pose as late as possible (`xrLocateViews` right before the game
reads `GetResult`) to cut motion-to-photon latency.

## 6. Social screen / spectator (bridge, generic)
Mirror choices: left eye, right eye, both, or the game's social-screen buffer.

## 7. WipEout-specific (per-title, outside the bridge)
- Cockpit and VR ship classes are already in the data; verify they load when VR is reported.
- `ComfortVRFoV` / `ComfortVRMask` are the game's own comfort settings; expose nothing extra.
- Lighting over-exposure in flat mode is an emulator rendering bug and will show in VR as well; track separately.
- Possible shader-level patches (bloom tuning for higher resolution) via the existing shader patch mechanism.

## 8. Not planned
Eye-tracked foveation, hand tracking, and haptics beyond the DualShock rumble: no PS4 API surface to map them to.
