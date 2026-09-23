# DragonBox Pyra port — documentation index

Eleven reports, written as the work happened. They are a record rather than a manual, which means
**the later ones correct the earlier ones** in several places. This index exists so that nobody
reads a superseded number and acts on it.

**If you are looking for what to do rather than what happened, read [`NEXT.md`](NEXT.md).** It is
the live work queue and it is rewritten as things land; where it disagrees with a report, it is
newer.

**If you are about to measure something, read [`TESTING.md`](TESTING.md) instead of report 9.** It
is the operating manual for the harness — setup, the three fixtures, the four commands, how to
read the output, and the traps that have each cost a session. Report 9 explains why it works;
`TESTING.md` is how to use it.

**If you are starting a fresh session on performance, [`PROMPT.md`](PROMPT.md) is the paste-ready
brief** — the current task, the device rule, what not to redo. Rewrite its task section when that
task lands; a stale prompt is worse than none.

## Scope: this fork targets the Pyra only

Decided 2026-09-22. Earlier reports were written under a "keep changes upstreamable" constraint
and several of their recommendations exist only to satisfy it — most explicitly report 4, which
argued for putting the GLES2 backend in libultraship behind `USE_OPENGLES2` rather than forking it,
on the grounds that *"I think you'd regret it at upstreaming time"*. That argument no longer
applies. Where a report justifies a choice by upstreamability alone, the justification is void;
where it justifies one on cost, risk or clarity, it still stands.

What this changes in practice:

- **Defaults may be Pyra-tuned directly.** No profile or platform-conditional machinery is needed
  to ship a setting this device wants. The first of these is `CVAR_TEXTURE_FILTER`, now defaulting
  to `FILTER_LINEAR` in `Fast3dWindow.cpp` — worth 19.88% of frame time (§8.14) at no visible cost
  (§8.13).
- **The "upstreamable?" column in report 8 §8.8 is no longer a ranking input.** Rank on measured
  benefit, cost and risk alone.
- **Device-specific assumptions are allowed.** SGX544MP2, OMAP5432, GLES2, 640×360 and the DDK's
  SONAME quirk can be assumed rather than abstracted.

What it does *not* change:

- **Existing abstractions stay.** `USE_OPENGLES2` and the `PYRA` CMake option already work;
  removing them would be effort spent to gain nothing. Stop paying *new* costs for upstreamability,
  do not go back and undo the old ones.
- **Pulling from upstream still has value and still has a price.** This port is itself a backport
  of upstream PR #1288 (report 2 §2.2). Divergence makes each future pull more expensive, and
  Ship of Harkinian is actively developed. That cost is now accepted deliberately rather than
  avoided.
- **Bugs that are upstream's should still be reported upstream.** Report 1 §1.2 found a guard that
  has never been buildable on any 32-bit ARM target, and report 5 §5.2 a latent colour-combiner
  gap on every platform. Reporting those costs nothing and is not contingent on merging.

## The hardware knowledge base lives in another repository

`~/git/dragonbox-pyra/docs/` holds the system-level work on this machine, and since 2026-09-22 it
carries a hardware playbook that covers both projects:

- [`optimization-playbook.md`](../../../dragonbox-pyra/docs/optimization-playbook.md) — index, and
  a triage of every optimisation proposed on this device, system and application alike. It is the
  only document that reads the two projects against each other, and doing that immediately caught
  a contradiction neither had noticed (item 7, below).
- `hardware-overview.md` + `known-limitations.md` — the minimum pair for context: the first builds
  the model, the second stops closed questions being re-derived.
- `gpu-optimization.md` + `rendering-guidelines.md` — read before touching the renderer.
- `profiling-guide.md` — read before measuring anything.

Their convention is worth adopting here too: every claim marked **Measured**, **Spec** or
reasoning, and corrections kept visible rather than edited away.

The reports in *this* directory remain the source of truth for the port itself; the playbook's
application-level triage (§4) is written against them and will lag them. Where the two disagree,
check the date.

## Read this first: which numbers are still good

| Claim | Where | Status |
|---|---|---|
| Device limits, extensions, GLES2 caps | 00, 05 | **current** — measured on hardware, unchanged |
| "GPU has ~15× fill-rate headroom, bottleneck is CPU-side" | 05 §5.2-5.3 | **wrong** — corrected by 07 §7.3 |
| Resolution sweep ratios (+75% halving resolution) | 07 §7.2 | **current** — structural |
| Absolute FPS figures | 07 | **superseded** — coarse rig, and predate the audio DSP move (07 §7.8) |
| `GL_EXT_discard_framebuffer` is a cheap win | 00, 04 | **wrong** — implemented and measured at no gain, 08 §8.10 |
| Optimisation ordering `4 → 6 → 5 → 3 → 1 → 2` | 08 §8.8 | **wrong** — corrected in 08 §8.9 |
| Item 1's estimated gain ("large") | 08 §8.2 | **overstated** — corrected in 08 §8.9 |
| Item 9 retired by the audio DSP move | — | **no** — see 08 §8.11 |
| Benchmark harness resolves better than ±13% | 09 §9.7 | **validated on hardware** — 0.57% at the current fixture with the CPU pinned, 09 §9.5 |
| "Items 5 and 6 are below the measurement floor" | 08 §8.9 | **no longer true** — the floor moved; see the same section |
| Item 1 (shader-variant key) is the top lever | 08 §8.2, §8.8 | **no** — measured small; the win is a default, 08 §8.12 |
| Three-point filtering is affordable on this GPU | — | **no** — 19.88% of frame time at a chosen viewpoint, 08 §8.14 |
| Dropping to linear costs image quality | 08 §8.12 | **no** — indistinguishable at 3× zoom, 08 §8.13 |
| Item 2 (dependent texture reads) is the top code candidate | 08 §8.3, §8.12 | **no** — already implemented; zero headroom, 08 §8.15 |
| Report 8's items 3, 5, 6, 7, 8 are a work queue | 08 §8.8 | **no** — unvalidated code readings; probe each first, 08 §8.15 |
| The benchmark measured the viewpoint the docs describe | 09 §9.2 | **no** — the player pins never held, 09 §9.9 |
| Item 7 (VBO ring buffer) needs a draw-call count to size | 08 §8.6, §8.9 | **no** — report 5 §5.3 had already measured it at 4× *worse*. Refuted, 08 §8.6 |
| Item 1 (shader-variant key) is still worth doing | 08 §8.2, §8.12 | **not here** — the shipped `FILTER_LINEAR` default already strips the branch at compile time, 08 §8.16 |
| Whether the IMG compiler flattens the filtering branch is open | 08 §8.2, §8.9 | **moot** — no branch exists on the default; §8.14 cannot isolate it on the other path, 08 §8.16 |
| The save corruption needs investigating before real play | 07 §7.7 | **mechanism closed, cause open** — saves are defended, the stray `close()` is not yet named, 10 |
| `logs/` not being writable explains the flush failures | 07 §7.7 | **no** — `EBADF` from `fflush` means the descriptor is gone, not the directory, 10 §10.1 |
| Any harness result or `mean_fps` taken before 2026-09-23 23:20 | 08 §8.10–§8.18, 09 | **capped at 30 FPS** — the harness never uncapped; `mean_us` is 33.3 ms minus the untimed work for any scene under the cap, 08 §8.19 |
| Hyrule Field is slow (~30 ms) and not fragment-bound | 08 §8.18, NEXT | **no** — both were the cap. Uncapped it runs 21.1 ms (47 FPS), the lightest fixture, 08 §8.19 |
| Halving internal resolution is worth +75%; the workload is fill-bound | 07 §7.2, `OTRGlobals.cpp` | **only from 1.0× to 0.5×** — at the shipped 0.5×, going to 0.25× buys 5–6%, 08 §8.19 |
| 33.9 / 34.15 FPS at the fixture; the `InterpolationFPS` trade has dissolved | 08 §8.14, §8.17, NEXT | **no** — 1/timed-window under the cap. Real uncapped Market period is 31.7 FPS, frozen, 08 §8.19 |
| `ShaderOpts` 3 is worth −3.59% / −2.40% / nothing by scene | 08 §8.17, §8.18, NEXT | **no** — capped. Uncapped: −4.85% Market, −4.55% Hyrule Field; Kokiri not re-measured, 08 §8.20 |
| The CPU was pinned at 1.25 GHz during benchmark runs | 09, 08 §8.14–§8.19 | **only when the die was above ~77 °C** — `pyra-thermal.service` rewrote the ceiling every 2 s; fixed with the userspace governor, 08 §8.20 |
| A 20% slower CPU costs the frame only ~2% | 08 §8.19, NEXT | **unverified** — taken on the leaky pin, 08 §8.20 |

## The reports

| # | Title | What it is for |
|---|---|---|
| [00](00-EXECUTIVE-SUMMARY.md) | Audit executive summary | The five findings that decided the plan, plus every measured device fact. Start here. |
| [01](01-architecture-compatibility.md) | Architecture compatibility | ARMv7-A / armhf / Cortex-A15. Conclusion: the CPU side is a non-event. |
| [02](02-renderer-compatibility.md) | Renderer compatibility | Fast3D → GLES 2.0 on SGX544MP2. The actual job. |
| [03](03-dependency-build.md) | Dependency & build system | What builds, what has to be replaced. |
| [04](04-implementation-plan.md) | Implementation plan | Phased plan. Phase 5's discard-framebuffer item is now known to be worthless. |
| [05](05-phase0-device-measurements.md) | Phase 0 device measurements | Synthetic benchmarks. **Its headline conclusion was wrong** — see 07 §7.3 for why, and treat it as a lesson about synthetic tests. |
| [06](06-phase1-cross-build.md) | Phase 1 cross build | Getting an armhf binary out of Docker. |
| [07](07-phase5-performance.md) | Phase 5 performance | The resolution sweep, the 20 FPS cap, the vsync trap, the game-speed coupling bug. Ratios good, absolutes stale. |
| [08](08-optimisation-opportunities.md) | Code-level optimisation opportunities | The catalogue of nine candidate optimisations, what each rests on, and the three results measured so far. |
| [09](09-benchmark-harness.md) | Deterministic benchmark harness | The measurement rig built because 07 and 08 both hit the same ±13% wall. Validated, and ABBA confirmed in §9.8. |
| [10](10-save-file-corruption.md) | The save file corruption bug | 07 §7.7, closed. The mechanism reproduced, the save path made verify-before-replace, and the one open question left with the tool that answers it. |

`tools/ccscan.py` is a helper used by report 08 to enumerate Fast3D colour-combiner variants.

## Two things that keep going wrong

Both are worth stating plainly, because each has already cost this project a phase.

**Synthetic benchmarks answer a different question than they appear to.** Report 5 measured one
textured blended quad, found enormous fill-rate headroom, and concluded the GPU was not the
constraint. The real workload — overdraw, small triangles, texture binds, state churn — behaves
nothing like that on a tile-based deferred renderer. Report 9's harness is a better rig but it is
still a *fixture*: a frozen frame tells you a frame got cheaper to draw, not that the game got
faster. Anything that screens well there still has to be confirmed in real play.

**Frame rate and game speed are the same number here.** One `RunCommands` call is one game-logic
tick emitting `InterpolationFPS / 20` rendered sub-frames, with no catch-up, so
`game speed = render FPS / InterpolationFPS`. Maximising FPS against a cap the hardware cannot
reach buys slow motion, not smoothness. This was found by the device's owner noticing the game
running slow during a benchmark — not by any measurement in these reports, all of which tracked
frame rate and none of which tracked speed. See 07 §7.5b.
