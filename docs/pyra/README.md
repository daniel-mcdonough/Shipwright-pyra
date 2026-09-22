# DragonBox Pyra port — documentation index

Ten reports, written as the work happened. They are a record rather than a manual, which means
**the later ones correct the earlier ones** in several places. This index exists so that nobody
reads a superseded number and acts on it.

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
| Benchmark harness resolves better than ±13% | 09 §9.7 | **validated on hardware** — resolves 0.40%, ~33× better |
| "Items 5 and 6 are below the measurement floor" | 08 §8.9 | **no longer true** — the floor moved; see the same section |
| Item 1 (shader-variant key) is the top lever | 08 §8.2, §8.8 | **no** — measured small; the win is a default, 08 §8.12 |
| Three-point filtering is affordable on this GPU | — | **no** — it costs 17.75% of frame time, 08 §8.12 |

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
