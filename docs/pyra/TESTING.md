# Testing on the Pyra — quickstart

Everything needed to measure or visually check a change, without reading the reports first.
Copy-pasteable. The *why* behind each rule is in report 9 (the harness) and report 8 (results);
this file is the operating manual and is kept current.

**Before anything: the device must be free.** It is someone's handheld. Do not launch, deploy or
benchmark on it unless the owner has said so in the current session.

---

## 0. Session setup

```sh
. ~/git/dragonbox-pyra/pyra-local.conf    # finds the agent holding the Pyra's key, exports PYRA_HOST
export PYRA_DIR=/home/pyra/soh-bench2
```

Several ssh agents run at once and only one holds the key the Pyra accepts. The old one-liner here,
`ls -t ~/.ssh/agent/s.*.agent.* | head -1`, picked the newest socket, and on 2026-09-25 that was the
wrong one: `Permission denied (publickey,password)` is this, not a missing key. `pyra-local.conf`
(gitignored, in the hardware repo) picks the agent by the key's fingerprint and prefers USB ethernet.

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

### Launching by hand (a demo or a look, not a measurement)

`bench.sh` starts the game with two variables that pick the SGX driver. A bare `./soh.elf` over ssh does
not, and SDL then loads the wrong GL library: the first shader fails to compile with an **empty info
log** (`vertex shader compilation failed (shader_id0=0x00000000d0001000 ...)`, a trivial pass-through
shader) and the game aborts with signal 6 (2026-09-28). Launch it the way `bench.sh` does, detached so
ssh returns:

```sh
ssh -n "$PYRA_HOST" "cd $PYRA_DIR && { SDL_VIDEO_X11_FORCE_EGL=1 \
    SDL_VIDEO_GL_DRIVER=/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1 DISPLAY=:0 \
    nohup ./soh.elf > demo.log 2>&1 < /dev/null & }"
```

The `{ ... & }` form is what keeps ssh from waiting on the game (trap 4). Without `SOH_BENCH` the harness is
off, and nothing pins the clock. Report 11's GL thread (`gSettings.RenderThread` 2) can be in the config for
a launch since libultraship `b32aac46` (§11.13): five hand launches and three harness runs started in mode 2
or 4 drew and checked clean. The owner's own install, `~/soh-pyra`, is launched from the desktop and is not
ours to start.

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
**None of the three draws a pre-rendered background** (§8.35: in `oot.o2r` only the interiors, e.g. the
Kokiri houses and shop, have a `…Background_…` resource). For a change that could touch that path, use
watch mode and go indoors.

## 3. The things you can run

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

**Several probe arms in one run (§8.48).** `bench.sh run $MARKET SOH_BENCH_AB_CVAR=<cvar>
SOH_BENCH_AB_VALUES=0,7,0,24,0,992 SOH_BENCH_REPS=4`: with more than two values the arms go round-robin,
not ABBA, so interleave the baseline and read the run with `docs/pyra/tools/bracket.py run.log 0 1000`,
which takes each probe segment against the mean of its two neighbours. The last segment of the run has
no neighbour after it and is dropped; give the probes one more rep than you need. For probes only: a
fix still gets the two-arm `ab` and `pairs.py`.

**Watts by arm in one process (§8.52).** `SOH_BENCH_ARM_FILE=<file>` makes the harness write
`<SOH_BENCH_ARM_PREFIX><value>-<n>` to the file at every arm change (n counts the changes, so ABBA's
back-to-back segments of one arm stay apart). Point it at thermcost's phase file and the battery gauge's
rows carry the arm: `thermcost.sh present` in the hardware repo is the worked example, and `thermclock.py
DIR present` reads it. Use it for anything read live, instead of restarting the game per arm.

**The config file nests CVars by prefix.** `~/soh-bench2/shipofharkinian.json` holds
`"CVars": {"gSettings": {"LeanCVar": 1, ...}}`, so grep it for `"Lean`, not `gSettings.Lean`. A check
for stray keys that greps the dotted name finds nothing and looks clean.

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

Keep `SOH_BENCH_UNCAP=0` — uncapped play runs the game too fast — and `PYRA_WATCH_PIN=1`.
Since §8.43 every block line also carries `hist_ms=34:a/40:b/50:c/67:d/+:e` (frames by end-to-end
period, upper edges in ms), `max_ms`, and for the frames over 40 ms `slow`, `slow_ms`, `slow_cpu_pct`
(render-thread CPU across those periods: high means computing, low means waiting) and `slow_swap_ms`.
Add `SOH_BENCH_SPLIT=<ticks>` for §8.33's logic/render line plus a `Benchmark split_blocks` line: the
§8.43 blocks in CPU ms per tick for the same window. It costs ~50-65 µs per frame, so leave it off in an A/B. Under
the cap the frame period is the cap, so compare **`thread_cpu_us`** (render-thread CPU per frame)
across adjacent ABBA pairs; blocks differ a lot in content, so it takes ~50 pairs (~17 minutes) to
resolve a ~10% effect. Every block line carries `scene=` and `cpu_min_mhz`/`cpu_max_mhz`.
`python3 docs/pyra/tools/watchpairs.py blocks.txt <arm A value> [MHz]` pairs them, dropping pairs off the
pin (1250 MHz unless given — **pass 1000 for a `PYRA_PIN_FREQ=1000000` run**, or every pair is dropped, as
in §8.41's first pass) or across a scene change, and prints the delta with t and MDE (§8.30).

**Miss rates, and two clocks in one session** (§11.14). `python3 docs/pyra/tools/slowpairs.py blocks.txt arm <A> <B>
[MHz]` gives frames over 40 ms per scene and arm, pooled and paired by block (trap 26: report both). To compare
clocks instead of a CVar, copy `docs/pyra/tools/clockflip.sh` to the device and start it after `watch start`
(its header has the command): it moves the userspace pin between 625 and 750 MHz every 30 s, and
`slowpairs.py blocks.txt clock 750 625` pairs single-clock blocks, dropping the ~30% that straddle a change.
Kill it before `watch stop`. For two voltages of the 750 MHz point, `docs/pyra/tools/voltflip.sh` does the same with
`pyra-undervolt set` (870/950 mV every 30 s, 870 left set at the end); pair with `slowpairs.py watch.log volt 870000
950000 voltflip.log 750` — the pulled `watch.log` itself, since it needs the block lines' timestamps (trap 33).
slowpairs.py also pairs `thread_cpu_us` the same way, in every mode.

For a session without an A/B (where play is heaviest, §8.43 (c)), pull the whole log and read it by scene:
`scp "$PYRA_HOST:$PYRA_DIR/watch.log" . && python3 docs/pyra/tools/watchsplit.py watch.log [MHz]` (the
session must run with `SOH_BENCH_SPLIT=<ticks>`: without split windows it dies on a ZeroDivisionError, §8.50) gives
frames over 34/40/67 ms per scene, what the slow frames were made of (render-thread CPU, swap, the rest),
and the `split_blocks` table by scene with a whole-session column. For the rest of the machine at the same
time, copy the hardware repo's `device/pyra-cpu-attribution.sh` to the device and run
`~/pyra-cpu-attribution.sh -d 300 -t soh.elf` there (with nohup, several windows back to back). Its process
table before hardware-repo commit 92f809b dropped any process whose CPU ticks crossed a digit, soh.elf
included; the thread and per-core tables were fine.

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

**Prefer the frame dump (§8.32).** `SOH_BENCH_DUMP=<dir>` writes the game's render target as
`seg<k>-f<n>.ppm` at frames n and n+1 of every segment (n = `SOH_BENCH_DUMP_FRAME`, default 300),
so both arms are captured at the same frame whatever their speed, with no screen grab. Compare with
`docs/pyra/tools/dumpcmp.py <dir>`. The frozen Market is not perfectly still: consecutive frames of
one arm differ in ~15-36 pixels (a short animation cycle), so compare the same frame index across
arms and read the within-arm numbers as the floor. Segments can start on different phases of the
game's 2-frames-per-3-ticks interpolation cycle, so frame n of one segment may match frame n+1 of
another: look at both. At Hyrule Field mask the sky (rows 216 and up of a 360-row dump, which is
stored bottom-up). If a comparison differs, `<frame>.ppm.vbo` and `<frame>.ppm.mtx` hold every
draw's vertices and every matrix of that frame; `tools/vbocmp.py` and `tools/mtxcmp.py` diff them.
The grab method below is what §8.22-8.31 used.

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

### Watts and die temperature — needs the device on battery and left alone

The only power instrument is the battery gauge, so every figure is a difference between arms, and the
device must be off the charger (the script waits for `bq24297/online` to read 0). It runs entirely on the
device, so a dropped network doesn't stop it:

```sh
scp ~/git/dragonbox-pyra/experiments/power-budget/thermcost.sh "$PYRA_HOST":thermcost/
ssh -n "$PYRA_HOST" 'cd ~/thermcost && SOH_DIR=/home/pyra/soh-bench2 setsid nohup ./thermcost.sh clock > driver.log 2>&1 < /dev/null &'
# ~85 minutes later (the log ends "done, restored"):
scp "$PYRA_HOST":'thermcost/thermcost-clock.*' "$PYRA_HOST":'thermcost/thermcost-run-*.log' .
python3 ~/git/dragonbox-pyra/experiments/power-budget/thermclock.py .
```

`clock` mode (§8.44) runs ABBA 60 s blocks for watts, 10-minute holds for die temperature, and reads the
game's fps per clock from its `BENCH split` lines. Edit its phase list for other arms; keep ABBA inside one
game process, because a restart is a load and a cold start. 20 fps goes through the harness's A/B CVar
(`gSettings.InterpolationFPS`, one value); the CVar lands when the fixture's segment starts, ~45 s after
launch, so the script warms for 60 s first. It backs up and restores `shipofharkinian.json`, the
governor and both backlights. Absolute watts depend on the backlights and wifi: compare arms within one run.
Die temperature needs the holds: a 60 s block is far from equilibrium.

### GPU: how busy is it, and what does a frame draw?

Two tools, both set up in `~/git/dragonbox-pyra/experiments/gpu-tools/` (its README has the
background and the build). Use them for GPU questions, not the swap time: `swap_us` is time the
render thread spends blocked, and on this tiler it mixes GPU work, presentation and the frame cap.
First results: report 08 §8.42.

**PVRTune: GPU busy time per frame.** Imagination's server reads the SGX's HWPerf stream: a start and
an end for every TA (tiling) and 3D (fragment) kick. It is already on the device in `~/gpu-tools/`.
The desktop GUI is in the hardware repo's `experiments/gpu-tools/pvrtune/` (git-ignored). Record to a
file on the device while any `bench.sh` run goes, then export on the host:

```sh
G=~/git/dragonbox-pyra/experiments/gpu-tools
ssh -n "$PYRA_HOST" 'cd ~/gpu-tools; rm -f cap.pvrtune; { LD_LIBRARY_PATH=/opt/omap5-sgx-ddk-um-linux/lib \
  nohup ./PVRPerfServerDeveloper --sendto=/home/pyra/gpu-tools/cap.pvrtune --qat=300 --pid \
  > perf.log 2>&1 < /dev/null & }'
PYRA_PIN_FREQ=1000000 ./scripts/pyra/bench.sh run $MARKET SOH_BENCH_UNCAP=0 SOH_BENCH_WARMUP=300 SOH_BENCH_FRAMES=900
ssh -n "$PYRA_HOST" 'pkill -INT -f PVRPerfServer'          # -INT, so the file is closed
scp "$PYRA_HOST":gpu-tools/cap.pvrtune $G/captures/
( cd $G/captures && ../pvrtune/GUI/Linux_x86_64/PVRTuneDeveloperGUI cap.pvrtune \
    --export-csv-timing=cap-timing.csv --export-csv-counters=cap-counters.csv )
$G/pvrtune_frames.py $G/captures/cap-timing.csv 12 54   # seconds into the capture: pick the steady part
```

`pvrtune_frames.py` prints GPU busy (TA or 3D) as a share of wall time, and 3D time per frame split
into the scene render (the 640×360 game framebuffer) and the present render (the 1280×720 window
pass). The counters CSV has the tool's derived rates every 500 ms: tiler/renderer active %, frames per
second, vertex and pixel load, HSR efficiency, memory words/s, per-CPU load. The GUI shows the same
data live, as a timeline: run the server without `--sendto` and connect to port 6520 from the GUI.

- Its help flag is `-h`. `--help` is ignored and the server starts and waits.
- In file mode the server records one counter group (`--group=N`, default 0). The timing events do
  not depend on the group.
- The "GPU clock speed" counter reads 270-410 MHz. The clock is a fixed 425.47 MHz, so don't trust
  that counter, or the per-cycle counters derived from it. The kick timestamps are fine.
- It is a separate process reading a driver buffer and has not been priced as a perturbation. Don't
  run it during an A/B you are timing.

**apitrace: what the frame draws, call by call.** An `LD_PRELOAD` wrapper records every EGL/GL call.
`bench.sh run` passes the variables straight through:

```sh
./scripts/pyra/bench.sh run $MARKET SOH_BENCH_UNCAP=0 SOH_BENCH_WARMUP=30 SOH_BENCH_FRAMES=30 \
    LD_PRELOAD=/home/pyra/apitrace/lib/apitrace/wrappers/egltrace.so \
    TRACE_FILE=/home/pyra/gpu-tools/soh-market.trace
scp "$PYRA_HOST":gpu-tools/soh-market.trace ~/.cache/pyra-traces/     # 4.4 GB for 460 frames
qapitrace ~/.cache/pyra-traces/soh-market.trace                       # step through draws, state, textures
apitrace dump --calls=<from>-<to> ~/.cache/pyra-traces/soh-market.trace   # or read it as text
```

- A traced run is huge and slow (~2 fps, 4.4 GB for a 460-frame Market run with the 400-frame
  settle). Its timings mean nothing.
- The desktop replays it on Mesa and draws the right picture. `apitrace trim` output does **not**
  replay: it leaves out the EGL context and the textures. `gltrim` keeps nearly everything.
- Replay on the device needs the patched `eglretrace` (the DDK's DRI3 WSEGL aborts in
  `eglWaitNative`) and `LD_LIBRARY_PATH=~/ddk-shim`. It works but runs at ~2 fps, parsing the trace.
  No timing from it yet.

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
- **The window's wall time is not all work.** `mean_us` includes time the render thread is blocked
  (GPU waiting, mostly in the swap), and that moves whenever the game side outside the window gets
  faster or slower. For a change that runs outside the window, read the window's CPU time
  (`BENCH oncpu … window_us`, `win_cpu_us` in `pairs.py`): §8.30's "+295 µs window regression" was
  +86 µs of CPU and the rest waiting (§8.34).
- `gSettings.ProbeSpinUs=N` and `gSettings.ProbeTouchKB=N` (benchmark builds, §8.34) run once per game
  tick outside the window: a CPU spin of N µs, and a write of one of two alternating N KB buffers plus
  a read of both. Use them to test whether game-side time or cache traffic reaches the window.
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
  `InterpolationFPS` untouched: 1 = record nothing, 2 = record but do not interpolate, 3 = walk as
  usual but build no replacement map (§8.36, the map's ceiling). **Pixel-exact in a frozen fixture**
  (§8.29), wrong in play. Do not A/B `InterpolationFPS` 30 vs 20 instead: that
  changes how many frames each tick emits.
- **Since 2026-09-25 evening `gSettings.LeanInterpMap` defaults to 1** (§8.36): the replacement map is
  moved out instead of copied. The frozen Market makes **178** heap allocations per frame, not 291. Mode
  2 also recycles the map's nodes (67 per frame, no faster); mode 3 checks mode 2 against a fresh map
  (`cmp`/`mismatch` on the interp line; `gSettings.ProbeInterpCorrupt=2` is its control).
- `eof_cpu_us` on the `BENCH oncpu` line (§8.36): the render thread's CPU time from the backend's
  `EndFrame` through the swap, per frame. ~280 µs at both fixtures: the swap's 5 ms is waiting.
- `BENCH split <seg>` (§8.43), only with `gSettings.ProbeSplit=1` or `SOH_BENCH_SPLIT` set: the whole
  main thread in fifteen contiguous blocks, per frame period, as `cpu/wall` µs, in the order a tick runs
  them (`input update draw post_draw interp events between_frames gui_start fb_start run gui_end eof
  recycle audio_wait`, plus `between`, the gap between ticks). `cpu_sum_us` must equal the oncpu line's
  `thread_us` (it does, within ~2 µs) and the window blocks `gui_start`..`eof` its `window_us` plus ~19 µs
  of harness. What each block contains is in `benchmark.h` and the §8.43 table. The marks cost ~50-65 µs
  of CPU per frame (§8.43): **don't time anything else with them on.** `python3 docs/pyra/tools/splitblocks.py
  run.log <arm> [MHz]` averages an arm's segments and prints the closure checks.
- `gSettings.ProbeSkipDraw` (benchmark builds only, §8.36): 1 = no `glDrawArrays` in the GLES2
  backend's `DrawTriangles`, 2 = none of its GL work. **Wrong picture by design** (nothing is drawn).
  At the Market mode 1 is −4.1 ms of render-thread CPU: the draw calls are a quarter of the thread.
  3 issues every draw twice with no state change between, 4 repeats only its first triangle (§8.37):
  +6.5 and +4.3 µs per draw at the Market. Read `win_cpu_us`: the doubled GPU work lands in the swap.
  5 and 6 (§8.38) repeat it after binding another texture to unit 0 and back, or with the other texture
  bound; A/B them against 3, not 0.
  7 and 8 (§8.39) repeat it after re-uploading the texture-size uniform unchanged, or changed; against 3.
  9 and 10 (§8.40) repeat it after re-issuing the program's attribute pointers unchanged, or pointed
  elsewhere and back; 11 after `glUseProgram` of another program and back. Against 3. The driver skips an
  unchanged pointer (mode 9 reads ~0), so only an away-and-back probe prices a real change.
- `BENCH batch <seg>` (§8.37): draws per frame by the set of state changes that split each from the
  previous batch. `cause=any/alone` per cause, then the commonest sets. `tex_same` is a reload of the
  texture already bound (NEXT.md item H); `other` should read ~1 (the frame's last draw). If `other`
  reads high, a flush site is marking after it flushes.
- **Since 2026-09-25 night `gSettings.LeanTexReuse` defaults to 1** (§8.37): no batch split on a
  same-texture reload. The frozen Market draws **228** per frame, not 261, and `tex_same` reads 0. Set it
  to 0 only to A/B against the original.
- `BENCH group <seg>` (§8.40): what sorting by texture could remove. `runs` are maximal sequences of
  consecutive opaque, depth-written, non-decal draws with no viewport, scissor or framebuffer change
  between (the backend bumps `gBenchFbEpoch` at every framebuffer switch, clear, copy and frame start);
  `draws` in them; `tex_dup`/`key_dup` the draws whose texture set / texture set and program an earlier
  draw of the run had; `tex_chg` → `tex_chg_sorted` texture-set changes inside runs as issued and if
  sorted. Frozen Market: 11, 159, 86, 85, 148 → 62. The host's native build gives the same numbers.
  `lg_deferred`/`lg_draws`/`lg_flushes`/`lg_buckets` (§8.41) are what `gSettings.LeanGroup=1` did: draws
  deferred, `glDrawArrays` its replays issued, replays of a non-empty set of buckets, buckets. All 0 with
  it off. Frozen Market: 159, 76, 12, 76; `lg_draws` above `lg_buckets` means buckets outgrew their
  chunks. **With it on, `draws` on the work line still counts `DrawTriangles` calls; read real draws
  from `glDrawArrays` on the glfn line** (Market 228 → 145).
- **Since 2026-09-25 late evening `gSettings.LeanGroup` defaults to 1 on GLES2** (§8.41, with
  `LeanGLState=3`): deferred, grouped opaque draws. The frozen Market draws **145** per frame, not 228,
  and its window CPU reads ~13.5 ms at 1000 MHz, not ~14.4. Set it to 0 only to A/B against the original.
  **Not exact**: where two opaque surfaces tie in depth the other may win (Hyrule Field: 26 pixels at ±1
  level on alternate frames). Compare dumps at both frame phases and expect that, not zero. The `.vbo`
  trace stays in `DrawTriangles` order, so `vbocmp.py` does not see the reordering.
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
    **The device build does it too** (2026-09-29): every Alt+F4 quit, four of four, in `RenderThread` 0 and
    2 alike, ends in `Segmentation fault` about a second after the close. Not the GL thread. Closing the
    window with `xdotool windowclose` is not a quit (it destroys the window; the game hangs): use Alt+F4.

19. **Until §8.32 the camera override did not reliably hold.** The freeze does not stop
    `Camera_Update` (it runs after `Play_Update`'s frame-advance skip label) and the camera wrote its
    own look-at over the override every tick, until it happened to stop - which turned on
    uninitialised reads in the camera code, so an unrelated code change could flip a fixture
    between the game's camera and the requested one. At the Market the two differ by 0.006° of yaw
    and 122k pixels: §8.31's "LeanCVar changes the picture" was this. The override is now applied
    from `OnPlayDrawBegin`, just before `View_Apply`. Timings are unaffected at that size; a visual
    comparison across builds made before the fix is not trustworthy. `SOH_BENCH_VIEW_LOG=<file>`
    logs the eye and look-at of every viewing matrix built, to check.

20. **The host is not a stand-in for the device's picture.** The host drew the same picture for
    `LeanCVar` 0 and 1 while the device did not (trap 19: stack contents differ by platform). Take
    visual checks on the device.

21. **At a 1000 MHz pin the Market does not reach 70 °C.** §8.43's soak gave up after 20,000 frames
    (~8 minutes) at 67.0 °C. The run was still valid, since every segment was on the pin at 67-69 °C,
    but it spent the time for nothing. At 1000 MHz use `SOH_BENCH_SOAK_C=66`; Hyrule Field then ran
    67-72 °C.
22. **A desktop dialog can resize the game mid-run, and the game saves the new size** (2026-09-27).
    When USB power drops (it does under load, even plugged in), mate-power-manager raises a "Battery
    Discharging" dialog. The game lost fullscreen and ran at 1278×650 (game framebuffer 639×325) for
    the rest of the run, with a different view (Hyrule Field 155 → 169 draws), and wrote
    `Window.Fullscreen` 1278×650 into `shipofharkinian.json`, so later runs started at that size too.
    Three runs were lost to it before anyone looked. **Check every run:** `BENCH config window=` must
    read 1280x720 and every segment `fb=640x360->640x360`; look at the screen (`xwd`, §3) if not, and
    put `Window.Fullscreen` back to 1280×720 before the next run.
23. **`pkill -f <script>` kills the shell that runs it** (2026-09-27). The pattern is on that shell's own
    command line, so `pkill -f pinab.sh; ...` killed the command before it did anything else. Use
    `pgrep -f '[p]inab.sh'` (the bracket keeps the pattern from matching itself) and `kill` the PIDs, and do the
    same on the device. A related trap: an agent's background job can be killed at its tool timeout (10 min).
    Drive any multi-run A/B longer than that with `setsid nohup <driver> > log 2>&1 < /dev/null &` on the host
    as well as on the device, and poll the log.
24. **In `gSettings.RenderThread` 2 (report 11), the backend's counters are approximate** (2026-09-27). The
    GL thread writes them and the main thread reads and resets them, without a lock, a frame apart:
    `BENCH work`/`group`/`batch`/`glfn`, `swap_us`, `eof_cpu_us`, and the watch lines' `draws` (~113 in
    mode 2 against ~205 in mode 0 at the Market; the dumps prove the drawing is the same). `thread_us`, the
    period, the window and the `BENCH rthread` line are exact. Read the GL thread from `gl_us`, and the
    render side's whole CPU from `pairs.py`'s `total_us` (main + GL thread). `main_us` means something for
    mode 1 runs only.
25. **An A/B between two arms of the same new code cannot see a bug they share** (2026-09-27, §11.8). The
    first recording proxy answered `GetClipParameters` from a cached value in *every* mode, so both arms
    drew upside down and the dumps compared equal; the owner saw it on screen. The reference arm must run
    the old code path (the proxy's mode 0 now forwards every call), and a control that the check can see
    the fault (here, the old flipped frame against the fixed one: 95,710 pixels) belongs in the run.
26. **Real-play miss rates are bursty: test them by block, not by frame** (2026-09-27, §11.11). Pooled
    frame counts gave the Market 3.22% → 1.61% and Death Mountain Trail 3.19% → 1.09%, which look decisive;
    paired by block (the independent unit) the same session read −0.66 points, t −1.74, under its MDE.
    Report both. Three blocks per arm in a scene is not enough.
27. **A new source file in libultraship needs a CMake reconfigure** (2026-09-27). `src/fast/CMakeLists.txt`
    globs `backends/*.cpp`, so the build links without the file (undefined references). On the host:
    `cmake build-cmake`; for the device: `./scripts/pyra/build.sh configure -DSOH_BENCHMARK=ON` again.
    Either rebuilds most of libultraship (~10 min host, longer in the armhf container).
28. **An external controller changes the machine a little, not the A/B** (2026-09-27, not measured). A
    watch-mode A/B alternates arms every ~10 s in one process, so a controller costs both arms alike. It
    does move absolute numbers against earlier sessions: a Bluetooth pad adds radio interrupts and
    `bluetoothd` wake-ups (device IRQs are all on cpu0, §8.47), a USB one adds polling. Say which input was
    used in the report, and compare absolutes only within one input method.

29. **The Pyra can freeze on its own through its WiFi driver** (2026-09-28). ssh times out *during the banner
    exchange* on both interfaces while both still answer ping, and the mouse is dead: that is not the game.
    The wl18xx driver looped in a hardware restart and flooded the kernel log (~17 stack traces a second).
    Recovery that worked: the power button's shutdown dialog, left to count down, shuts down cleanly. The
    details, why the journal lost the start, and the mitigations (none applied) are in the hardware repo,
    `docs/troubleshooting.md`, "The WiFi driver can flood the kernel log". Before blaming a change under test
    for a freeze, check `sudo journalctl -b -1 | grep -c 'cut here'`.

30. **`SOH_BENCH_DUMP` does not create its directory** (2026-09-28). With the directory missing, the run
    completes and measures normally, and the only trace is `[error] Benchmark: cannot write frame dump ...`
    in `run.log`, four times. `ssh -n "$PYRA_HOST" "mkdir -p $PYRA_DIR/<dir>"` before the run, and count the
    `.ppm` files before comparing.

31. **The device build runs in `RenderThread` 2 by default since §11.15** (2026-09-29). A fixture or watch
    run with no `RenderThread` key and no A/B on it measures the GL thread, where every number before
    2026-09-29 was mode 0 unless it said otherwise: main-thread CPU ~18% lower, the backend's counters
    approximate (trap 24). To compare with older absolutes, put `"RenderThread": 0` in the config or A/B it.

32. **Frozen-fixture dumps from different processes do not match** (2026-09-29, §8.49). The settle is counted
    in rendered frames and the game ticks on wall time, so the Market's dancers freeze at a phase that depends on
    the frame rate: any two runs differ in 3,000-6,500 pixels, all NPCs. Compare dumps only between segments of
    one process (an A/B, or a CVar against itself with `SOH_BENCH_REPS`); a mask of the NPCs is not enough, a new
    pose uncovers pixels outside it.

33. **A live `pyra-undervolt set` stalls the game, and the watch block does not show it** (2026-09-29, §8.51). A
    switch takes ~1.2 s (sudo, a governor change, ~0.2 s at the next clock up while the point is set); blocks
    overlapping one missed 3.2-3.7% of frames against 1-2.3% for the rest. The block samples the clock every few
    frames and usually misses the excursion, so `cpu_min_mhz`/`cpu_max_mhz` still read 750. Drop such blocks by
    time: `voltflip.sh` logs each switch's start and end, and `slowpairs.py watch.log volt ...` uses them.
34. **The keyboard backlight's idle timeout moves an idle measurement by 0.77 W** (2026-09-29, §8.52). With no
    input, the desktop takes the keyboard backlight from 12 to 0 some minutes in, and the floor drops from ~2.7 to
    ~1.9 W at once. A game run keeps it pulsing 50/12 instead (hardware repo power-budget README). Pairs within a
    run survive it if both arms see it; absolutes and anything compared across the step do not. thermcost's CSV
    logs `kbl` every second: check it for a step before reading any idle row. Relatedly, the charger can drop
    `online` to 0 on its own while USB networking stays up, so a thermcost run can start without the cable
    being pulled.

## 6. Recipes

**"Where does the main thread's time go?"** — `bench.sh ab gSettings.ProbeSplit 0 1 <fixture>`, then
`splitblocks.py run.log 1 1000` for the blocks and `pairs.py run.log 0 1000` for what the marks cost
(§8.43). For the whole machine and the other threads, run `~/git/dragonbox-pyra/device/pyra-cpu-attribution.sh
-d 120 -t soh.elf` on the device while the game runs: per process, per core, and SoH's threads by name.

**"What shape is the game's data?"** — run the fixture **natively on the host** (§8.30). The host
benchmark build (`cmake --build build-cmake --target soh`) runs the same fixtures, and its `BENCH`
counters matched the device's exactly. Put the device's `oot.o2r` (copy it; the host's other copies
are different versions) and a `shipofharkinian.json` with the device's gameplay CVars and an
800×480 window in a scratch directory, and run
`SOH_BENCH=1 $MARKET SOH_BENCH_EXIT=1 ... build-cmake/soh/soh.elf` from there. The interpolation
recording's call stream is written with
`SOH_BENCH_INTERP_TRACE=<file> SOH_BENCH_INTERP_TRACE_TICK=600`.

**"Render-thread CPU, paired, with t and MDE"** — `bench.sh` pairs only the window and the period.
`python3 docs/pyra/tools/pairs.py run.log <arm A value> [MHz]` pairs `thread_us`, `period_us`, the
window, the window's own CPU time (`win_cpu_us`) and `swap_us` from `run.log`, and drops any pair with
a segment off the pin (1250 MHz, or the clock given). It also pairs `eof_cpu_us` (nan in logs from
before §8.36).

**"A/B one CVar with another held fixed"** — the harness sets only the A/B CVar. Put the fixed one in
`$PYRA_DIR/shipofharkinian.json` for the run and put the file back afterwards (§8.34 ran
`ProbeTouchKB` on `LeanInterp=0` that way).

**"Is this change worth anything?"** — A/B it at Market *and* Hyrule Field, 5–6 pairs each, with
the soak. Two numbers, not one.

**"Did this change the picture?"** — `shotcheck.sh` at the busiest fixture you have, then read the
control number before the result number.

**"Is the GPU the limit?"** — take a PVRTune capture (§3) and read GPU busy against the frame
period. At the Market at the cap it is 20.5% (§8.42). The resolution A/B below is the indirect way,
from before PVRTune worked.

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
- **GPU-side:** PVRTune and apitrace work. See §3, "GPU: how busy is it", and report 08 §8.42.
  (This line used to say no GPU profiler worked here. Nobody had tried until 2026-09-26.)

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
| `~/git/dragonbox-pyra/experiments/gpu-tools/` | PVRTune and apitrace on the Pyra: build, background, the analysis script. |
| [09](09-benchmark-harness.md) | the harness: design, validation, every env var, the fixtures. |
| [08](08-optimisation-opportunities.md) | results, and §8.18 on why one fixture is not a measurement. |
| [10](10-save-file-corruption.md) | the save bug and `fdguard.c`. |
