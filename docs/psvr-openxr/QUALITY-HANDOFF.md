# Where the quality work stopped (2026-09-23)

Read this first, then QUALITY-OVERRIDE-PLAN.md ("Phase 0 results" and the image notes under it).

## Branches

- `quality-override` (this branch) holds everything here: the plan, `gpu_time_scale`, the EOP
  timestamp changes, the `IMAGE_SAMPLE_CD` recompiler fix and these tools. Pushed to `origin`
  (hmofet/shadPS4) and `private` (hmofet/shadps4-psvr-openxr). Never push to `upstream`.
- `psvr-openxr` is PSVR support only, meant for an upstream PR. Keep quality and render work off
  it. It has moved on since this branch was cut (this branch is based on 6844b31d; the PSVR
  session has added commits up to at least 51b38af3), so rebase `quality-override` onto
  `private/psvr-openxr` before the next build, and push with `--force-with-lease`.
- The pre-split head is kept as tag `backup/psvr-openxr-3242a1b9`.

## Done

- **Phase 0 verdict H1:** WipEout's eye size is a 16-step dynamic resolution driven by the EOP
  `PerfCounter` timestamps. `gpu_time_scale` 0.1 holds the full 1882x2117 eye in a race.
- **Black track fixed** (4f7b5b57): the recompiler emitted `IMAGE_SAMPLE_CD` as a plain sample, so
  gradients were read as UVs. Confirmed in game on Vineta K. Also a bug in upstream shadPS4.

## Next, in order

1. **Rebase** `quality-override` onto `private/psvr-openxr`, build, and check a race still renders
   (black track stays fixed, eye stays full size).
2. **Posterization** (open): hard colour bands in the sky and steps in dark areas, before and
   during races. The eye scene target is R11G11B10_FLOAT with 4x MSAA, resolved to a
   single-sample R11G11B10 copy, then tonemapped into the R8G8B8A8_SRGB eye array (2 layers).
   Take a capture, find the pass where smooth gradients become bands (PickPixel along a sky
   gradient on each target in turn), then read that pass's shader and formats. Suspects: a
   resolve or copy that goes through an 8-bit or wrongly typed view, or an sRGB/UNORM mix-up
   in the tonemap output. Two old pre-race captures are in `%APPDATA%\shadPS4\captures`
   (`CUSA05670_capture.rdc`, `_2.rdc`, about 5 GB each; `_3.rdc` is the tunnel race frame used
   for the track bug). They predate the fix, which is fine for this bug; delete them when done.
3. **Upstream PR for the recompiler fix:** cut a branch from `upstream/main`, cherry-pick
   4f7b5b57 alone, push to `origin`, and open the PR from there (ask the user before opening it).
4. Plan items still ahead: Phase 1 step 6 (`render_scale` auto), Phase 3 (headset eyes through
   FSR/RCAS). Phase 4 (readback stall) and Neo mode are their own projects.

## Machine state left behind

- `%APPDATA%\shadPS4\custom_configs\CUSA05670.json`: `render_scale` 1.4, `gpu_time_scale` 0.1,
  `pose_source` `openxr` (headset), `renderdoc_enabled` false. For desk-mode captures set
  `"pose_source": "desk"` and `"renderdoc_enabled": true` (see below); set them back afterwards.
- `%APPDATA%\shadPS4\input_config\global.ini` has `hotkey_capture_frame = back`: the DualSense
  Create button does what F12 does (backup `global.ini.bak-phase0`).
- RenderDoc 1.46 is installed (winget), analytics opted out in `%APPDATA%\qrenderdoc\UI.config`.
- MINI hard-rebooted on 2026-09-22 with a WHEA Machine Check (CPU cache hierarchy error), its
  third this month. Hardware or BIOS stability (PBO/Curve Optimizer, EXPO), not shadPS4.

## How to capture and analyse without the headset

Tools are in `tools/quality-debug/`. Paths in them are for MINI.

- **Session:** key injection and window capture need a connected, unlocked console session.
  `query user` must show `Active`; after a crash it can come back `Disc`, and then presses and
  screenshots silently do nothing.
- **Build:** `build.bat` (vcvars64 + `cmake --build` for the emulator and the settings tests).
  Run it through the PowerShell tool as `cmd /c "<bat> > <log> 2>&1"`.
- **Launch:** `Remove-Item Env:XR_RUNTIME_JSON`, then
  `Build\x64-Clang-RelWithDebInfo\shadps4.exe -g C:\Users\arinb\Documents\shadPS4-games\CUSA05670\eboot.bin`.
  Only one shadPS4 at a time: another session runs PSVR titles on this machine, so check
  `Get-Process shadps4` (its build lives in `shadPS4-psvr`).
- **Drive:** dot-source `drive.ps1`, then `Focus`, `Key <name> [holdMs]`, `KeyDown`/`KeyUp`,
  `Shot <png> [scale]`. Keys: `n` = cross, arrows = D-pad, `enter` = options, `f12` = capture or
  screenshot. To reach a race from the main menu: `right`, `n` (RACEBOX), `n` (settings),
  `n` (track Vineta K), `n` held 200 ms (ship), wait about 25 s, then the START RACE prompt;
  hold `n` to start and accelerate. Turn the DualSense off or leave it untouched (a held input
  fought the keys at ship select), and do not switch it off while the game runs: that aborts
  shadPS4 (`controller.h:140 Unreachable`).
- **Capture:** with RenderDoc loaded the game runs at 10 to 20 fps; hold keys longer. F12 writes
  `%APPDATA%\shadPS4\captures\CUSA05670_capture*.rdc`. Without RenderDoc, F12 saves a game-only
  screenshot to `%APPDATA%\shadPS4\screenshots`, and `Shot` returns black (use F12 instead).
- **Analyse:** run a script with `qrenderdoc.exe --python <script>`; each reads `RD_CAP` and
  `RD_OUT` (plus its own variables, see the header of each) and writes text or PNGs there.
  - `rd_dump.py`: every action with its outputs, formats, and the final colour targets as PNG.
  - `rd_hist.py`: pixel history on the 1882x2117 R11G11B10 targets (`RD_PIXELS="x,y;x,y"`).
  - `rd_draw.py`: a draw's bound textures with min/max per mip and slice, and its disassembly.
  - `rd_tex.py`: a draw's pixel-shader textures as RGB and alpha PNGs, with view swizzles.
  - `rd_spv.py`: a draw's pixel shader as SPIR-V; decompile with
    `plugins\spirv\spirv-cross.exe <spv> --vulkan-semantics --output <frag>`.
  - `rd_replace.py`: replace that shader with edited versions (`RD_GLSL`, `;`-separated) and
    report pixels on `RD_SCENE` plus the final eye `RD_EYE`. Compile edits with
    `plugins\spirv\glslangValidator.exe -V --target-env vulkan1.1 -S frag <frag> -o <spv>`:
    RenderDoc's own GLSL build targets too old a SPIR-V for these shaders.
  - `rd_debug.py`: RenderDoc's pixel debugger; returns no trace for WipEout's shaders (they use
    `SPV_AMD_shader_trinary_minmax`), so bisect with `rd_replace.py` instead.
  - Resource ids differ per capture: run `rd_dump.py` first and take ids from its output.
