# PSVR → OpenXR bridge for shadPS4

Work-in-progress branch `psvr-openxr` in a private copy of shadPS4 (hmofet/shadps4-psvr-openxr), tracking upstream `main`. It becomes a public fork only when ready for review.

Goal: make PSVR titles playable on modern PC VR hardware (PSVR2 with the PC adapter,
Quest 3 via Link / Virtual Desktop, Valve headsets) by implementing the PSVR system
libraries on top of OpenXR instead of emulating the PSVR hardware. Intended for an
upstream pull request once the quality bar in `CONTRIBUTING-UPSTREAM.md` is met.

| Doc | Contents |
|---|---|
| [DESIGN.md](DESIGN.md) | Architecture, per-library plan, frame flow, milestones |
| [WIPEOUT-TEST-TITLE.md](WIPEOUT-TEST-TITLE.md) | WipEout Omega Collection as the first test title: what it calls, what it needs |
| [ENHANCEMENTS.md](ENHANCEMENTS.md) | Beyond parity: resolution, refresh rate, FOV, quality options for modern headsets |
| [CONTRIBUTING-UPSTREAM.md](CONTRIBUTING-UPSTREAM.md) | What "acceptable for upstream" means and how the work will be split into PRs |

Test titles planned, in order: WipEout Omega Collection (v1.07), Rez Infinite, Astro Bot Rescue Mission, then camera/Move-heavy titles.
