# Report 9 — A deterministic benchmark harness

> **Status: validated on hardware 2026-09-21. It resolves 0.40%, against the ±13% it replaces.**
> Ten identical segments spread 1.15% peak-to-peak while the die went 59 °C → 81 °C and the
> clock throttled 1.5 → 1.25 GHz. Full numbers in §9.7.

Reports 7 and 8 both ended at the same wall: the measurement rig could not resolve anything
smaller than about 13%. The `GL_EXT_discard_framebuffer` A/B (report 8 §8.10) needed five
interleaved pairs and 300 s of warm-up to place a bound of ±1.8% on a single change, and that was
the *good* case — a fixture that never left the title-screen attract demo. Anything measured
during real play had a standard deviation of 2.75 FPS between identical configurations.

This report describes the harness built to fix that, why each piece is there, and what it still
does not solve.

## 9.1 Why the old rig was noisy

Three things moved at once, and only one of them was the change under test.

**The workload was not held constant.** Mean FPS over a fixed wall-clock window sounds like a
fair comparison. It is not, because in SoH

```
game speed = render FPS / InterpolationFPS
```

(report 7 §7.5b). A faster arm therefore travels *further through the scene* in the same 60
seconds, and ends up rendering different geometry from the slower arm. The two arms are not
measuring the same work. This is the same confound that invalidated index-wise pairing in the
audio A/B, and it is structural: no amount of repetition averages it away, because it is
correlated with the very thing being measured.

**The die temperature moved.** Frame rate falls ~30% between cold and equilibrium (report 7
§7.1). Interleaving arms helps, but the two arms of a pair were still minutes apart because each
needed its own process launch.

**The process was relaunched between arms.** Shader compilation, resource cache population and
allocator state all differ run to run.

## 9.2 What the harness does

Compile with `-DSOH_BENCHMARK=ON`. Every call site is `#ifdef`'d, so a normal build is unchanged.
Configuration is entirely through environment variables, so a run is fully described by its
command line and nothing touches the saved config.

| Piece | What it removes |
|---|---|
| Boots straight into a chosen entrance on a debug save | file-select navigation, run-to-run starting position |
| Freezes game logic (`frameAdvCtx.enabled`) | scene content drift — the display list becomes identical every frame |
| Pins the camera explicitly (`view.eye` / `view.at`) | the entrance's incidental viewpoint |
| Counts **rendered frames**, reports **time** | the game-speed/frame-rate coupling above |
| Alternates a CVar **inside one process** | thermal drift and process-start variance |
| Forces fullscreen and logs the render size | the window size silently differing between runs |
| Raises the frame cap | measuring `SyncFramerateWithTime` instead of the renderer |

### The static-scene trick

`FrameAdvance_Update` (`soh/src/code/z_frame_advance.c:16`) returning false skips the whole of
`Play_Update`. `Play_Draw` is called unconditionally from `Play_Main`
(`soh/src/code/z_play.c:1736`), *outside* that check. So setting `frameAdvCtx.enabled` freezes
actors, camera, animation and RNG while rendering continues — the same display list, every frame,
indefinitely. No new code was needed for this; it is the existing frame-advance debugger used as
a fixture.

That also makes the camera fully controllable. `Play_Draw` rebuilds the viewing matrix from
`play->view.eye/at/up` every frame, and the only thing that normally writes those is
`Camera_Update` inside `Play_Update` — precisely what the freeze skips. Writing them from the
harness therefore holds the view exactly still wherever it is pointed.

### Why the camera must be pinned deliberately

The obvious fixture — warp to Kokiri Forest, stand still — is not neutral. `ENTR_KOKIRI_FOREST_0`
spawns Link on the treehouse platform with the camera tight behind him and the hut wall filling a
large part of the frame. That is a much cheaper view than the scene can produce, and a
fragment-cost measurement taken there would come back optimistically small.

The spawn a warp lands on is a property of the entrance table, not a considered benchmarking
choice. The harness therefore logs position, yaw, eye, at and fovy every run, and warns when no
override is set. Establish the viewpoint once, read it out of the log, and pin it for every run
afterwards.

### Intra-process A/B

`SOH_BENCH_AB_CVAR` flips a setting between segments without restarting, so both arms of a pair
are seconds apart at one die temperature rather than minutes apart across two launches.

This needed one change in libultraship. Texture filtering is compiled into the shader source
(`o_three_point_filtering`) but is **not** part of the shader variant key, which is why the
settings menu labels it "Needs reload". `Interpreter::ResetShaderCache()` now drops the compiled
variants *and* the `ShaderProgram*` cached inside every `ColorCombiner`, and
`Fast3dWindow::SetTextureFilter` calls it. That makes the setting take effect live — useful
beyond benchmarking, and upstreamable on its own merit.

Because dropping the cache means the next frames pay recompilation, each segment discards
`SOH_BENCH_SEG_WARMUP` frames before timing resumes. Without that the harness would be measuring
the IMG shader compiler.

The arms are **counterbalanced**, not strictly alternated. Under ABAB, B always follows A, so any
systematic drift through the run — the die warming, a background process settling — biases every
pair in the same direction and the paired test happily reports it as an effect. Running ABBAABBA
puts each arm first in half the pairs, which cancels a linear drift exactly instead of trusting
repetition to average it away. The summary reads which arm a segment ran from its recorded label
rather than inferring it from position.

The original value of the A/B CVar is restored before exit, so a benchmark run never leaves the
last arm persisted into the saved config.

## 9.3 Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `SOH_BENCH` | unset | master switch; `1` to activate |
| `SOH_BENCH_ENTRANCE` | `0xEE` | entrance index; accepts `0x` form |
| `SOH_BENCH_AGE` | `1` | 0 adult, 1 child |
| `SOH_BENCH_TIME` | `0x8000` | `dayTime`; noon. Fixed so lighting cannot drift |
| `SOH_BENCH_NIGHT` | `0` | night flag |
| `SOH_BENCH_POS` | unset | `x,y,z` to place the player |
| `SOH_BENCH_YAW` | unset | facing, binang |
| `SOH_BENCH_EYE` | unset | `x,y,z` camera position — takes effect once frozen |
| `SOH_BENCH_AT` | unset | `x,y,z` camera target; required with `EYE` |
| `SOH_BENCH_STATIC` | `1` | freeze game logic |
| `SOH_BENCH_SETTLE` | `240` | frames before freezing |
| `SOH_BENCH_WARMUP` | `600` | frames discarded before the first segment |
| `SOH_BENCH_SEG_WARMUP` | `180` | frames discarded after each arm switch |
| `SOH_BENCH_FRAMES` | `600` | measured frames per segment |
| `SOH_BENCH_REPS` | `5` | A/B pairs (counterbalanced ABBA) |
| `SOH_BENCH_AB_CVAR` | unset | CVar to alternate |
| `SOH_BENCH_AB_VALUES` | unset | comma-separated, e.g. `0,1` |
| `SOH_BENCH_FULLSCREEN` | `1` | force fullscreen before measuring |
| `SOH_BENCH_TARGET_FPS` | `240` | high enough that the limiter never sleeps |
| `SOH_BENCH_OUT` | unset | write results here as well as to the log |
| `SOH_BENCH_EXIT` | `1` | quit when finished |

## 9.4 Output

```
BENCH config entrance=0x00EE age=1 dayTime=0x8000 night=0
BENCH config settle=240 warmup=600 measure=600 static=1 targetFps=240
BENCH config window=1280x720 game_fb=640x360 fullscreen=1
BENCH config abCVar=gSettings.TextureFilter reps=5
BENCH segment 0 arm=0 frames=600 mean_us=... median_us=... p95_us=... mean_fps=...
...
BENCH pair 0 a_us=... b_us=... delta_us=... delta_pct=...
BENCH paired n=5 mean_delta_us=... sd_us=... t=... df=4
BENCH mde_us=...
```

The last line is the minimum detectable effect. Reporting it next to a null result is the
difference between "no effect" and "no effect this rig could see" — a distinction report 8 §8.10
had to make by hand.

## 9.5 What this does *not* fix

**A static frame is a screen, not a verdict.** It answers "is this shader cheaper", not "does the
game get faster". Those are different questions, and report 5 §5.2-5.3 already burned this
project once by reading a synthetic fill-rate benchmark as an answer to the second when it only
measured the first (see report 7 §7.3). Anything that screens positive here still has to be
confirmed on a real gameplay run before it earns a row in report 8's summary table.

**A frozen scene has no CPU-side game logic.** `Play_Update` is skipped, so actor updates,
collision and AI are absent. That is the right call for fragment-cost work and the wrong one for
anything touching the interpreter's CPU cost or the resource cache. Use `SOH_BENCH_STATIC=0` for
those, and accept the higher variance.

**One viewpoint is not one scene.** Kokiri Forest from the treehouse and Kokiri Forest from the
village floor are different workloads. Several pinned viewpoints per scene are worth more than
one, and Hyrule Field and Market/Kakariko are still to be added.

## 9.6 Bring-up order on the device

The harness has three things that can be wrong independently, so they are checked in order rather
than all at once by looking at a number at the end.

**1. Does it reach the scene at all?** `SOH_BENCH=1` with defaults. Expect the log to show the
scene loaded and a `BENCH viewpoint` line. A stall here means the entrance index is wrong; the
harness gives up after 3600 frames and says so rather than hanging.

**2. Is the viewpoint worth measuring?** Read `pos`, `yaw`, `eye`, `at` out of that line and look
at what is on screen. The default `ENTR_KOKIRI_FOREST_0` spawn faces Link with very little scene
behind him and is unrepresentatively cheap — it is the default because it is the *scene* that was
chosen, not the view. Pick a view with real depth complexity in it, then pin it with
`SOH_BENCH_POS` / `YAW` / `EYE` / `AT` so every later run is the same work.

**3. Is the rig actually quiet?** Run the *same* arm against itself:
`SOH_BENCH_AB_CVAR=gSettings.TextureFilter SOH_BENCH_AB_VALUES=1,1`. The paired t should be
insignificant and the reported MDE is then the real resolution of the rig. This is the number
that says whether any of report 8's items can be screened at all, and it is worth more than the
first real A/B. Skipping it means the first surprising result has two possible explanations.

Only then the measurement this was built for: `bench.sh ab gSettings.TextureFilter 0 1`.

## 9.7 Result: the rig resolves 0.40%

First run on hardware, 2026-09-21. The check is the one §9.6 step 3 describes — **the same arm
against itself**, `gSettings.TextureFilter` 1 vs 1, so the true effect is exactly zero by
construction and anything the harness reports is its own noise.

Kokiri Forest, child, noon, static, viewpoint pinned. 640×360 internal on a 1280×720 fullscreen
window. 9000 warm-up frames, then 10 segments of 600 measured frames each, ABBA.

| Segment | Mean frame time (µs) | FPS |
|---|---|---|
| 0 | 28113.4 | 35.57 |
| 1 | 28005.7 | 35.71 |
| 2 | 27960.5 | 35.77 |
| 3 | 27857.0 | 35.90 |
| 4 | 27908.2 | 35.83 |
| 5 | 27881.1 | 35.87 |
| 6 | 27956.2 | 35.77 |
| 7 | 27935.4 | 35.80 |
| 8 | 28063.1 | 35.63 |
| 9 | 28177.5 | 35.49 |
| **mean** | **27985.8** | **35.73** |

Ten identical measurements spanning **320 µs, or 1.15% peak-to-peak**, with a standard deviation
across segments of 0.37%.

```
paired n=5  mean_delta=-28.96 us  sd=89.97  t=-0.720  df=4
mde_us=111.86
```

`t = -0.72` against a critical value of ±2.776: **correctly not significant**. A rig that reported
a difference here would be broken, and this one does not.

### What the resolution figure means, precisely

The harness prints `2.78 × se`, which is the smallest delta that *would* have reached
significance — 111.9 µs, **0.40%** of the 27986 µs baseline. The conventional minimum detectable
effect at 80% power is larger, `(2.776 + 0.941) × se` = 149.6 µs, or **0.53%**. Quote 0.53% when
asking "what could this rig have found"; quote 0.40% when asking "what would it have called
significant". Both are roughly **33× better than the ±13% floor** reports 7 and 8 worked under.

### It held through a 22 °C thermal excursion

The die went **59.3 °C → 81.5 °C** across the run and the clock throttled **1.5 GHz → 1.25 GHz**.
That is the single largest confound in every earlier measurement, and it is visible in the
segment table if you look for it — segments 8 and 9 are the two slowest, which is the late-run
throttle arriving. It is worth being precise about what the design did and did not do here:

- **The drift is not linear.** Frame times fall from segment 0 to segment 3, sit flat through the
  middle, then rise at 8 and 9. ABBA counterbalancing cancels a *linear* drift exactly; against
  this shape it does not cancel exactly, it just averages well.
- **Both arms were identical**, so the pairing had nothing to cancel anyway. This run demonstrates
  that the drift is *small* — 1.15% over the whole excursion — not that the counterbalancing
  works. That still has to be shown by a real two-arm A/B.

The reason the drift is small at all is the fixed frame count: a segment is 600 frames of the
same display list whether the clock is at 1.5 or 1.25 GHz, so throttling lengthens the segment
rather than changing what is inside it.

### One number that is not comparable to report 7

35.73 FPS here against report 7's 28.7 FPS at the same resolution is **not** a 24% improvement.
This scene is frozen, so `Play_Update` never runs — no actors, no collision, no AI — and audio
has since moved off the CPU. It is a different workload. Do not quote it as a gameplay figure.
