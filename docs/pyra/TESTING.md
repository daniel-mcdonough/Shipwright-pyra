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

`pyra.local` — the `<pyra-ip>` in the hardware repo is stale. Two install directories:

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

A fixture is a scene, a player position and a pinned camera. **Use at least two.** The same shader
change measured −3.59%, −2.40% and −0.07% across these three (§8.18) — scene choice moves an answer
sevenfold, far more than the rig's 0.5% resolution ever will.

**Market Day** — a dozen NPCs, near and far buildings. Actor-dense, fragment-bound.

```sh
MARKET="SOH_BENCH_ENTRANCE=0x00B1 SOH_BENCH_POS=-122.7,0.0,72.1 SOH_BENCH_YAW=0xD5A6 \
SOH_BENCH_EYE=0.0,64.0,0.0 SOH_BENCH_AT=-134.5,64.0,79.0 SOH_BENCH_SETTLE=400"
```

**Hyrule Field** — open plain, long draw distance. **Not fragment-bound**: three-point filtering
costs +0.11% here. The control case for any shader change.

```sh
FIELD="SOH_BENCH_ENTRANCE=0x00CD SOH_BENCH_POS=1089.4,379.7,5669.1 SOH_BENCH_YAW=0xA1F9 \
SOH_BENCH_EYE=1217.7,458.2,5780.3 SOH_BENCH_AT=1089.4,424.7,5669.1 SOH_BENCH_SETTLE=400"
```

**Kokiri Forest** — enclosed village square, layered combiners.

```sh
KOKIRI="SOH_BENCH_ENTRANCE=0x00EE SOH_BENCH_POS=-592.8,0.0,-195.9 SOH_BENCH_YAW=0x369E \
SOH_BENCH_EYE=-707.3,55.3,-222.8 SOH_BENCH_AT=-592.8,34.7,-195.9 SOH_BENCH_SETTLE=400"
```

Market and Hyrule Field bracket the answer between them; Kokiri sits in the middle and adds little.

## 3. The four things you can run

### A/B a CVar — the main tool

```sh
PYRA_TIMEOUT=1800 ./scripts/pyra/bench.sh ab gSettings.ShaderOpts 0 3 $MARKET \
    SOH_BENCH_SOAK_C=76 SOH_BENCH_WARMUP_MAX=20000 SOH_BENCH_REPS=6
```

One process, arms swapped between segments, ABBA counterbalanced, CPU pinned to 1.25 GHz for the
duration and restored on exit. A segment is 600 measured frames plus a 180-frame recompilation
grace, so ~25 s; `reps` is **pairs**, so `REPS=6` is 12 segments and roughly 5 minutes, `REPS=8`
is 16 and roughly 7. Add settle and soak. Run it with `run_in_background` and read the output file
— `bench.sh` has its own timeout (`PYRA_TIMEOUT`, default 900 s), so a hung harness will not hang
you. The default is too short for a soak that has to run long; see trap 9.

Five or six pairs resolves around 0.5% on a quiet device; a single noisy segment can push that to
1.5%, which is why the `mde_us` line matters more than the `t`. If you need to separate "no
effect" from "no effect this could see", add pairs.

### One arm, absolute numbers

```sh
./scripts/pyra/bench.sh run $FIELD SOH_BENCH_SOAK_C=76 SOH_BENCH_WARMUP_MAX=20000 SOH_BENCH_REPS=3
```

Use this to verify a *default* took effect, where an A/B would override the very thing you are
testing. **Never quote an absolute taken without `SOH_BENCH_SOAK_C`** (trap 1).

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

```sh
./scripts/pyra/shotcheck.sh gSettings.ShaderOpts 0 3 $MARKET
```

Three launches: arm A, arm B, then arm A again as an **in-session control**. Reports both numbers
and passes only if the real comparison is near the control. The control is not optional — see
trap 3.

## 4. Reading the output

```
BENCH segment 3 arm=0 frames=600 mean_us=30580.2 ... mean_fps=32.701 temp_c=81.5->80.6 fb=640x360->640x360
BENCH pair 1 a_us=30580.2 b_us=30573.1 delta_us=-7.1 delta_pct=-0.02
BENCH paired n=8 mean_delta_us=-20.54 sd_us=164.36 t=-0.353 df=7
BENCH mde_us=137.22 (2.365*se at df=7)
```

- `delta` is **arm B minus arm A**, so negative means B is faster.
- `|t| > 2.4` or so is a real effect at these sample sizes. `t = -23.7` is not a subtle result;
  `t = -0.35` is nothing.
- `mde_us` is the smallest effect the run could have detected. A null result is only meaningful
  next to it.
- `temp_c=start->end` — two runs whose temperature ranges do not overlap are not comparable in
  absolute terms however tight their error bars.
- A segment whose temperature *falls* mid-segment is suspect; one produced a −2.48% outlier that
  did not reproduce.
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
   cancel. Always pass `SOH_BENCH_SOAK_C=76 SOH_BENCH_WARMUP_MAX=20000`. It is free on a device
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
10. **`Failed flush to file ... Bad file descriptor`** in every log is a known open bug
   (report 10), not a symptom of your change.

## 6. Recipes

**"Is this change worth anything?"** — A/B it at Market *and* Hyrule Field, 5–6 pairs each, with
the soak. Two numbers, not one.

**"Did this change the picture?"** — `shotcheck.sh` at the busiest fixture you have, then read the
control number before the result number.

**"Is this scene fragment-bound at all?"** — A/B `gSettings.TextureFilter` 1 vs 0. Three-point
filtering is pure fragment ALU and worth 3.6–19.9% where it bites; if it is free here, nothing
shader-side will help.

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
