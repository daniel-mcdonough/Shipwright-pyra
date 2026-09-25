# Testing on the Pyra — quickstart

Everything needed to measure or visually check a change, without reading the reports first.
Copy-pasteable. The *why* behind each rule is in report 9 (the harness) and report 8 (results);
this file is the operating manual and is kept current.

**Before anything: the device must be free.** It is someone's handheld. Do not launch, deploy or
benchmark on it unless the owner has said so in the current session.

---

## 0. Session setup

```sh
export SSH_AUTH_SOCK="$(ls -t ~/.ssh/agent/s.*.agent.* | head -1)"
export PYRA_HOST=pyra@pyra.local
export PYRA_DIR=/home/pyra/soh-bench2
```

`pyra.local` — the `<pyra-ip>` in the hardware repo is stale. `bench.sh` defaults `PYRA_DIR` to
`~/soh-bench2` since 2026-09-24; before that it defaulted to `~/soh`, a stale binary with no
archives, and a run that forgot the export stopped at a missing-o2r dialog. Two install directories:

| | |
|---|---|
| `~/soh-bench2` | the benchmark install. Work here. |
| `~/soh-pyra` | **the owner's real install. Do not clobber it.** |

Check the device is idle and note its temperature before trusting anything:

```sh
ssh -n "$PYRA_HOST" "pgrep -x soh.elf | wc -l; awk '{printf \"%.1f C\n\", \$1/1000}' /sys/class/thermal/thermal_zone0/temp"
```

A non-zero process count means a previous run leaked. Kill it before measuring (see trap 2).

`pyra.local` is mDNS. **"Could not resolve hostname" almost always means the handheld is asleep or
off, not that anything is broken** — ask the owner rather than debugging the network.

**Someone must be logged in on the device, not just booted.** Everything here runs on `DISPLAY=:0`
over ssh, and that needs the session's X cookie in `~/.Xauthority`. A device sitting at the
lightdm greeter has an empty one, and every graphical command fails with `Authorization required,
but no authorization protocol specified` / `unable to open display ':0'` — the game launch as much
as `xwd`. Check before blaming the tool:

```sh
ssh -n "$PYRA_HOST" "who"      # want a seat0 line for the owner's user, not for lightdm
```

If you only need a screenshot and cannot get a login, `sudo XAUTHORITY=/var/run/lightdm/root/:0
DISPLAY=:0 xwd -root -silent` works — but it will photograph the greeter, which is rarely what you
want.

A freshly booted device reads ~39 °C with the full 250 MHz–1.5 GHz range available; a working one
sits near 80 °C with `scaling_max_freq` held at 1.25 GHz by `pyra-thermal.service`. Seeing 1.5 GHz
is therefore a warning, not good news: a run started cold begins with that ceiling and loses it
partway through, which is the one drift shape ABBA cannot cancel. Soak (trap 1).

## 1. Build and deploy

```sh
./scripts/pyra/build.sh configure -DSOH_BENCHMARK=ON   # once
./scripts/pyra/build.sh build -j"$(nproc)"
./scripts/pyra/bench.sh deploy                          # copies soh.elf only
```

`libgbm.so.2` / `libffi.so.6` link warnings are normal — the DDK's own deps, resolved on device.

**If you changed anything under `libultraship/src/fast/shaders/`, the binary is not enough.**
Shaders ship inside `soh.o2r`, not in the executable. ZAPD is a host binary and cannot run in the
armhf container, so the archive is built natively and copied:

```sh
cmake --build build-cmake --target GenerateSohOtr
scp build-cmake/soh/soh.o2r "$PYRA_HOST:$PYRA_DIR/soh.o2r"
```

Make sure `$PYRA_DIR/soh.o2r` is a real file and not a symlink into `~/soh-pyra`, or you will
overwrite the owner's assets. A binary that meets an old archive logs `soh.o2r is older than this
binary` and silently falls back to the old shader.

## 2. The three fixtures

A fixture is a scene, a player position and a pinned camera. **Use at least two.** Uncapped
(§8.19) Hyrule Field runs 21.1 ms and the Market 31.5 ms at the same settings — they are different
workloads, not the same one twice. (The −3.59% / −2.40% / −0.07% spread quoted in §8.18 was taken
capped; see trap 10. Uncapped, `ShaderOpts` is −4.85% / −4.55% at Market / Hyrule Field, §8.20.)

**Market Day** — a dozen NPCs, near and far buildings. The heaviest fixture: 31.5 ms uncapped,
261 draws, 5.7 ms of game work outside the timed window. Quartering the pixels buys 4.9%.

```sh
MARKET="SOH_BENCH_ENTRANCE=0x00B1 SOH_BENCH_POS=-122.7,0.0,72.1 SOH_BENCH_YAW=0xD5A6 \
SOH_BENCH_EYE=0.0,64.0,0.0 SOH_BENCH_AT=-134.5,64.0,79.0 SOH_BENCH_SETTLE=400"
```

**Hyrule Field** — open plain, long draw distance. The light fixture: 21.1 ms (47 FPS) uncapped,
167 draws. ~~Not fragment-bound~~ — that came from capped runs (trap 10); quartering the pixels
buys 6.3% uncapped.

```sh
FIELD="SOH_BENCH_ENTRANCE=0x00CD SOH_BENCH_POS=1089.4,379.7,5669.1 SOH_BENCH_YAW=0xA1F9 \
SOH_BENCH_EYE=1217.7,458.2,5780.3 SOH_BENCH_AT=1089.4,424.7,5669.1 SOH_BENCH_SETTLE=400"
```

**Kokiri Forest** — enclosed village square, layered combiners.

```sh
KOKIRI="SOH_BENCH_ENTRANCE=0x00EE SOH_BENCH_POS=-592.8,0.0,-195.9 SOH_BENCH_YAW=0x369E \
SOH_BENCH_EYE=-707.3,55.3,-222.8 SOH_BENCH_AT=-592.8,34.7,-195.9 SOH_BENCH_SETTLE=400"
```

Market and Hyrule Field bracket the answer between them. Kokiri has not been re-measured uncapped.

## 3. The four things you can run

### A/B a CVar — the main tool

```sh
PYRA_TIMEOUT=1800 ./scripts/pyra/bench.sh ab gSettings.ShaderOpts 0 3 $MARKET \
    SOH_BENCH_SOAK_C=70 SOH_BENCH_WARMUP_MAX=20000 SOH_BENCH_REPS=6
```

One process, arms swapped between segments, ABBA counterbalanced, CPU pinned to 1.25 GHz for the
duration (userspace governor, `scaling_setspeed`) and restored on exit — see trap 12 for why it is
not `min == max` any more. A segment is 600 measured frames plus a 180-frame recompilation
grace, so ~25 s; `reps` is **pairs**, so `REPS=6` is 12 segments and roughly 5 minutes, `REPS=8`
is 16 and roughly 7. Add settle and soak. Run it with `run_in_background` and read the output file
— `bench.sh` has its own timeout (`PYRA_TIMEOUT`, default 900 s), so a hung harness will not hang
you. The default is too short for a soak that has to run long; see trap 9.

Five or six pairs resolves around 0.5% on a quiet device; a single noisy segment can push that to
1.5%, which is why the `mde_us` line matters more than the `t`. If you need to separate "no
effect" from "no effect this could see", add pairs.

### One arm, absolute numbers

```sh
./scripts/pyra/bench.sh run $FIELD SOH_BENCH_SOAK_C=70 SOH_BENCH_WARMUP_MAX=20000 SOH_BENCH_REPS=3
```

Use this to verify a *default* took effect, where an A/B would override the very thing you are
testing. **`run` measures exactly one segment** — `SOH_BENCH_REPS` only counts in A/B mode
(`TotalSegments()`), so the `REPS=3` above is inert. For several identical segments, e.g. to
attach a profiler to the middle ones and keep clean ones either side, A/B a CVar against itself:
`bench.sh ab gSettings.ShaderOpts 3 3 ... SOH_BENCH_REPS=5` is ten segments of the shipped arm. **Never quote an absolute taken without `SOH_BENCH_SOAK_C`** (trap 1).

### Real-play A/B — needs the owner holding the device

The frozen fixtures cannot see anything that happens only in play: lazy asset loads, scene
changes, game logic. Watch mode can alternate a CVar during normal play and log one line per
300-frame block (§8.23):

```sh
PYRA_WATCH_PIN=1 ./scripts/pyra/bench.sh watch start SOH_BENCH_ENTRANCE=0x00B1 SOH_BENCH_UNCAP=0 \
    SOH_BENCH_AB_CVAR=gSettings.LeanResLookup SOH_BENCH_AB_VALUES=0,3
./scripts/pyra/bench.sh watch blocks > blocks.txt      # while or after they play
./scripts/pyra/bench.sh watch stop                     # also restores the governor
```

Keep `SOH_BENCH_UNCAP=0` — uncapped play runs the game too fast — and `PYRA_WATCH_PIN=1`. Under
the cap the frame period is the cap, so compare **`thread_cpu_us`** (render-thread CPU per frame)
across adjacent ABBA pairs; blocks differ a lot in content, so it takes ~50 pairs (~17 minutes) to
resolve a ~10% effect. Every block line carries `scene=` and `cpu_min_mhz`/`cpu_max_mhz`.
`python3 docs/pyra/tools/watchpairs.py blocks.txt <arm A value>` pairs them, dropping pairs off the
pin or across a scene change, and prints the delta with t and MDE (§8.30).

### Author a new fixture — needs the owner holding the device

```sh
./scripts/pyra/bench.sh watch start SOH_BENCH_ENTRANCE=0x00CD   # game runs normally
./scripts/pyra/bench.sh watch read                              # last viewpoint lines
./scripts/pyra/bench.sh watch stop
```

Watch mode never freezes, never measures, and never ends; it logs position/yaw/eye/at every 60
frames. Ask the owner to walk somewhere and stand still, then read the **last** line — the camera
spring creeps ~0.3 units every two seconds and never fully settles.

**Grab a frame and look at it before trusting a new fixture:**

```sh
ssh -n "$PYRA_HOST" "DISPLAY=:0 xwd -root -silent > /tmp/f.xwd"
scp "$PYRA_HOST:/tmp/f.xwd" /tmp/f.xwd && magick /tmp/f.xwd /tmp/f.png    # then read the png
```

The device has `xwd` and nothing else; the conversion to PNG happens on the build host. Needs a
logged-in session (§0).

Entrance indices are an unreliable way to pick a viewpoint — two of seven surveyed Kokiri
entrances loaded the wrong scene entirely (§9.9).

### Visual check — did the picture change?

**If the change alters frame time, use the in-process check below as well** (§8.22).
`shotcheck.sh`'s control is launch-to-launch, and a launch that settles faster freezes animated
actors (Link's idle, NPCs) at a different pose. A faster arm can then fail on geometry that is not
wrong — mode 3 of `LeanResLookup` read DIFFER at Hyrule Field for exactly this reason and was
identical in-process.

```sh
./scripts/pyra/shotcheck.sh gSettings.ShaderOpts 0 3 $MARKET
```

Three launches: arm A, arm B, then arm A again as an **in-session control**. Reports both numbers
and passes only if the real comparison is near the control. The control is not optional — see
trap 3.

### In-process visual check — one frozen pose, both arms

Run an A/B with long segments and grab frames during each, so the pose cannot differ between arms.
Segment boundaries are not logged, so time the grabs from `Benchmark measuring` in `run.log`:
segment 0 starts ~4 s after it (the recompilation grace) and lasts `FRAMES × period`.

```sh
# on the device, armed before the run: grab twice in segment 0 (arm A), twice in segment 1 (arm B)
until grep -q 'Benchmark measuring' ~/soh-bench2/run.log; do sleep 1; done
sleep 25; for i in 1 2; do DISPLAY=:0 xwd -root -silent > /tmp/g-A-$i.xwd; sleep 2; done
sleep 55; for i in 1 2; do DISPLAY=:0 xwd -root -silent > /tmp/g-B-$i.xwd; sleep 2; done

# on the host
./scripts/pyra/bench.sh ab <cvar> A B $FIELD SOH_BENCH_FRAMES=2400 SOH_BENCH_REPS=1
```

Compare only pixels that are stable within each arm (A-1 = A-2 and B-1 = B-2), then look at where
the A ≠ B pixels are. **At Hyrule Field the clouds scroll in the frozen fixture**: on 2026-09-24 two
grabs of the *same* arm 23 s apart differed in 262,618 pixels, all at y ≤ 300 (§8.24; §8.22 had
seen only ~600 over 150 s, cause of the difference unknown). Mask the sky or take a same-arm
control at the same spacing. With `REPS=2` a third pair in segment 3 (arm A again) gives a same-arm
control at a similar time distance: slow drift (the sky at Hyrule Field) shows up there too.

## 4. Reading the output

```
BENCH config settle=400 warmup=600 measure=600 static=1 targetFps=240 uncap=1
BENCH segment 0 arm=single frames=600 mean_us=18382.7 ... temp_c=75.5->76.0 cpu_mhz=1250->1250 cpu_min_mhz=1250 cpu_max_mhz=1250 fb=640x360->640x360 swap_us=5039.8 finish_us=0.0 period_us=21113.7 period_fps=47.36
BENCH work 0 draws=167.3 tris=776.3 tex_uploads=0.00 ... shader_loads=63.0 fb_reallocs=0.00
BENCH pair 1 a_us=30580.2 b_us=30573.1 delta_us=-7.1 delta_pct=-0.02
BENCH paired n=8 mean_delta_us=-20.54 sd_us=164.36 t=-0.353 df=7
BENCH mde_us=137.22 (2.365*se at df=7)
BENCH period pair 1 ...
BENCH period paired n=8 ...
BENCH period mde_us=...
```

- **`period_us` is the frame rate; `mean_us` is not.** `mean_us` times only
  `DrawAndRunGraphicsCommands` (interpreter, GL submission, swap). `period_us` is start-of-frame
  to start-of-frame, so it also contains `Play_Draw` and everything else the game does between
  frames — 2.7 ms at Hyrule Field, 5.7 ms at the Market. It matches the game's own `LUS_FPS_LOG`
  counter to within 0.1 fps. Read the `period` paired lines for "did the game get faster"; read
  the unprefixed ones for "did drawing get cheaper".
- `swap_us` is time blocked in `SDL_GL_SwapWindow`. `finish_us` is non-zero only with
  `SOH_BENCH_FINISH=1`, which calls `glFinish` first: whatever is left in `swap_us` after that is
  presentation, not rendering. ~5 ms of every frame is (2026-09-23, both fixtures).
- `BENCH work` is what the interpreter handed to GL per frame, averaged. Use it to check a change
  did what it claims — fewer draws, no texture uploads — rather than inferring from the time.
- `delta` is **arm B minus arm A**, so negative means B is faster.
- `|t| > 2.4` or so is a real effect at these sample sizes. `t = -23.7` is not a subtle result;
  `t = -0.35` is nothing.
- `mde_us` is the smallest effect the run could have detected. A null result is only meaningful
  next to it.
- `temp_c=start->end` — two runs whose temperature ranges do not overlap are not comparable in
  absolute terms however tight their error bars.
- A segment whose temperature *falls* mid-segment is suspect; one produced a −2.48% outlier that
  did not reproduce.
- `cpu_mhz=start->end`, `cpu_min_mhz`, `cpu_max_mhz` — cpu0's `scaling_cur_freq` at the segment's
  first and last measured frame, and the extremes of a sample every 30 frames between. **All four
  must equal the pin.** A segment where any differs is discarded, not averaged, and the report says
  so. It reads the kernel's requested frequency; the true hardware clock (`cpuinfo_cur_freq`) is
  root-only and the game cannot read it.
- `BENCH oncpu` (since 2026-09-24, NEXT.md §2a P1) — is the frame computing or waiting?
  `thread_pct` is the render thread's own CPU time (`CLOCK_THREAD_CPUTIME_ID`) over wall time for
  the segment, and `thread_us` scales it to one frame period so it reads against `period_us`.
  `window_us`/`window_pct` are the same thread's CPU time inside the timed window, against
  `mean_us`. `proc_pct` is every thread in the process and can exceed 100 (audio is on its own
  thread). A thread near 100% is running and `perf record` will see where; well below 100% it is
  blocked and a sampling profiler will not — use `perf sched` (§6).
- **Since 2026-09-24 every baseline includes the asset-lookup cache** (`gSettings.LeanResLookup`
  defaults to 3): the frozen Market reads ~27.5 ms at 1250 MHz, not ~31.4. `lookup_hit`,
  `lookup_miss` and `lookup_flush` on the work line show the cache at work.
- `BENCH work` also counts asset lookups per frame (§8.22): `timg_hash`, `vtx_hash`, `dl_hash`
  (commands resolved by hash), `timg_path`/`vtx_path`/`dl_path` (by path), `timg_sig` (`G_SETTIMG`
  on an `__OTR__` string), `res_loads` (`LoadResourceProcess` entries over the whole period) and
  `res_loads_window` (inside the timed window). With `gSettings.LeanResLookup=3` `res_loads` should
  read 0 in a frozen scene; if it does not, the cache is being flushed.
- `BENCH work` also counts GL traffic (§8.24): `gl_calls` (every call the OpenGL backend makes into
  the driver), `gl_uniforms` (the `glUniform*` subset), `vbo_uploads`/`vbo_kb` (`glBufferData`
  calls and what they send; 0 with `gSettings.LeanGLState=3`, which draws from client arrays). A
  `BENCH glfn <seg>` line lists calls per GL entry point per frame, largest first. **GL call count
  is not a cost proxy on this driver**: removing 43% of them saved 0.5% (§8.24). Draws and uploads
  are what cost.
- `BENCH geom <seg>` (§8.26) counts geometry per frame: `vtx` transformed, `vtx_lit`, `vtx_unused`
  (overwritten before any drawn triangle used them; a lower bound), `tri_in` (`GfxSpTri1` calls),
  `tri_rect`, `tri_clipped`, `tri_culled`, and `tris` drawn. `tri_in = tri_clipped + tri_culled +
  tris`. At the Market 38% of submitted triangles are drawn.
- `BENCH light <seg>` (§8.27) counts inside the lighting loop per frame: `dir`/`pos` light
  iterations summed over lit vertices, `texgen` and `fog` vertices, and `recalc` (light-direction
  recomputes). Both fixtures run exactly two directional lights per lit vertex.
- `gSettings.ProbeSkipLight` (benchmark builds only) skips vertex work for a ceiling A/B: 1 =
  lighting + texgen + fog, 2 = lighting + texgen, 3 = fog. **Wrong picture by design.**
- `BENCH interp <seg>` (§8.29): `mtx` (`GfxSpMatrix` calls per frame), `mtx_repl` (answered by an
  interpolated matrix), `map` (replacement map size), and `children`/`ops`, the frame-interpolation
  recording's nodes and ops per frame period (recorded in `Play_Draw`, outside the window).
  **`ops` counts every recorded item, child nodes included** (§8.30 corrected §8.29 on this):
  recorded items = `ops`, not `children + ops`. `cmp`/`mismatch` are segment totals, non-zero only
  with `gSettings.LeanInterp=2`: replacement-map entries compared between the original and the
  arena recorder, and the ones that differed. `mismatch` must be 0;
  `gSettings.ProbeInterpCorrupt=1` is its positive control (~1 per frame).
- **Since 2026-09-25 `gSettings.LeanTexLoad` and `gSettings.LeanGameLookup` default to 1** (§8.31); the
  frozen Market makes 970 heap allocations per frame, not 3202. Set either to 0 only to A/B.
- **Since 2026-09-24 the interpolation recording lives in an arena** (`gSettings.LeanInterp`
  defaults to 1, §8.30): the frozen Market's render thread reads ~15.6 ms, not ~17.1. Set it to 0
  only to A/B against the original.
- `gSettings.ProbeSkipInterp` (benchmark builds only) skips frame interpolation with
  `InterpolationFPS` untouched: 1 = record nothing, 2 = record but do not interpolate. **Pixel-exact
  in a frozen fixture** (§8.29), wrong in play. Do not A/B `InterpolationFPS` 30 vs 20 instead: that
  changes how many frames each tick emits.
- `BENCH lookup <seg>` (§8.31): `calls` is resource lookups by name or CRC made by the render thread,
  counted once each at their outermost entry (a `ResourceMgr_*` helper, a bridge function or a
  `ResourceManager` method). `ResourceMgr_OTRSigCheck` is scoped too and is a prefix check, not a
  lookup: subtract it using the site table. `cvar` is CVar reads by name. Neither is visible to
  `res_loads`, which only counts cache misses.
- `BENCH ops <seg>` (§8.31): display-list commands per frame per opcode, largest first, named for
  the ucode loaded when the results are written.
- `BENCH alloc <seg>` (§8.31), only with `LD_PRELOAD=$PYRA_DIR/allocshim.so`: heap calls per frame
  period on the render thread. `new`/`delete` are every `operator new`/`delete`; `malloc` includes
  calloc, the memalign family and strdup. The shim costs time: **count with it, never time with it.**
- `SOH_BENCH_SITES=<dir on the device>` writes `lookup-sites-N.txt` per segment, and with the shim
  `alloc-sites-N.txt`: counts per frame binned by the immediate caller (its return address, so no
  unwinding is involved). Copy them back and run
  `python3 docs/pyra/tools/sites.py build-pyra/soh/soh.elf <files of one arm>`, against the binary that
  ran.
- `fb=start->end` is the game framebuffer size actually in use at the segment's first and last
  measured frame, read back from the interpreter rather than inferred from the CVar. For a
  resolution A/B it is the proof the arm took effect; for anything else it should be constant.

### Shader elision coverage, when a null needs explaining

`gfx_opengl.cpp` logs, per compiled shader variant, whether the combiner wrap was proved
redundant. It is `SPDLOG_DEBUG` and the release default is `info`, so raise the runtime level
first — the compile-time cutoff is already `TRACE`, so nothing needs rebuilding. The
`gDeveloperTools` section may not exist in the config yet; creating it works.

```sh
ssh -n "$PYRA_HOST" "python3 -c \"
import json
p='$PYRA_DIR/shipofharkinian.json'
d=json.load(open(p))
d['CVars'].setdefault('gDeveloperTools',{})['LogLevel']=1
json.dump(d,open(p,'w'),indent=4)\""

# ...then any run, e.g. bench.sh run $FIELD SOH_BENCH_WARMUP=300 SOH_BENCH_FRAMES=120

ssh -n "$PYRA_HOST" "grep -o 'shader .*wrap_out=[a-z]*' $PYRA_DIR/run.log | sort -u | tee /tmp/v.txt | wc -l; grep -c 'wrap_out=false' /tmp/v.txt"
```

`wrap_out=false` means elided. Verified 2026-09-23: prints `26` then `25` at the Hyrule Field
fixture, the same counts taken when the line was still `INFO`. This is what distinguished "the
optimisation misses these shaders" from "this frame is not fragment-bound" — Hyrule Field elides
25 of 26 and still gains nothing.

Put the level back afterwards (delete the key). Debug is not ruinous — a short run logs ~240 lines
— but leaving it on makes every later log harder to read.

## 5. Traps that have each cost real time

1. **Soak, or the absolute is meaningless.** Frame rate moves ~30% cold to hot, and
   `pyra-thermal.service` *drops the clock ceiling from 1.5 to 1.25 GHz when warm* — so an
   unsoaked run straddles the daemon's decision, which is a step change, the one shape ABBA cannot
   cancel. Always pass `SOH_BENCH_SOAK_C=70 SOH_BENCH_WARMUP_MAX=20000` (trap 9 on the target). It is free on a device
   that is already hot, so a back-to-back sequence pays it once.
2. **The game catches SIGTERM and dies slowly.** A plain `pkill` returns while a fullscreen window
   is still up. Instances accumulate, `xwd -root` photographs whichever is on top, and you end up
   comparing an arm against itself. Kill with TERM, wait, then KILL; `bench.sh watch stop` does it.
3. **A visual check without a control is worthless.** Two launches of the *same* arm differ by
   10–24k pixels, because particles and wandering NPCs settle somewhere different each launch and
   then hold still — no within-launch mask removes them. At the Market the control floor (24.3k)
   came out *higher* than the real comparison (22.3k). Three confident conclusions were wrong
   before this was understood.
4. **`ssh host 'cd x && cmd > log &'` hangs forever.** The `&` backgrounds the whole list, so the
   earlier commands hold the channel's stdout open. Group the redirect around the launch alone:
   `{ ENV... nohup ./soh.elf > log 2>&1 < /dev/null & }`.
5. **The frozen fixture measures drawing, not the game.** `Benchmark_FrameBegin/End` wrap only
   `DrawAndRunGraphicsCommands` (`OTRGlobals.cpp:1786`) — one rendered frame and its swap.
   `Play_Update` is outside the window, in every mode. A result here says a frame got cheaper to
   draw, not that the game got faster.
6. **`SOH_BENCH_STATIC=0` runs the game clock.** Useful for an animating display list, but keep
   the run under ~4 minutes: noon gives about six in-game hours before dusk, and Hyrule Field at
   night spawns Stalchildren while Market Day becomes a different scene.
7. **Don't pipe a script through `tail` while debugging it.** Buffering hides all progress and you
   cannot see where it stalled. Write to a file.
8. **Some CVars are not read live.** `ApplyArm` already handles `CVAR_TEXTURE_FILTER` and
   `CVAR_SHADER_OPTS` (shader cache dropped) and `gSettings.InternalResolution` (read once at
   `Interpreter::Init`, so `ApplyArm` pushes it through `SetResolutionMultiplier`; it is also the
   only *float* arm — every other CVar goes through `CVarSetInteger`, which would turn `0.5` into
   `0`). Anything else is assumed to be read per frame. Check before A/Bing a new one or you will
   measure nothing and believe it.
9. **76 °C is not always reachable, and missing it can cost the whole run.** On 2026-09-23 in the
   evening the die plateaued at 71–73 °C with the pin at 1.25 GHz, so the soak ran to
   `WARMUP_MAX=20000` (~11 min) before measuring. Add six pairs (~5 min) and the run crosses
   `bench.sh`'s default `timeout -s KILL 900`, which kills the game *before* it writes
   `bench.txt` — a quarter-hour for "no results". Check the first soak lines of `run.log`; if the
   temperature has flattened below target, lower `SOH_BENCH_SOAK_C` to where it flattened and say
   so in the result. Set `PYRA_TIMEOUT=1800` for any soaked A/B.
   70 °C is the working target since. On 2026-09-24, on the userspace pin, a warm device crossed
   it within a second and the segments then ran 73–80 °C at the Market and 78–81 °C at Hyrule
   Field — the latter just under `pyra-thermal`'s 82 °C rung, which capped three segments to
   1.0 GHz in one run (trap 12). Expect to discard the odd Hyrule Field segment.
10. **The frame cap was never off, until `uncap=1`.** The harness set the target FPS to 240 once;
    `Graph_ProcessGfxCommands` sets it back to `InterpolationFPS` on *every tick*
    (`OTRGlobals.cpp:1837`). So every run before 2026-09-23 23:20 ran capped at 30 FPS, with the
    limiter's sleep inside the timed window. A frame that fit under the cap then read as "33.3 ms
    minus the untimed work" whatever the change did — Hyrule Field showed a flat ~30.7 ms against
    4x fewer pixels, a 20% slower CPU and a 4x smaller window, when uncapped it runs at 21.1 ms.
    Every Hyrule Field null in report 8 §8.18 is this artifact. Market-type results, where frames
    overran the cap, were real but distorted. The config line now says `uncap=`; if it says `0`,
    or is missing, the result is capped. `SOH_BENCH_UNCAP=0` reproduces the old behaviour.
11. **`Failed flush to file ... Bad file descriptor`** in every log is a known open bug
   (report 10), not a symptom of your change. `RGBA Texture that isn't 16 or 32 bit. Size = 0`,
   ~36 lines at Market scene load, is also pre-existing noise.
12. **`pyra-thermal.service` moved the "pinned" clock, in both directions.** It rewrites
    `scaling_max_freq` every 2 s: 1.5 GHz below its 77 °C core rung (released only once the die
    falls 5 °C further), 1.0 GHz or lower once the fuel-gauge die passes 56 °C. The old pin was
    `min == max == 1.25 GHz`, so whenever the die was cool enough the daemon widened it to
    1.25–1.5 GHz and ondemand took the top. Found 2026-09-24 by the new `cpu_mhz` field: the first
    two Market segments ran at 1500 MHz. Every pinned result before that is only as trustworthy as
    its die temperature was high — above ~77 °C the daemon's cap happened to equal the pin. The
    pin is now the userspace governor with `scaling_setspeed`: a daemon ceiling above it is inert,
    a thermal cap below it still bites and shows in `cpu_min_mhz`. The daemon is left running on
    purpose — it is the thermal backstop and it owns the charger's current limit. If a run is ever
    killed so hard that `unpin_cpu` does not run, the device is left on `userspace` at 1.25 GHz;
    `echo ondemand | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor` puts it back.
13. **`bench.sh`'s output file can carry the results block twice, the second copy interleaved
    mid-line with other output** (seen 2026-09-24 in `ab` runs). Parse the first occurrence of each
    `BENCH segment`/`BENCH oncpu` line and ignore the rest. ~~or read `$PYRA_DIR/bench.txt`~~ —
    §8.29's first run had a log line spliced into the *first* copy of `BENCH segment 11`, **in
    `bench.txt` too**. `run.log` held it intact: `ssh -n "$PYRA_HOST" "grep -o 'BENCH .*'
    $PYRA_DIR/run.log"` is the clean source. The harness's own `paired` lines are computed
    in-process and are not affected.
14. **Back-to-back soaked A/Bs push the Market past 82 °C.** Two in a row (Hyrule Field then Market,
    2026-09-24) capped six of the Market's twelve segments to 1000 MHz, leaving three clean pairs.
    Let the die fall to ~65 °C between runs, or run the Market first. Cooling is not always enough:
    §8.26's Hyrule Field run started at 61 °C, after a Market run, and still capped pairs 3–6.
    `pyra-thermal` has hysteresis, so segments stay at 1000 MHz while the die reads 78 °C. Pairs
    whose two halves share a clock are still valid pairs. Analyse them per clock, and say so.

15. **The first segments can still be settling after the soak** (2026-09-24, §8.31). A Market A/B
    that reached 70 °C within seconds of the default 600-frame warmup read 17.98, 17.88 and 16.97
    ms of render-thread CPU in segments 0-2, then 15.8-15.9 ms flat for the other nine, at the pin
    throughout. That curve is not linear, so ABBA leaves it in the first two pairs. Pass
    `SOH_BENCH_WARMUP=2400`, and check the per-segment `thread_us` before believing the paired line.

16. **`SOH_BENCH_OPTIME=1` costs ~0.6 µs per display-list command** (two `clock_gettime` calls, not
    vDSO here): the Market's window goes from ~19 to ~32 ms. The no-op sync commands read the floor
    (~630-680 ns). Subtract it; use the result to choose a handler to skip-probe, never as a size.

17. **The game saves CVars on its own, so an A/B arm can end up in the config file** (§8.31:
    `gSettings.LeanCVar=1` did, and every later run booted with it). `bench.sh ab` now restores and
    saves the CVar; **watch mode does not** — after `watch stop`, remove the A/B CVar from
    `$PYRA_DIR/shipofharkinian.json`. Check that file for stray `Lean*`/`Probe*` keys before trusting a run.

18. **The host build segfaults on exit** in `Ship::ResourceManager`'s destructor (static
    teardown, seen every run 2026-09-24, with or without that day's changes). The results are
    already written by then. Ignore it, or fix it separately; it is not a benchmark problem.

## 6. Recipes

**"What shape is the game's data?"** — run the fixture **natively on the host** (§8.30). The host
benchmark build (`cmake --build build-cmake --target soh`) runs the same fixtures, and its `BENCH`
counters matched the device's exactly. Put the device's `oot.o2r` (copy it; the host's other copies
are different versions) and a `shipofharkinian.json` with the device's gameplay CVars and an
800×480 window in a scratch directory, and run
`SOH_BENCH=1 $MARKET SOH_BENCH_EXIT=1 ... build-cmake/soh/soh.elf` from there. The interpolation
recording's call stream is written with
`SOH_BENCH_INTERP_TRACE=<file> SOH_BENCH_INTERP_TRACE_TICK=600`.

**"Render-thread CPU, paired, with t and MDE"** — `bench.sh` pairs only the window and the period.
`python3 docs/pyra/tools/pairs.py run.log <arm A value>` pairs `thread_us`, `period_us` and the
window from `run.log`, and drops any pair with a segment off the 1250 MHz pin.

**"Is this change worth anything?"** — A/B it at Market *and* Hyrule Field, 5–6 pairs each, with
the soak. Two numbers, not one.

**"Did this change the picture?"** — `shotcheck.sh` at the busiest fixture you have, then read the
control number before the result number.

**"Is this scene fill-bound at all?"** — A/B `gSettings.InternalResolution 0.5 0.25`. It is the
direct test: a quarter of the pixels, every fragment cost with them. Read the `period` lines and
check `fb=` alternates. At the shipped 0.5× neither Market nor Hyrule Field is (4.9% / 6.3%,
§8.19). Make sure the config line says `uncap=1` — a capped run answers "no" for any scene that
fits under 33 ms.

**"Is the frame CPU-bound?"** — run the same fixture at `PYRA_PIN_FREQ=1250000` and `1000000`,
bracketed (1.25, 1.0, 1.25). Separate launches, so not ABBA; a 20% clock step is large enough to
see anyway, if the bracket agrees with itself. §8.19 had one bracket that did not — taken on the
old `min == max` pin, which the thermal daemon could lift (trap 12), so re-take it before using it.
Check `cpu_min_mhz`/`cpu_max_mhz` on every segment: at 1.0 GHz they must read 1000.

**"Is the frame computing or waiting?"** — the open question about 65% of it (NEXT.md §2a), and
nothing in this file answered it before 2026-09-24. A sampling profiler will not: a blocked thread
takes no samples and a spinning driver looks like work. Compare **on-CPU time against wall time**
for the render thread first — `perf stat --per-thread -p $(pidof soh.elf) -- sleep 30`, and read
`task-clock` per thread against the elapsed time, remembering that audio has its own thread on
core 1. On-CPU ≈ elapsed means it is running, and `-e cycles,instructions` then splits compute
from memory stalls by IPC. On-CPU well below elapsed means it is blocked, and the next question is
where:

**Answered for the Market, 2026-09-24 (§8.21): ~77% on-CPU, i.e. computing.** Read the
`BENCH oncpu` line rather than re-deriving it. What was learned running the tools:

```sh
PID=$(pgrep -x soh.elf)
sudo perf stat --per-thread -e task-clock,context-switches,cycles,instructions -p $PID -- sleep 20
# ioctls: there is no /dev/pvrsrvkm and no strace. The DDK uses /dev/dri/renderD130; ioctl is
# ARM EABI syscall 54. Each perf-script line carries fd and request as its first two args.
sudo perf record -e raw_syscalls:sys_enter --filter 'id == 54' -p $PID -o io.data -- sleep 10
sudo perf script -i io.data -F tid,time,trace
# flat on-CPU profile of the render thread; sort by dso first
sudo perf record -F 499 -t $PID -o rec.data -- sleep 25
sudo perf report -i rec.data --no-children --sort dso,sym --stdio
```

- **Uprobes do not exist on the `5.6.19-m4` kernel** (`CONFIG_UPROBE_EVENTS` is off), so
  `perf probe -x libc.so.6 malloc` fails, quietly with `-q`. `5.6.19-m4-uprobe` (hardware repo,
  `pc/pyra-crossbuild.sh kernel-m4-uprobe`) is m4 plus that option; it is installed in the test
  slot `l1` but is not the default. For counting heap calls `scripts/pyra/allocshim.so` needs no
  kernel change (§8.31).
- **Call graphs do not work here.** No `.debug_frame` in the shipping build; a `-g1` build adds it
  but the unwinder still stops at the stripped DDK and invents callers inside `soh.elf` (§8.21).
  Price call sites with `BENCH_` counters and A/Bs.
- `perf sched record -p` loses idle transitions and reports impossible run times; use `-a`.
- Generate `perf report` **after** the game exits, or it perturbs the segments it overlaps; with a
  debug-info build pass `--no-inline`, or `addr2line` runs for half an hour.
- Attach to the middle segments of a `bench.sh ab <cvar> X X` run and keep clean ones either side,
  so the harness shows which segments the profiler disturbed.
- **Where inside a function** (§8.26): release builds have no line tables, and `perf annotate`
  finds no samples once a newer `soh.elf` is deployed over the recorded one. Take the IPs with
  `perf script -F ip,dso --show-mmap-events`, subtract the `soh.elf` mmap base, pull the binary
  from `/root/.debug/<path>/<build-id>/elf`, and bin by 16-byte region against `llvm-objdump
  --triple=thumbv7a-linux-gnueabihf`. The PCs skid onto the instructions after a `VDIV` or `VMRS`,
  and the counts **overstate** what removing that instruction saves, by ~3× in §8.26 — and
  **understate** a dependent chain by ~2.5× in §8.27 (lighting: sampled ~0.33 ms with the
  transform, measured ~0.8 ms alone). Use the result to pick a candidate, and an A/B to size it.
- No GPU-side profiler is known to work on this part (`~/git/dragonbox-pyra/docs/profiling-guide.md`
  §3.2). The DDK does ship `libPVRScopeServices.so`, which is PowerVR's counter library; untried.

Run it on the frozen Market fixture, uncapped, on the userspace pin, after the soak — and cover
both ends of a run, because the Market's frame period creeps +4% across five minutes at a fixed
clock (§8.20).

**"Would this loop rewrite be faster?"** — microbenchmark it on the device before building it into
the game (§8.28). Compile with the cross container and the game's flags, pin the clock, check the
outputs are identical, compare ns per element. `docs/pyra/tools/lightbench.cpp` is the template;
its header has the commands. It took seconds and predicted a null that a soaked A/B then confirmed.
`tools/interpbench.cpp` (§8.30) is the template for **allocation-heavy** code. A single-threaded
benchmark reads glibc's malloc ~30% cheap (it skips its locks until a second thread exists), so
pass `mt`, and a thrash size to model the game's cold caches.

**"Did my new default actually take effect?"** — remove the CVar from the device's
`shipofharkinian.json`, then `bench.sh run` one segment and compare the absolute against the known
arm values. An A/B cannot answer this: it sets the CVar itself.

## 7. Where the detail lives

| | |
|---|---|
| [`NEXT.md`](NEXT.md) | the live queue. Newer than any report. |
| [09](09-benchmark-harness.md) | the harness: design, validation, every env var, the fixtures. |
| [08](08-optimisation-opportunities.md) | results, and §8.18 on why one fixture is not a measurement. |
| [10](10-save-file-corruption.md) | the save bug and `fdguard.c`. |
