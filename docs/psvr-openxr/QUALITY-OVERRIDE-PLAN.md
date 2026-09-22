# Forcing the highest shipped quality: analysis and plan

Written 2026-09-22 against branch `psvr-openxr` at 323cba5e (the private remote is at the same
commit, the tree is clean, and neither of the two problems handed over in IMPLEMENTATION.md
"Not fixed yet" has been touched since).

The question this answers: can the emulator stop a game from dropping its own quality, and more
generally make every game render the best version of the assets it ships, sized to the host GPU
rather than to a 2013 console? Short answer: yes for most of it, because the inputs a game uses to
make those decisions all pass through the emulator; the plan below lists them and how to bend
each one. What the emulator cannot do is invent detail the disc does not contain, and temporal
upscalers (DLSS, FSR 2/3, XeSS) are not usable in an emulator at all. Section 2 explains why and
what to use instead.

## 1. Why WipEout halves its eye target in a race

### What the code says

- **The game sizes its eye from the panel we report.** `VR::GetPanelResolution`
  (src/core/vr/vr_service.cpp:182) returns 1920x1080 times `render_scale`, and the game's eye
  T# is 1.4 x half of that. It has exactly two sizes: that "full" size and half of it.
- **The GPU time it measures is CPU parse time.** The four EOP timestamps per frame use
  `DataSelect::PerfCounter`, served by `GetGpuPerfCounter()` (src/video_core/amdgpu/pm4_cmds.h:347):
  the host TSC rescaled to 800 MHz, sampled when the command processor parses the packet. The
  host GPU is never consulted.
- **The readback stall sits inside the measured interval.** On every `EventWriteEop`,
  `EventWriteEos` and `ReleaseMem` (src/video_core/amdgpu/liverpool.cpp:674, :654, :1098) the
  processor first calls `rasterizer->ProcessDownloadImages()`, which drains the GPU, and only
  then samples the timestamp in `SignalFence`. The 250 ms per second of drain time measured in
  a race is therefore counted as GPU time by the game.
- **`GpuClock64` is in the wrong units.** `GetGpuClock64()` returns nanoseconds since the epoch;
  the GCN global counter is a reference-clock counter (100 MHz class). WipEout does not read it,
  but any engine that does (UE4's GPU profiler reads both kinds) sees a clock ten times too fast
  and will make the same kind of decision wrongly.

### Two hypotheses, one experiment apart

The doc's measurements fit two different mechanisms, and the plan diverges on which is true:

- **H1, timestamp-driven dynamic resolution.** The game compares per-eye GPU time against a
  budget with hysteresis and steps between full and half. Consistent with: full size held at the
  START RACE prompt (little geometry), half chosen a few seconds into the race (whole track in
  view), and the decision not following the frame rate (the frame rate is set by the emulator's
  stalls, which are only partly inside the measured intervals).
- **H2, a fixed platform policy.** The engine renders races at half resolution on a base PS4 in
  VR regardless of measurement, with menus and pre-race at full. Consistent with the same
  observations, and with a shipped title that had to hold 60 fps on a base PS4.

Experiment 0b below separates them in one run: make the reported GPU time tiny and see whether
the halving stops. Everything else in the plan is worth doing under either hypothesis.

## 2. What "highest quality" means inside an emulator

A game's quality decisions come from a short list of inputs, all of which the emulator owns:

| Input | Where it is served | Games use it for |
|---|---|---|
| Neo (PS4 Pro) flag | `sceKernelIsNeoMode`, src/core/libraries/kernel/process.cpp:19 | Pro render targets (1440p to 4K), higher settings, more memory |
| GPU core clock | `sceGnmGetGpuCoreClockFrequency` (911 MHz on Neo, else 800) | Converting timestamps to milliseconds |
| EOP timestamps | `PerfCounter` and `GpuClock64` in pm4_cmds.h | Dynamic resolution, adaptive quality, profilers |
| Flip status | `sceVideoOutGetFlipStatus`, `sceVideoOutIsFlipPending` | Missed-vblank detection driving dynamic resolution |
| Vblank rate | `vblank_frequency`, `hmd_refresh_hz` | Frame pacing, PSVR 120 Hz reprojection budget |
| HMD panel size and FOV | `sceHmdGetDeviceInformation`, `sceHmdGetFieldOfView` | VR eye target size, projection |
| Display mode | VideoOut resolution and HDR status | 4K output paths, HDR |
| Occlusion queries | `EventWrite ZpassDone` (currently a fake counter) | Visibility, sometimes LOD |

Everything the game then chooses from is data on the disc: mip chains, LODs, shadow-map sizes,
particle budgets. Forcing quality is therefore three different jobs:

1. **Make the capability inputs read "fast hardware".** Generic, emulator-wide, one setting
   group. This is where most of the value is, for every game.
2. **Patch hard-coded policies per title.** When a game does not measure but simply branches on
   the platform, the only lever is a memory patch. shadPS4 already has that machinery
   (src/common/memory_patcher.cpp loads per-title JSON patches).
3. **Improve what leaves the emulator.** Sharpening and spatial upscaling on the output image.
   Cheap, already partly in the tree, and the only place FSR belongs.

A fourth, true internal-resolution scaling of every render target as Xenia and RPCS3 do, is the
lever for games that hard-code sizes and cannot be patched. It is a large texture-cache project
and it is not needed for WipEout, which happily renders any size we tell it the panel is.

### FSR and DLSS, plainly

- **shadPS4 already ships FSR 1** (EASU upscale plus RCAS sharpening, src/video_core/host_shaders/fsr.comp,
  `fsr_enabled` / `rcas_enabled` / `rcas_attenuation`). It runs on the desktop mirror, including
  in VR mode (vk_presenter.cpp:943). The headset eyes bypass it: vr_pass.cpp:180 copies each eye
  into the OpenXR swapchain with a plain bilinear `blitImage`.
- **DLSS 2, FSR 2/3 and XeSS are temporal.** They need per-pixel motion vectors, depth, and a
  sub-pixel jitter injected into the game's projection matrix every frame. An emulator sees only
  a stream of draw calls; it cannot identify which buffer is the motion-vector target or change
  the game's projection. There is no generic way to feed them. DLSS also has no spatial-only
  mode; NVIDIA's spatial option is NIS, which is FSR 1's peer and adds nothing over what is
  already in the tree.
- **Frame generation is a runtime feature in VR.** DLSS 3 and FSR 3 frame generation need the
  same motion vectors. But the OpenXR runtime already does compositor-side synthesis with no
  input from the app: Oculus ASW over Link, SteamVR Motion Smoothing. A game held at a steady
  60 fps on a 120 Hz session gets synthesised to 120 by the runtime. That is the frame
  generation we can have, and it is free.
- **Upscaling cannot raise the frame rate here.** The RX 7800 XT is 17 to 20 percent busy in a
  race; the frame rate is set by emulator stalls (IMPLEMENTATION.md "Not fixed yet" 2). The
  GPU has room to render more pixels natively, so the right move is supersampling through the
  game's own target size, then RCAS on the output, not upscaling from a smaller target.
- **The RTX 2070 Super** is useful for two things: NVIDIA's driver offers Mailbox presentation,
  which exercises the other branch of the mirror pacing fix, and it is slow enough to be GPU
  bound at high scales, which tests that the headroom spoof degrades gracefully instead of
  producing a slideshow. It is not a DLSS test bed for the reasons above.

## 3. Plan

### Phase 0: diagnose (one session on MINI, no code beyond one setting)

- **0a. `render_scale: 2.8`.** The untried experiment from the handover. Expected: menus at
  3763x4234, races halved to 1882x2117, which matches the Quest 3 swapchain. Record eye size
  (F12 screenshot and the `eyes:` trace line) and frames per second at both.
- **0b. GPU time spoof.** Add `gpu.gpu_time_scale` (float, default 1.0) and apply it inside
  `GetGpuPerfCounter()` as `base + (cycles - base) * scale`, with `base` captured at first use
  so the counter stays monotonic and only elapsed time shrinks. Run a race at 0.1. If the race
  stays at full size, H1 holds and Phase 1 is the fix. If it still halves, H2 holds and the fix
  is a patch (Phase 2). One build, one race, decisive.
- **0c. Trace what the game measures.** A `Render` trace line per EOP `PerfCounter` write with
  the delta from the previous one, so the per-eye numbers the game sees are in the log next to
  the resolution changes. Also confirm from the log that `sceVideoOutGetFlipStatus` and
  `sceVideoOutIsFlipPending` are not consulted during a race, which rules out the flip-based
  mechanism for this title.

### Phase 1: a generic "GPU headroom" model (emulator, every game)

1. **Sample the timestamp before the drain.** In the three `ProcessDownloadImages` sites, take
   the TSC before the drain and hand it to `SignalFence`. Readback data must still land before
   the fence value, so the order of the writes does not change; only the sampled time does.
   This alone removes the emulator's 250 ms per second stall from what any game measures.
2. **Keep the spoof as a setting** (`gpu_time_scale`, per-game overridable) with a documented
   meaning: how much faster than a PS4 the game should believe the GPU is. A "max quality"
   profile sets it low; the default stays 1.0 so upstream behaviour is unchanged.
3. **Fix `GpuClock64` units** to a reference-clock counter (100 MHz), scaled by the same
   factor. Cheap, and it stops other engines from misreading the clock.
4. **A correct model later, not now.** Writing host GPU timestamps (`vkCmdWriteTimestamp`)
   into the EOP address would be the real emulation, but the EOP fence is written at parse time
   and the guest reads the timestamp after the fence, so the value would have to be written
   before the host GPU has produced it. Making fences signal at host completion is the same
   ordering change that crashed the deferred-readback experiment, and it adds latency to every
   fence spin. Park it behind the scaled model, which games cannot distinguish from a fast GPU.
5. **Neo mode in the max profile.** The biggest generic lever for flat games, blocked by the
   `TextureCache::ResolveOverlap` abort (texture_cache.cpp:477) that WipEout hits at boot in
   Neo mode. The failing case is a larger image arriving at the address of a smaller one with
   no more subresources; the fix is to expand the cached image and copy or invalidate its
   contents, and then to find the second crash that `ExpandImage` alone exposed after a few
   seconds. This is texture-cache work and gets its own session.
6. **`render_scale` auto.** Derive the panel scale from the runtime's recommended eye size at
   session start (Quest 3 over Link 1872x2016, SteamVR 1996x2156) so the game's half or full
   step lands on the swapchain size without hand tuning, and persist the choice into the
   per-game config so the user can override it. The `hmd_refresh_hz` and FOV pass-through
   already in the bridge stay as they are.

### Phase 2: per-title policy patches (only if 0b says H2)

- Locate the code that consumes `panel_resolution` from the `sceHmdGetDeviceInformation` struct
  and follows it to the 0.5 factor: dump the loaded module, find the readers of the two u32s
  at the panel-resolution offset, and follow the eye-size computation to the branch that picks
  half. The patch forces the full-size branch.
- Ship it through the existing patcher as a `CUSA05670` entry (title id plus version 1.07) so it
  is toggled per game like the 60 fps patches in the community patch repo, not compiled in.
- The same route covers any game-side "platform == base" branch that lowers LOD, shadow size or
  effect budgets: those are policy, not measurement, and only a patch reaches them.

### Phase 3: output-side quality (cheap, in-tree pieces)

- **Run the headset eyes through `FsrPass`** instead of the bilinear blit in vr_pass.cpp:180.
  EASU plus RCAS when the eye is smaller than the swapchain, RCAS only when it is equal or
  larger (a supersampled eye should be sharpened, not resampled). Reuse the presenter's
  settings so the existing `fsr_enabled` / `rcas_*` keys apply to both outputs.
- **Frame generation through the runtime:** document how to enable ASW over Link and Motion
  Smoothing in SteamVR, and make the emulator's pacing hold a steady 60 when it cannot hold 120,
  since the runtime synthesisers need a stable half rate to lock on to.

### Phase 4: the frame-rate problem, unchanged

The readback stall (IMPLEMENTATION.md "Not fixed yet" 2) is what limits WipEout's race frame
rate and it is separate from everything above; the plan there is still to do the copy on its
own queue so it waits for the image's last write and not the whole recorded frame. Phase 1
step 1 has a side effect worth noting: once the timestamp is sampled before the drain, the
game stops seeing the stall, so any dynamic-resolution decision stops reacting to it even
before the stall itself is fixed.

### Phase 5: true internal-resolution scaling (long term, not for WipEout)

Scale every render target, viewport and scissor by a factor and rewrite the shaders that read
render-target dimensions. Xenia and RPCS3 both did this and both spent a long time on
screen-space effects and texture aliasing. It is the only lever for games that hard-code target
sizes and resist patching, and it should wait until Phases 1 and 2 have shown which games those
actually are.

## 4. Settings this adds

| Setting | Default | Effect | Acceptance test |
|---|---|---|---|
| `gpu.gpu_time_scale` | 1.0 | Scales elapsed GPU time reported by EOP timestamps | 0b: a race at 0.1 holds full size if H1 |
| `gpu.timestamp_before_readback` | on | Samples EOP time before the readback drain | Per-eye deltas in the 0c trace drop by the drain time |
| `general.neo_mode` in the max profile | off | PS4 Pro paths | WipEout boots and runs a race in Neo mode |
| `vr.render_scale = auto` | 1.4 | Panel scale from the runtime's eye size | `eyes:` line matches the swapchain size in a race |
| `vr.eye_upscale` | fsr | Eye to swapchain through FsrPass | RCAS visibly sharper at equal size, no seam |
| `quality_profile` | psvr | Bundle: neo on, time scale low, render_scale auto | One switch applies all of the above |

## 5. Order and expected outcome

1. Phase 0 on MINI in one session: 0a, then 0b and 0c from one build. This decides between
   Phase 1 and Phase 2 for WipEout and also produces the first sharp race either way (2.8 lands
   the half step on the headset's native size, at a GPU cost the 7800 XT has room for).
2. Phase 1 steps 1 to 3 are a day: three call sites, one setting, one unit fix. They are the
   generic deliverable and belong upstream eventually (with the default at 1.0 they change no
   existing behaviour).
3. Phase 3's eye upscale is an afternoon and improves every headset frame.
4. Neo (Phase 1 step 5) and the readback queue (Phase 4) are the two real projects; both are
   texture-cache and scheduler work, and both are already scoped in IMPLEMENTATION.md.
5. Test on the RTX 2070 Super after Phase 1 to confirm the profile degrades to a lower
   `render_scale` on a slower GPU instead of stalling.
