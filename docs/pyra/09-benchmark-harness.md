# Report 9 — A deterministic benchmark harness

> **Status: validated on hardware 2026-09-21, and again across a reboot on 2026-09-22. It
> resolves 0.26-0.40% depending on how warm the machine starts, against the ±13% it replaces.
> ABBA counterbalancing confirmed working in §9.8.**
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

The obvious fixture — warp to Kokiri Forest, stand still — is not neutral. The spawn a warp lands
on was chosen to start a game, not to exercise a renderer, and §9.9 shows how far that can go: of
seven Kokiri Forest entrance indices surveyed by screenshot, two loaded a different scene
entirely, one spawned with a dialogue box already open, and two put the camera inside a wall.

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
| `SOH_BENCH_POS` | unset | `x,y,z` ground position for the player; `y` is a starting height, the floor decides the rest (§9.9) |
| `SOH_BENCH_YAW` | unset | facing, binang |
| `SOH_BENCH_EYE` | unset | `x,y,z` camera position — takes effect once frozen |
| `SOH_BENCH_AT` | unset | `x,y,z` camera target; required with `EYE` |
| `SOH_BENCH_STATIC` | `1` | freeze game logic |
| `SOH_BENCH_SETTLE` | `240` | frames before freezing |
| `SOH_BENCH_WARMUP` | `600` | frames discarded before the first segment — a floor, not the whole warm-up when a soak is set |
| `SOH_BENCH_SOAK_C` | `0` (off) | hold warm-up until the CPU thermal zone reads this many °C, so every run starts measuring at one temperature (§9.10) |
| `SOH_BENCH_WARMUP_MAX` | `12000` | frames after which the soak gives up and says so |
| `SOH_BENCH_TEMP_PATH` | auto | thermal zone to read; resolved by looking for a zone whose `type` starts `cpu` |
| `SOH_BENCH_SEG_WARMUP` | `180` | frames discarded after each arm switch |
| `SOH_BENCH_FRAMES` | `600` | measured frames per segment |
| `SOH_BENCH_REPS` | `5` | A/B pairs (counterbalanced ABBA) |
| `SOH_BENCH_AB_CVAR` | unset | CVar to alternate |
| `SOH_BENCH_AB_VALUES` | unset | comma-separated, e.g. `0,1` |
| `SOH_BENCH_FULLSCREEN` | `1` | force fullscreen before measuring |
| `SOH_BENCH_TARGET_FPS` | `240` | high enough that the limiter never sleeps |
| `SOH_BENCH_OUT` | unset | write results here as well as to the log |
| `SOH_BENCH_EXIT` | `1` | quit when finished |
| `SOH_BENCH_WATCH` | `0` | authoring mode: never freeze, never measure, log the viewpoint every 60 frames so a fixture can be walked to (§9.9) |

## 9.4 Output

```
BENCH config entrance=0x00EE age=1 dayTime=0x8000 night=0
BENCH config settle=240 warmup=600 measure=600 static=1 targetFps=240
BENCH config window=1280x720 game_fb=640x360 fullscreen=1
BENCH config abCVar=gSettings.TextureFilter reps=5
BENCH config soak_target_c=76 warmup_max=20000 temp_now_c=79.3
BENCH segment 0 arm=0 frames=600 mean_us=... median_us=... p95_us=... mean_fps=... temp_c=76.1->78.4
...
BENCH pair 0 a_us=... b_us=... delta_us=... delta_pct=...
BENCH paired n=5 mean_delta_us=... sd_us=... t=... df=4
BENCH mde_us=...
```

The last line is the minimum detectable effect. Reporting it next to a null result is the
difference between "no effect" and "no effect this rig could see" — a distinction report 8 §8.10
had to make by hand.

### The CPU is pinned to 1.25 GHz for the duration of a run

The device runs the `ondemand` governor across eleven steps from 250 MHz to 1.5 GHz. Runs up to
2026-09-22 happened to sit at 1.25 GHz throughout and nothing showed; the run in report 8 §8.14
moved 1.4 GHz → 1.0 GHz mid-run and cost a factor of three in resolution, because ABBA cancels
linear drift and not a step change part-way through. The game blocks waiting on the GPU, so CPU
utilisation is not high enough to hold `ondemand` at the top of its range.

`bench.sh` now sets `scaling_min_freq == scaling_max_freq == 1250000` around every run and restores
the original range from a `trap` on exit, failure or interrupt. The **governor is deliberately left
alone**: pinning min to max forces any governor to a single frequency, so there is no governor
state to save — and sysfs will not hold a saved copy anyway. `PYRA_PIN_FREQ=` disables it.

1.25 GHz rather than the 1.5 GHz maximum, for two reasons: it is where every quiet run happened to
sit, so it keeps continuity with earlier numbers; and it is below the 1.4-1.5 GHz the governor
otherwise reaches, so at equal load a pinned run cannot draw more power than an unpinned one.
(That is a statement about power, not a measured temperature claim — a pinned 10-segment run still
ended at 81.1 °C. Thermal protection is unaffected either way: the `cpu_thermal` zone clamps
frequency through the `thermal-cpufreq-0` cooling device whatever the governor is set to, with the
passive trip at 100 °C and benchmarks running around 80 °C.)

**Measured effect**, null A/B at the §9.9 fixture, both arms identical, against the unpinned
§8.14 run at the same fixture:

| | pair sd | resolution |
|---|---|---|
| unpinned | 302.17 µs | 1.02% |
| pinned 1.25 GHz | **135.63 µs** | **0.57%** |

Thermal drift remains — segments ran 29415 → 29804 µs, +1.3%, as the die warmed 65.7 → 81.1 °C.
Pinning removes the clock step, not the drift. The drift is now monotonic, which is the shape ABBA
actually cancels: the five pair deltas came back +0.56, +0.03, +0.85, −0.03, −0.26%, mixed sign
and not significant (t=1.111).

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

## 9.8 The rig across a reboot, and ABBA's first real test

§9.7's repeatability check ran both arms identical, which proved the rig quiet but left the ABBA
counterbalancing unexercised — the pairing had nothing to cancel. The first real A/B (report 8
§8.12, 2026-09-22, after a power-down, a charge and a fresh boot on the same m4 kernel) exercised
it.

**Cross-day reproducibility: 0.22%.** The linear arm measured 28046.9 µs against §9.7's
27985.8 µs — across a reboot, a different ambient, and a cold start at 46.1 °C instead of 59.3 °C.

**ABBA holds.** With a 6051 µs effect present and worse thermal drift than §9.7 saw, the pair
standard deviation was 87.40 µs — indistinguishable from §9.7's 89.97 µs, which was measured with
*no* effect at all. The five pair deltas were −17.71, −17.99, −18.05, −17.51, −17.47%: same sign,
spread 3.2% of the effect. The drift did not leak into the pairs. That is the evidence §9.7 could
not supply.

**Resolution varies with how warm the machine starts.** Run 1 (cold, 46.1 °C) gave
`mde_us=108.66` on a 28 ms frame, 0.39%. Run 2, started warm at 63.6 °C, gave `mde_us=71.52`,
**0.26%**. Starting warm is worth about a third of the rig's error.

**Warm-up is still the limiting factor.** 16000 frames (~7.5 min) was not enough from a cold
boot: the linear arm drifted 1.44% across run 1 while the die climbed 46.1 → 76.8 °C. For a small
effect that would matter even though ABBA cancels the linear component. The cheap fix is to run
something on the device before benchmarking rather than to keep raising `SOH_BENCH_WARMUP`.

## 9.9 The pins did not hold, and the fixture was never the one in the docs

Every number in reports 8 and 9 up to this point was measured at a viewpoint nobody chose.

**The bug.** `SOH_BENCH_POS` and `SOH_BENCH_YAW` were applied once, at settle frame 30, and then
left alone for the remaining 210 settle frames. Ordinary player update — entrance animations,
gravity, sliding, and an NPC's collision push-out — walked the player straight back off the mark.
The camera pins held, because `ApplyCameraOverride` alone was re-applied every frame. So the runs
were reproducible and the *camera* was exactly where it was asked to be, while the player ended
up roughly 500 units away:

```
asked for  pos=[ -66.0,-80.0, 938.6]  yaw=0x638A
got        pos=[-104.4,-80.0,1435.6]  yaw=0xBD0D     (reproducible to the decimal)
```

`ApplyPlayerOverrides` carried the comment *"a silent miss here would benchmark the wrong viewpoint
and look entirely normal"*. That is what happened — not through a miss, but through a hit that was
immediately undone.

**What was actually being measured.** Not the "treehouse close-up" the docs described. The player
was ~390 units *behind* the camera and out of frame entirely; the frame was a treehouse ladder in
the right foreground with a large ground plane filling most of the screen. As a fill-rate fixture
that is not unreasonable — large textured surfaces are what stress per-fragment filtering — but it
is not what anyone wrote down, and it had little scene depth.

**The fix.** The position pin is now held every settle frame rather than fired once. Height is
deliberately *not* held: pinning `y` every frame hovers the player above sloping ground or buries
him in it, which makes a viewpoint impossible to author by hand — you would have to know the floor
height at every candidate position. `y` is now a starting height and the floor catches him, so
`SOH_BENCH_POS` reads as a ground position. The settled height is logged. At freeze the run
compares actual x/z against the request and logs an error above 4 units, so a fixture that has
drifted says so instead of quietly benchmarking somewhere else.

### Choosing a fixture by looking at it

Entrance indices are a bad way to pick a viewpoint. Seven Kokiri Forest entrances were surveyed by
screenshotting the frozen frame of each:

| Entrance | What actually loaded |
|---|---|
| `0x00EE` | Kokiri Forest, village path — usable |
| `0x0209` | **Deku Tree dungeon interior** (scene 0, not 85) |
| `0x020D` | Kokiri Forest, long village vista — usable |
| `0x0211` | **A house interior** (scene 52) |
| `0x0266` | Kokiri Forest, but spawns with **a dialogue box already open** |
| `0x026A` | Camera inside a wall, player mid-fall |
| `0x0286` | Camera inside a wall, dark |

Two of seven were not even the right scene. Pinning `0x020D` and walking the player forward ran
into a second problem: Mido stands blocking that path, and his collision push-out fights the pin.

So the fixture was authored by hand instead, which is what `SOH_BENCH_WATCH` exists for. It runs
the game normally — no freeze, no overrides, no measurement — and logs position, yaw, eye, at and
fovy every 60 frames. Walk the player to a good spot, let the camera settle, read the last line,
pin those numbers.

### The fixture

```
SOH_BENCH_ENTRANCE=0x00EE
SOH_BENCH_POS=-592.8,0.0,-195.9
SOH_BENCH_YAW=0x369E
SOH_BENCH_EYE=-707.3,55.3,-222.8
SOH_BENCH_AT=-592.8,34.7,-195.9
```

Kokiri Forest village square, looking down the valley. A large ground plane in the near field, a
stone pillar, the shop roof, a bridge and fences at mid range, terrain and sky beyond, the player
and a second Kokiri close to camera, and Navi as a large alpha-blended sprite. Near geometry, far
geometry, characters and transparency in one frame — which is what the old fixture lacked.

**Numbers taken before this section are not comparable to numbers taken after it.** The viewpoint
changed, so every percentage in report 8 §8.12 belongs to the old frame.

## 9.10 The soak: making two runs comparable, not just two segments

ABBA cancels thermal drift *inside* a run. Nothing made two runs start at the same temperature,
and the harness did not record what temperature either one ran at — so a frame time from one run
and a frame time from another were being compared as though they meant the same thing when the
die could differ by 30 °C between them, on a part whose frame rate moves ~30% across that range
(§7.1, §9.1).

Two changes:

- **`SOH_BENCH_SOAK_C` gates the end of warm-up on temperature**, not just on a frame count. Both
  conditions have to hold — `SOH_BENCH_WARMUP` frames *and* the target — because the frame count
  is the shader-compilation floor and skipping it on a part that is already hot would measure the
  IMG compiler instead. `SOH_BENCH_WARMUP_MAX` bounds the wait and logs a warning rather than
  hanging when the fixture is too light to ever reach the target.
- **Every segment line carries `temp_c=start->end`**, and the config block carries the target.
  Thermal state is now in the results rather than in someone's memory of the run.

`76` is the recommended target: it is the bottom of the 73–79 °C equilibrium band report 7 §7.1
measured, and §8.14 reached 76.8 °C in about 16000 frames from cold — hence
`SOH_BENCH_WARMUP_MAX=20000`. Soaking to near the asymptote also shrinks the drift ABBA has to
cancel, because the curve is flattest there: §9.7's pinned run climbed 65.7 → 81.1 °C and drifted
+1.3% across its segments, and most of that climb was the steep part below equilibrium.

The cost is paid once per cold start, not once per run. A back-to-back sequence — the visual
check, then `0 3`, then `0 1`, then `0 2` — soaks during the first and finds the target already
met for the rest.

**What it still does not fix:** the part keeps warming during the measurement. The soak sets a
repeatable starting point; it does not hold a temperature. Ambient is also unrecorded and
uncontrolled, so a run in a warm room and a run in a cold one reach 76 °C at different rates and
sit at different equilibria. Comparing absolute frame times across sessions remains weaker than
comparing arms within one.

## 9.11 Checking the screen, not just the clock: `shotcheck.sh`

The harness answers "is this frame cheaper". For a shader change the prior question is "is this
frame still correct", and a frame time cannot see a colour saturating where it should have
wrapped. `scripts/pyra/shotcheck.sh` answers that one: it runs the fixture once per arm, grabs
frames with `xwd` off the device, and `shotdiff.py` reports how much the arms differ.

Three things it learned the hard way, all of which are now enforced in the tool:

- **A control is not optional.** Two launches of the *same* arm differ by ten to eighteen thousand
  pixels at the §9.9 fixture, with a worst channel around 240/255. The scene is frozen but not
  reproducible: particles and fairies come to rest in different places in each launch and then
  hold still there, so a mask built from frames *within* a launch does not remove them. The script
  therefore always runs a third slot with arm A again, and the verdict is B-against-A measured
  against A-against-A. Without that, every comparison looks like a catastrophic regression.
- **Kill the previous instance properly.** The game catches `SIGTERM` and shuts down slowly enough
  that the next launch starts alongside it. Instances accumulate, every one of them fullscreen,
  and `xwd -root` grabs whichever is on top — so the tool photographs an arm nobody asked for. The
  cleanup now escalates to `SIGKILL`, waits, and refuses to grab frames if anything is left.
- **`ssh host 'cd x && cmd > log &'` hangs.** The `&` backgrounds the whole list, so `cd` still
  holds the channel's stdout open, the channel never closes and ssh waits forever. Group the
  redirect and the `&` around the launch alone.

Frames are named by slot rather than by arm value, so `shotcheck.sh <cvar> 0 0` is a real null
rather than one launch quietly overwriting the other's frames and being compared with itself.

### The 1.25 GHz cap is a daemon, not luck

§9.7 says every run before the pin "sat at 1.25 GHz by luck". It is not luck: `pyra-thermal.service`,
the Pyra's own thermal daemon, moves `scaling_max_freq` **with die temperature**. Warm, it holds
1.25 GHz — writing `cpuinfo_max_freq` (1,500,000) in takes effect and is reverted within three
seconds, with the `thermal-cpufreq-0` cooling device still at state 0, so this is the daemon's
policy and not kernel thermal throttling. Cooled to 65 °C it puts the ceiling back to 1.5 GHz on
its own.

Three consequences, and the third is the important one:

- `bench.sh` pinning to exactly 1.25 GHz agrees with the device's own warm steady state rather
  than fighting it, which is part of why pinning worked so well.
- A run never leaves the device in an unusual state; the daemon puts it back.
- **A cold, unpinned run starts with a 1.5 GHz ceiling and loses it part-way through as the part
  warms.** That is a step change in the middle of a measurement, which is exactly the shape ABBA
  does not cancel and exactly what §8.14 saw. It is a second, independent reason for the soak
  (§9.10): reaching equilibrium before measuring puts the run entirely on one side of the
  daemon's decision instead of straddling it.
