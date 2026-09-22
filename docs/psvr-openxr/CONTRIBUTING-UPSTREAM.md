# Path to an upstream pull request

The work lives on the `psvr-openxr` branch of the fork and tracks `upstream/main`.
It will be proposed upstream only when it meets the bar below, and in small pieces.

## Quality bar

- Follows the repository's existing conventions: HLE library layout under
  `src/core/libraries/`, NID tables, `LOG_*` categories, clang-format, no warnings.
- OpenXR is optional at compile time and at run time. Without a runtime, behaviour is
  identical to today.
- No game-specific logic in library code. Per-title needs go into config or patches.
- Each PR is independently useful and reviewable, with a description of the tested
  titles and runtimes.
- No leaked SDK material. Struct layouts and semantics come from public reverse
  engineering and from observing game behaviour, consistent with how the existing
  stubs were written.

## PR sequence (proposed)

1. **Trace logging** for Hmd/HmdReprojection/VrTracker/Camera with decoded arguments. Pure diagnostics.
2. **Device presence and setup dialog**: config-gated "report a connected PSVR" that lets games enter VR mode and render stereo into the window. Useful on its own for research and for 2D-VR titles.
3. **VrTracker poses from an abstract pose source**, with a keyboard/mouse "desk mode" source. Still no OpenXR dependency.
4. **OpenXR runtime + submit path** (the big one), `XR_KHR_vulkan_enable2` integration in the Vulkan backend.
5. **Input mapping and social screen.**
6. Enhancement toggles (render scale, FOV mode, refresh hint).

## Housekeeping

- Rebase on `upstream/main` regularly; keep the branch free of merge commits.
- Keep this `docs/psvr-openxr/` folder out of the upstream PRs unless maintainers want it; the PR descriptions carry the relevant parts.
- Coordinate on the project Discord before opening PR 4; ask which maintainer owns the Vulkan backend.
