# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-27**, after §8.48's real play: **LeanCosmetics=1 is the default** (no effect on frames
over 40 ms in 13.5 min of play, t 0.27; nothing visible), and `ClearVariable` no longer bumps the CVar
generation for an absent variable (the game cleared one every tick). Final frozen: −277 µs Market, −260 µs Hyrule
Field. **30/750 still misses ~3.5% at the Market and at Death Mountain Trail** (1000 MHz: ~1.9%). 0S is done;
**the owner's call now**: the two-core design (0T (3)) or settle on 30/875 or 20/625.

Earlier the same day, §8.48's frozen half: **0S sized. One hook is 0.3 ms a tick.** Skipping the flat blocks
(`gSettings.ProbeSkipFlat`) frees ~0.88 ms at the Market at 1000 MHz: input ~0.2, post_draw ~0.32, ImGui ~0.36.
The post_draw part is `CosmeticsUpdateTick` (~450 CVar lookups by name a tick to learn that no rainbow is on);
the game's audio update is ~0.03. **`gSettings.LeanCosmetics=1`** (default 0 for now) caches the pointers per CVar
generation: exact (0 mismatches in 614k checks, control 1,040), pixel-identical, **−273 µs Market (t −6.8), −251
µs Hyrule Field (t −22.2)** of render-thread CPU. Owed: the real-play A/B at the Market, 30/750, then the default.
The other pieces are each 0.1-0.2 ms. Not enough to close the Market at 30/750 alone.

Before that, after §8.47: **the audio stack is ~5 ms of core time a frame at 750 MHz, and not a
frame-time lever.** The 32→48 kHz resampler is 0.44 ms of it. Pinning the main thread to either core costs
~3.2 ms of period at the frozen Market (all device IRQs are on cpu0): don't pin. Next for the Market at 30/750:
0S, the flat blocks.

Before that, after §8.46: **the present pass and the swap.** Drawing the game quad without
blending (`gSettings.LeanPresent=1`) is exact and −0.32 ms of GPU per frame: **now the GLES2 default.**
Skipping the clear under it costs +1.6 ms; nearest filtering nothing. **The swap waits for the frame's GPU
work** (item 5 answered: not pipelined, uncapped), so GPU time is frame time too, uncapped. Deferring the swap
(`gSettings.LeanSwap=1`) takes 1.2-1.5 ms off the frozen fixtures uncapped (t −12/−14) but **is worse in real
play** (>40 ms frames 0.97% → 1.54%, render CPU +5%, t 2.2): at the cap the frame-cap sleep already overlaps
the GPU. It stays off. And **real play at 30 fps and 750 MHz nearly holds**: 0.97% of frames over 40 ms
against 0.81% at 1000 MHz (§8.43), **except the Market (~3%)**. §8.44's fixture (3.9% lost) overstated play.
The owner's goal now: 30 fps at 750 MHz, which means the Market's heavy ticks (0A, 0S), judged in real play.

Before that, after §8.45: **the owner chose 20 fps at 625 MHz, and it holds real play**:
0.7% of frames lost over 13 minutes and eight scenes, against 0.6% for 30 fps at 1000 MHz (§8.43), for
−0.81 W (§8.44). Thin at the Market (0.7 min, 1.5%). Shipping it is launcher work, a TODO in
`~/git/pyra-gamemode/TODO.md` (per-game CPU profiles); a longer Market stretch at 625 if the owner wants it
settled. Next then was the present pass (item I); §8.46 did it, and the owner's switch to 30/750 made 0A/0S live.

Before that, after §8.44 (task 0T (1) and (2) done, no code): **875 MHz holds 30 fps
at the Market fixture and 750 does not** (3.9% of frames lost, although the render thread averages only
19.2 ms). Battery watts against 30 fps at 1000 MHz (4.43 W, 67.7 °C; desktop idle 2.62 W, 47 °C): 30/875
−0.17 W, 30/750 −0.34 W and −2.7 °C, **20/750 −0.70 W and −5.6 °C** (0.6% lost), 20/625 −0.81 W,
20/500 −0.91 W and −8.2 °C (2.4% lost). The GPU now runtime-suspends (the hardware repo's 0034 GPU-power module; loaded by hand for its soak then, installed as the m4-kbdled default on 09-26), and the idle floor
fell 1.1 W and ~9 °C. **Owner's call: 30 fps at 875 MHz, or 20 fps at 625-750 MHz.** Either way the next
step is a real-play check of the chosen arm in the Market, Kokiri Forest and Hyrule Field.

Before that, §8.43 (b) and (c), task 0 done: **real play holds 30 fps at 1000
MHz.** 22.9 minutes, the owner playing, twelve scenes, die 52 → 72 °C: 0.8% of frames over 40 ms (one
every ~4 s, spread out, none steadily in any scene), 0.17% over 67 ms. The main thread averages 12.1 ms
of CPU per 33.5 ms frame (Market 17.6, Hyrule Field 13.4, the temples ~9.8); the whole machine is 22 ms
of core time per frame, 33% of each core. A slow frame is ~2× the CPU of a normal one plus 7-19 ms of
swap plus ~20 ms off the CPU, not measured; the main thread is preempted ~555 times a second. **The
audio stack (the game's synthesis thread, its Pulse client threads and pulseaudio) is 3.7 ms per frame**,
more than Xorg and the WM together. The thermal track (0T) is next; the scenes to hold are the Market,
Kokiri Forest and Hyrule Field.

Before that, §8.43 (a): **the whole main thread is now
accounted for, block by block.** Frozen, uncapped, 1000 MHz, CPU per frame: the Market's 16.18 ms is
`Interpreter::Run` 12.58 (78%), `Play_Draw` with the interpolation recording 1.33,
`FrameInterpolation_Interpolate` 0.57, ImGui 0.63 (new frame 0.43, end 0.20), after-draw 0.34, input
0.22, eof 0.30 and ~0.3 of small blocks. Hyrule Field: 10.61 ms, `Run` 8.11 (76%), `Play_Draw` 0.76,
ImGui 0.59, after-draw 0.34, input 0.20. The blocks sum to `thread_us` within 2.4 µs. Input, after-draw
and ImGui are a flat ~1.2 ms per frame whatever the scene, and none of them is explained yet. Parts (b)
and (c) need the owner playing; the instrument for them is built.

Before that, a review of what the CPU accounting covered (no new measurements):
**the timed window is well priced, and the rest of the main thread is not.** Frozen Market at the cap,
1000 MHz: render thread 16.8 ms on-CPU per frame, of which ~13.5 ms is inside the timed window and priced
block by block (§8.26-8.41). The ~3 ms outside it (derived: `thread_us − window_us`; §8.20 measured the
block at 5.7-6.1 ms of wall time uncapped, on older defaults) has never been split, nor have the other
threads (~2.5 ms per tick, §8.33) or the rest of the machine while SoH runs. And §8.33 found the main
thread on-CPU only 28-45% of the time at the cap, so nobody has shown that real play ever misses 30 fps
for CPU reasons. The two-core design rests on that. **New task 0 below: complete the CPU picture, then
decide whether the two-core design is worth anything.**
**The owner's goal is thermals, not frame rate** (2026-09-26). Both A15 cores share one clock and one
rail (`smps123`: 1000 MHz 1.05 V, 750 MHz 0.95 V, 500 MHz 0.85 V), so CPU work pays off when it lets
the clock, and so the voltage, drop a step. 1250 → 1000 MHz measured −0.3 W and −2-3 °C with 30 fps
held (hardware repo `experiments/power-budget/`). **New task 0T: the thermal track**, starting with
the two experiments that need no code. The two-core design is now gated on 0T, not on missed frames.

Before that, §8.42: **the GPU can be profiled, and it is 20% busy at the cap.**
PVRTune's HWPerf timing and apitrace both work on the device (TESTING.md §3, "GPU: how busy is it";
hardware repo `experiments/gpu-tools/`). Frozen Market, 30 fps cap, 1000 MHz: GPU busy (TA or 3D)
**6.9 ms of the 33.4 ms period (20.5%)**; render thread on-CPU 16.8 ms. The frame is CPU-bound by
a wide margin. Each frame is two renders: the scene into the 640×360 framebuffer, **2.64 ms** of 3D,
and the present pass, **2.87 ms**, which is a 1280×720 clear plus one alpha-blended ImGui quad. Showing
the picture costs the GPU more than drawing it. That pass is the new item I below.
Before that, §8.41: **grouping by texture is built and measured.**
`gSettings.LeanGroup=1` (GLES2 backend, now the default) defers opaque, depth-written, non-decal draws into
buckets by program and textures and replays each run in one arena: Market draws 228 → 145, window CPU
**−840 µs (−5.8%, t −41.1, MDE 53)**; Hyrule Field 155 → 140, −143 µs (−1.6%, t −8.9, MDE 41). That is
~10 µs per removed draw against §8.40's conservative ~11: the draw prices add. Market pixel-identical;
Hyrule Field 26 pixels at ±1 level on alternate frames (depth ties on the horizon band and Link's boot).
Clean in real play (nothing visible; speed unresolved, MDE ~500 µs). **Now the GLES2 default** (owner's
call; the first default that is not exact). Frozen Market: 145 draws, window CPU ~13.5 ms at 1000 MHz.
Before that, §8.40: **grouping by texture was the next build.** At
the Market 159 of 228 draws sit in runs of consecutive opaque, depth-written, non-decal draws, and 85 of
those have the same texture set and program as an earlier draw of their run (`BENCH group`); sorting
would cut texture changes in runs from 148 to 62. Estimated 0.9-1.3 ms of the ~14.1 ms window CPU
(6-9%); Hyrule Field only 15 draws (~2%). The rest of a draw is priced: an attribute pointer changed
and back is ~2.4 µs per draw (`ProbeSkipDraw=10`, t 20.0; unchanged ones are free, mode 9), a program
switch and back ~5.8 µs (mode 11, t 45.3). Of a 17.1 µs state-changing draw ~14.2 µs is now accounted
for; one vertex layout for all programs would save ~0.1 ms, so it is not worth building.
Before that, §8.37-8.39: **what a draw costs, and why.** A Market draw that
changes state is ~17.1 µs of CPU at 1000 MHz: ~4.3 µs the call, ~2.2 µs its vertices, ~5.5 µs a texture
bind (validated at the draw even when the bind restores the same texture, §8.38), ~1.2 µs per uniform
upload (whatever the value, §8.39, but only 85 per frame). 225 of the Market's draws change a texture
(`BENCH batch`, §8.37). Item H's same-texture splits were 33 of 261 draws: `gSettings.LeanTexReuse=1`
removes them, −158 µs (−1.1%, t −18.2) window CPU, pixel-identical, clean in real play, now the default.
Before that, §8.36: **the draw calls are a quarter of the main thread.**
Skipping `glDrawArrays` alone (`gSettings.ProbeSkipDraw=1`) is −4125 µs of render-thread CPU at the
Market (−24.3%, t −340, MDE 31), ~15.8 µs per draw at 1000 MHz over 262 draws; the rest of the per-draw
GL state is ~260 µs and the flush plus swap ~285 µs of CPU (`eof_cpu_us`). So the render side's window
CPU is ~1/3 GL (nearly all of it the draw calls) and ~2/3 interpreter. Also §8.36: the interpolation
replacement map was being **copied** on return; `gSettings.LeanInterpMap=1` (now the default) moves it:
−145 µs (−0.9%, t −8.6) Market, −37 µs (t −2.9) Hyrule Field, 0 pixels, heap allocations 291 → 178 per
frame. Recycling its nodes too (mode 2, exact in motion, 67 per frame) bought nothing more measurable.
Before that, §8.35: **client arrays (`LeanGLState=3`) hold at Kokiri
in real play: −20.2% render-thread CPU (t −7.7, 14 pairs), −18.9% (t −9.0) inside five pre-rendered
interiors**, no visible change there or in the pause or ImGui menus, and no per-frame resource loads
from the backgrounds. No frozen fixture has a pre-rendered background (checked in `oot.o2r`).
Before that, §8.34: **§8.30's timed-window regression is gone in wall
time** (+74 µs, t 1.6, MDE 107, 8 pairs at 1000 MHz; it was +295). Two thirds of the old figure was the
render thread *waiting* inside the window, not working. What is left is +44 µs of window CPU (t 4.0,
MDE 26). A probe that adds ~690 KB of cache traffic per tick outside the window costs the window +91 µs
of CPU, so the L2 guess fits. Sizing the arena buffers cannot change the lines touched, and the
ceiling is 44 µs, so nothing was built. `LeanInterp=1` nets −1533 µs render-thread CPU at the Market.
Before that, §8.33: **logic is 15–20% of the main thread** (fixtures and 18 minutes of real play), so a
logic/render pipeline on core 1 buys ~10% uncapped and nothing at the 30 fps cap. The CPU time is in the
render side (7.7–13.2 ms per frame); a two-core split has to divide that, and how much of it is GL is
not measured.
Before that, §8.32: **§8.31's "LeanCVar changes the picture" was the
benchmark's camera override not holding.** The freeze does not stop `Camera_Update`, which wrote the
game's own look-at over the override each tick until it happened to stop — and that turned on
uninitialised reads in the camera code, so unrelated code changes flipped fixtures by 0.006° of yaw
(122k pixels). The override is now applied just before `View_Apply`. `SOH_BENCH_DUMP` replaces the
timed screen grabs with same-frame dumps plus vertex and matrix traces. **`gSettings.LeanCVar=1` is
exact and now the default: −257 µs (−1.5%, t −9.1) Market, −186 µs (−1.7%, t −6.4) Hyrule Field at
1000 MHz** (the Field contradicts §8.31's −526 µs). Heap allocations per frame at the Market: 970 → 291.
Before that, §8.31: two thirds of the Market's 3200 heap allocations per frame were lookups by name;
`LeanTexLoad=1` (−3 to −5%) and `LeanGameLookup=1` (−7.3% Market, −5.6% real play) became defaults.
Before that, §8.30: **the interpolation recording now lives
in a per-recording arena (`gSettings.LeanInterp=1`, the default)**: −1365 µs render-thread CPU at
the Market (−8.0%, t −110, MDE 32), −266 µs at Hyrule Field (−2.7%, t −13.1). It is exact: 0 of
139,324 replacement-map entries differ in motion on the device, and the positive control fires.
In real play (owner playing, 32 pairs, eleven scenes) it is −1068 µs, −8.1% (t −3.3, MDE 644).
A microbenchmark replaying the game's traced tick predicted 59–76% of the recording's cost would go;
64% did. Unexplained: the Market's timed window got +143 µs slower (t 3.2, MDE 116).
Before that, §8.29: **frame interpolation costs 2.7 ms of
render-thread CPU per frame at the Market (−15.9%, t −142) and 0.65 ms at Hyrule Field (−6.6%)**,
measured by a pixel-exact skip. That is 2.9× its sampled size. ~2.15 ms of it is building and
freeing the recording tree (~2.6 µs per recorded item, §8.30's correction), ~0.55 ms the interpolate walk. Before that, §8.28: interleaving the lighting across vertices is
exact (0 mismatches at three fixtures) and worth nothing (+43 µs, t 1.9); a microbenchmark shows the
A15 already overlaps the chains. Lighting alone measures −740 µs at the Market (t −27.6), about
twice what the isolated loop predicts, which is unexplained. After §8.27: the ceiling of lazy vertex lighting is
≈0.47 ms at the Market and ≈0.2 ms at Hyrule Field, under the gate, so it is not built. Lighting
itself measures ~0.8 ms at the Market — two directional lights per vertex at ~165 cycles, a latency
chain — and fog 0.16 ms. After §8.26: re-profiled with client arrays on. The driver's
CPU fell 5.0 → 3.3 ms per frame and the game side did not move. The interpreter's geometry is now
the top named item (3.3 ms), and **62% of the triangles it processes are discarded**.
`gSettings.LeanGLState=3` (§8.25) and `gSettings.LeanGeom=1` (§8.26, bit-exact, −1.1%) are defaults.
Before that, §8.25: **client-side vertex arrays (`gSettings.LeanGLState`) default to 3 in GLES2
builds**. The gains: −13.4% of the frozen
Market frame (27.5 → 23.8 ms, 36.3 → 42.0 FPS), −7.3% at Hyrule Field, and −13.6% of render-thread
CPU in real play (t −4.7), with nothing visibly wrong. §8.24 refuted "the driver is paid per GL
call": removing 43% of GL calls saved 0.5%. Before that, §8.22–8.23: the asset-lookup cache
(`gSettings.LeanResLookup`, **default 3**) is −12.1% of the Market frame and −15.1% at Hyrule Field,
and −11.7% of render-thread CPU in real play. §8.21: the frame is the render thread *computing*, not
waiting on the GPU.

## Ground rules that keep being learned the hard way

1. **Probe before estimating.** Three of the nine catalogued optimisations were closed by reading
   the C++ call site after having estimated from the shader or the GL call — items 1, 2 and 7.
   That is one method failure, not three items (§8.16). Before building anything below, spend the
   twenty minutes to confirm the thing you think is happening is happening.
2. **The rig resolves 0.3–0.9%** of the frame period uncapped, six pairs, CPU pinned (§8.20) —
   read each run's own `mde_us`. Effects smaller than about 1% need the pin and an ABBA design.
3. **Frame rate and game speed are the same number** (§7.5b). An FPS win above the
   `InterpolationFPS` cap buys slow motion, not smoothness.
4. **The fixture is a frozen frame.** It tells you a frame got cheaper to draw, not that the game
   got faster. Confirm in real play.
5. **One fixture does not measure a change, it samples one.** Run shader-side changes against
   **Market and Hyrule Field** at minimum. (The example that taught this — the same shader change
   "worth −3.59%, −2.40% and nothing" by scene, §8.18 — turned out to be the frame cap; uncapped it
   is −4.9% and −4.6%, §8.20. The rule stands; its best example did not.)
6. **A sampling profiler cannot see waiting.** A blocked thread takes no samples and a spinning
   driver looks like work, so `perf record` alone cannot tell "computing" from "waiting" — which
   is the open question about 65% of the frame (§2a). Establish on-CPU time first, then profile.
7. **Read every segment's `cpu_mhz`, `cpu_min_mhz`, `cpu_max_mhz`.** `pyra-thermal.service` moves
   the clock under the harness in both directions (§8.20, TESTING.md trap 12). A segment off the
   pin is discarded, and the report says so.

## 1 — Finish the save-corruption job (report 10)

The mechanism is settled and the save file is defended. The cause is not found.

- [ ] **Run `scripts/pyra/fdguard.c` on the device.** Needs a free Pyra and a save to happen. It
      names whoever closes the log's descriptor, with a backtrace. Until this runs, every other
      descriptor in the process is still exposed and we are guessing at the culprit.
- [ ] Apply the same verified write to `Config::Save()` — `libultraship/src/ship/config/Config.cpp`
      has the identical defect (no temp file, no check).
- [ ] Surface a failed save on screen. Right now it is loud in the log and silent to the player.
- [ ] `fsync` before `rename`. Removable-battery handheld; the rename can land before the data.

The host build works again as of report 10 §10.6 (`text.h` was missing `<functional>` under
GCC 16). It compiles and links, which is enough for the kind of cheap experiment §10.2 used.
~~no ROM or `oot.o2r` on the host~~ — there is one, and `tools/ccscan.py` has been reading it all
along; static questions about the game's data do not need the device. Launching on the host is
untested and not needed (section 3).

## 2 — Performance work, ranked

Of the nine catalogued items: **2 measured and won** (5, 6 — together −4.9% / −4.6% of the frame
uncapped, §8.20), **3 closed** (1, 2, 7), 1 implemented at no gain (4), 1 premise weakened (3). What is actually left:

All nine catalogued items were aimed at the fragment pass, which is 16% of the Market frame. The
other 65% was profiled for the first time in §8.21: it is CPU work — interpreter, resource lookups,
allocation, and the GL driver's CPU side — so the list is no longer ranked against a guess.

### Do this first

0. **Complete the CPU picture before designing the two-core split** (added 2026-09-26). Three parts,
   all measurement:
   - ~~**(a) Split the main thread's CPU outside the timed window**~~ **Done (§8.43).** Fifteen
     contiguous marks, `BENCH split` per segment, closure within 2.4 µs of `thread_us` at both
     fixtures. Outside the window: 2.65 ms Market, 1.60 ms Hyrule Field; the table is in §8.43. The
     marks cost +48 µs (t 1.9, MDE 66) Market and +65 µs (t 4.3, MDE 39) Hyrule Field. The original
     brief, for the record: split the main thread's CPU outside the timed window on the frozen Market and Hyrule
     Field. The window is `Benchmark_FrameBegin`/`FrameEnd` around `DrawAndRunGraphicsCommands` in
     `RunCommands` (`OTRGlobals.cpp`). Outside it, per tick: `Play_Draw` building the display list,
     `Graph_ProcessGfxCommands`' set-up and the interpolation recording and walk
     (`FrameInterpolation_*`), `HandleEvents`, the audio hand-off, and whatever else the loop runs.
     Extend `SOH_BENCH_SPLIT`'s marks (§8.33) with thread-CPU marks at each of those boundaries
     rather than adding a new instrument. The blocks have to sum to `thread_us − window_us` within
     the MDE. If they don't, something is missing: find it before going further. Price the extra
     marks with one A/B against the unmarked build, as P5 says.
   - ~~**(b) Whole-machine attribution in real play**~~ **Done (§8.43).** soh.elf ~59% of busy time
     (main thread 11.3 ms per frame, audio synthesis 1.2, Pulse client threads 0.6), pulseaudio 1.9 ms,
     Xorg 1.5, marco 0.9, PVR workers 0.6, IRQs ~1 ms; 22 ms of core time per frame, cores even. The
     script dropped any process whose ticks crossed a digit (unstable sort); fixed, hardware repo 92f809b.
     The original brief:
     ( the script now takes `-t soh.elf` for
     the game's threads and prints busy time per core): `~/git/dragonbox-pyra/device/pyra-cpu-attribution.sh`
     during a real-play session (the owner playing, watch mode, pinned), with SoH's threads named
     (`/proc/<pid>/task/*/comm` and `stat`): main, audio, and any SDL/driver threads, then Xorg,
     PulseAudio, the window manager, kernel threads and IRQs. On other workloads PulseAudio and Xorg
     were 12-14% of the machine each (`cpu-optimization.md` §3.1); for SoH this has never been taken.
   - ~~**(c) Does real play miss the 30 fps cap, and where?**~~ **Done (§8.43): not at 1000 MHz.** 0.81%
     of frames over 40 ms, spread across blocks; heavier blocks have more. Slow frames are CPU spike +
     swap + ~20 ms off-CPU, not one of them. Hold-list for 0T: Market, Kokiri Forest, Hyrule Field.
     `docs/pyra/tools/watchsplit.py watch.log` reads it all by scene. The original brief: ( in the same session as (b): watch
     blocks now carry `hist_ms`, `max_ms` and `slow`/`slow_cpu_pct`/`slow_swap_ms`, and
     `SOH_BENCH_SPLIT=150` adds the §8.43 blocks per 5 s window.) Frame-period histogram (`LUS_FPS_LOG` or
     the watch-mode blocks) over a session that reaches thermal equilibrium, logging the clock and die
     temperature next to each block. Name the scenes and clocks where frames go over 33.3 ms, and say
     for each whether the main thread was on-CPU (CPU-bound) or waiting (swap/GPU).
   **Decision this feeds:** if (c) finds no CPU-bound misses at equilibrium, the two-core design buys
   nothing a player sees. CPU work then only buys heat and power, and the ranking should follow the
   (a)/(b) numbers by CPU-ms saved per frame on *either* core, not main-thread latency. If (c) does
   find them, the two-core design goes ahead, sized against the scenes (c) named.
   *Superseded as the gate by 0T (the owner's goal is heat, not frames); (c) is still needed, as
   the list of scenes each clock has to hold.*

**Since §8.46 the goal is 30 fps at 750 MHz**, and real play there fails only at the Market (~3% of frames
over 40 ms against ~1.9% at 1000 MHz; the other scenes match 1000 MHz). So 0A and 0S are live, and each is
judged the same way: an A/B in **real play at the Market**, pinned at 750 MHz, reading frames over 40 ms
and paired render CPU (`watchpairs.py`), not the frozen fixtures' mean. §8.44's fixture overstated play.

0A. **The audio stack: sized (§8.47), not a frame-time lever.** ~5.0 ms of core time per frame at 750 MHz
   (pulseaudio 2.5, synthesis thread 2.0, Pulse client threads 0.5), mostly on the other core. The sink is
   48 kHz and the game 32 kHz, so pulseaudio resamples with `speex-float-1`: **0.44 ms per frame** (t ≈ 9.5),
   not changed because the method is system-wide and music would lose quality. What is left is heat, not
   frames: pulseaudio wakes ~126 + ~70 times a second (SDL's `SampleLength` sets the client's period,
   untested), and SDL's ALSA backend straight to the AESS is untried.

0P. **Why slow frames spend ~20 ms off the CPU** (§8.43 (c)). ~~Pinning is the cheap test.~~ **Done (§8.47):
   pinning the main thread alone to either core costs ~3.2 ms of period and +1.6 ms of its CPU per frame
   at the frozen Market, 30/750** (both directions; all device IRQs are on cpu0). Don't pin. The main
   thread is preempted ~760/s at 750 MHz. If slow frames matter again, `perf sched record -a` over a
   real-play window names who ran while it was runnable.

0S. **Done (§8.48).** Input ~0.2 ms, post_draw ~0.32 (`CosmeticsUpdateTick`, fixed by `gSettings.LeanCosmetics`,
   the default since its real-play check), ImGui ~0.36 in pieces of 0.07-0.15 (menus/windows 0.10, the SDL backend's NewFrame 0.14,
   the rest 0.08). The SDL NewFrame reads `_NET_WORKAREA` from X every frame (`UpdateMonitors`): a candidate for
   `gui_start`'s off-CPU time, unmeasured. The original brief:
   **Size the flat per-frame blocks by skipping them** (added 2026-09-26, §8.43). **Next, for 30 fps at
   750 MHz at the Market** (0A and 0P are closed, §8.47). Three blocks, ~1.2 ms of main-thread CPU per
   Market frame at 1000 MHz (7% of the thread), the same in both fixtures, none of them explained. §8.43's
   table (frozen, uncapped, 1000 MHz, CPU µs per frame, Market / Hyrule Field):
   - **`input` 218 / 199** (~0.3 ms per tick): `Graph_StartFrame()` then `PadMgr_ThreadEntry(&gPadMgr)`,
     between the `SPLIT_TICK_BEGIN` and `SPLIT_INPUT_DONE` marks in `soh/src/code/graph.c` (~line 489).
   - **`post_draw` 335 / 335**: from `SPLIT_DRAW_DONE` (after `Play_Draw`, `soh/src/code/z_play.c:1745`)
     to `SPLIT_RENDER_BEGIN` (the top of `Graph_ProcessGfxCommands`, `soh/soh/OTRGlobals.cpp:1810`): the
     rest of `Play_Main` (`func_800C4344`, the game-interactor hooks), then `GameState_Update`'s and
     `Graph_Update`'s tails. The marks' meanings are listed in `soh/soh/Enhancements/benchmark.h:44-70`.
   - **ImGui ~0.6 ms: `gui_start` 426 / 404, `gui_end` 200 / 181.** In
     `Fast3dWindow::DrawAndRunGraphicsCommands` (`libultraship/src/fast/Fast3dWindow.cpp:205`):
     `gui_start` is `IsFrameReady`, `GetMouseStateManager()->StartFrame()` and `Gui::StartDraw()` =
     `StartFrame` (ImGui new frame) + `DrawMenu` + `CalculateGameViewport` (`Gui.cpp:800`); `gui_end` is
     `Gui::EndDraw()` = `DrawGame` (the present quad: needed for a picture) + `EndFrame` +
     `DrawFloatingWindows` + `CheckSaveCvars` (`Gui.cpp:809`). `gui_start` is only 64-70% on-CPU (~200 µs
     a frame waiting, unexplained: the frame-ready check and the SDL/X calls are the candidates).

   **Method** (the §8.26-8.29 skip-probe pattern): a benchmark-only CVar, e.g. `gSettings.ProbeSkipFlat`
   with one bit per candidate, read under `#ifdef SOH_BENCHMARK` like `gSettings.ProbeSkipInterp`
   (`soh/soh/frame_interpolation.cpp:770`) or `ProbeSkipLight` (`libultraship/src/fast/interpreter.cpp:1387`).
   Each skip must leave the frozen frame drawable (wrong is fine in a probe). Split a block into its
   calls when it is not obviously one thing, e.g. `DrawMenu` and `DrawFloatingWindows` separately from
   ImGui's new-frame/render. In a frozen fixture skipping `PadMgr_ThreadEntry` should be harmless; check
   the picture with `SOH_BENCH_DUMP` anyway. Don't size anything from the split marks: they cost ~4-5 µs
   each and are inside the blocks (§8.43), and `ProbeSplit` must be OFF for any timed A/B.
   - Gate 1, frozen: `bench.sh ab gSettings.ProbeSkipFlat 0 <bit>` at the Market and Hyrule Field, 6
     pairs, uncapped, 1000 MHz pin (`PYRA_PIN_FREQ=1000000`, `SOH_BENCH_SOAK_C=66`, TESTING trap 21),
     `pairs.py run.log 0 1000`; read `thread_us` (these blocks are mostly outside the timed window; the
     ImGui ones are inside it, so read `win_cpu_us` too). Report t and MDE.
   - Only a block worth ~0.3 ms or more earns a real fix. A fix must be exact (or the owner's call), then
     the §8.22-8.24 ladder, and finally real play at the Market, 30 fps, 750 MHz (PROMPT task 1): frames
     over 40 ms per arm and paired `thread_cpu_us` from `watchpairs.py blocks.txt 0 750`, the owner playing
     longer at the Market than §8.46's 1,450 frames.
   - Budget reality: the Market at 30/750 misses ~3% against ~1.9% at 1000; §8.44 puts the gap at ~5 ms off
     the worst ticks. 0S's whole ~1.2 ms (×4/3 at 750, ~1.6 ms) will not close that by itself. If 0S comes up
     short, the next lever is the two-core design (0T (3)), or the owner settles on 30/875 or 20/625.

0T. **The thermal track: the lowest clock that holds the game** (added 2026-09-26). Judge every arm by
   **battery watts and die °C** at equilibrium, with fps as a pass/fail floor. Method: the hardware
   repo's `experiments/power-budget/thermcost.sh` (on battery, ABBA blocks of 60 s, first 15 s dropped,
   pinned with `userspace` + `scaling_setspeed` because `pyra-thermal` overrides min==max pins).
   Extend it for the arms below rather than writing a new script. The frame-rate floor is
   `SOH_BENCH_SPLIT` windows or `LUS_FPS_LOG`, read in the scenes 0(c) names, not only the Market.
   **(1) and (2) done (§8.44)**; see the header. What is left of 0T:
   - ~~**(4) Real-play check of the owner's choice**~~ **Done (§8.45): 20/625 holds.** The brief was: (30/875 or 20/625-750): watch mode pinned at that clock
     (`PYRA_WATCH_PIN=1 PYRA_PIN_FREQ=<kHz>`, plus `SOH_BENCH_AB_CVAR=gSettings.InterpolationFPS
     SOH_BENCH_AB_VALUES=20,20` for 20 fps), `SOH_BENCH_SPLIT=150`, the owner playing through the Market,
     Kokiri Forest and Hyrule Field; `watchsplit.py watch.log <MHz>` against §8.43's 1000 MHz table (0.81%
     of frames over 40 ms). Then decide how to ship the clock: a per-game pin in the launcher
     (`pyra-gamemode`) is the obvious place, not the game.
   - **(5) If 30 fps at 750 MHz is wanted**: ~5 ms off the Market's worst ticks (p95 40 ms per tick at 750
     against 35 at 875). 0A, 0S, then the two-core design, in that order.
   - **(6) Undervolt the chosen point, 625 MHz (20 fps) or 750 MHz (30 fps), whichever the owner settles on** (added 2026-09-26; hardware repo
     `experiments/undervolt/`). Each clock step's voltage is a fixed device-tree value (625 MHz = 0.900 V,
     50 mV per 125 MHz step), not calibrated for this chip. The rail follows the table (read back
     from the PMIC). `pyra_undervolt.ko` changes one point's voltage at runtime and puts it back on unload
     or reboot. It is built, and one 10 mV step reached the PMIC and reverted. Next: step 625 MHz down from
     0.900 V in 10 mV steps. Each step has to pass `stress-ng --verify` on both cores, cold and hot, and
     `SOH_BENCH_DUMP` frozen-Market frames pixel-identical to a 0.900 V reference. Back off 20-30 mV from the
     first failure, then `thermcost` ABBA at 20/625, stock against undervolted. Expected 0.05-0.15 W
     (~2% of CPU switching power per 10 mV), not measured. It also makes any later
     clock drop worth more. Too little voltage gives silent wrong results, then a crash:
     it cannot damage the chip, but it can corrupt the card, so the tested backup of 09-26
     (hardware repo `docs/card-backup-restore.md`) comes first. Needs the device free for a few hours.
   The original brief of (1)-(3), for the record:
   - **(1) Pin 750 MHz, no code.** Estimate: Market render thread ~22 ms per frame (16.8 ms × 4/3,
     less for memory stalls that don't scale with the clock), plus ~2 ms of logic in play and the
     swap wait: ~28-30 ms against 33.3. Probably holds at the Market, marginal in heavier scenes.
     Against 1000 MHz: watts, °C, and the scenes where it drops frames. If the kernel carries the
     intermediate steps (`scaling_available_frequencies` lists 875000), try 875 MHz as well.
   - **(2) `InterpolationFPS` 20 at 750 and 500 MHz, no code.** One rendered frame per tick instead
     of 1.5: about a third less render work on CPU and GPU, and a 50 ms budget. 500 MHz probably holds
     single-threaded. The trade is a 20 fps picture, and that is **the owner's call**: bring the
     numbers, don't pick. Compare watts and °C only. The frame period is not comparable between
     20 and 30 (TESTING.md: it changes how many frames each tick emits).
   - **(3) The two-core design, only if (1) and (2) fall short** of the clock the owner wants. The
     split must come with the clock drop that pays for it (a pin, or `ondemand`, which follows the
     busier core of the shared policy). At a fixed clock the hand-off adds CPU work and heat. Size
     it against the lowest clock that failed, in the scenes where it failed, using 0(a)/(b)'s table.
   **Limit to keep in view:** the idle floor sets most of the die temperature (56-60 °C at desktop
   idle; the game adds ~1.5 W and 11-15 °C). CPU work moves play temperatures by degrees, not tens.
   The GPU runtime-suspends at idle since the hardware repo's 0034 (installed as the default on 09-26).

00. ~~**Real-play A/B of `gSettings.LeanGLState` 0 vs 3, then default it.**~~ **Done (§8.25):
    −2020 µs (−13.6%) of render-thread CPU over 25 real-play pairs, t −4.7. No GL errors and no
    visible glitches. Now the GLES2 default; desktop GL keeps 0 because a core profile rejects
    client arrays.** Still open:
    - ~~Kokiri, and scenes with pre-rendered backgrounds.~~ **Done (§8.35): −20.2% at Kokiri in real
      play (t −7.7), −18.9% (t −9.0) in five pre-rendered interiors, nothing visible.**
    - ~~The pause menu was exercised in play; the ImGui menu is unrecorded.~~ Both exercised in §8.35.
    - At Hyrule Field a quarter of the CPU saving came back as swap time. If the GPU is now the
      next limit there, the "is the frame pipelined" question (item 5 below) is live again.
    - Modes 1-2 alone are ~0.1 ms. Kept because mode 3's pointer handling is built on the shadow.
    - ~~**Next profile** with mode 3 on.~~ **Done (§8.26).**

000. ~~**Price lazy vertices before building them.**~~ **Done (§8.27): not worth building.**
    Skipping lighting, texgen and fog outright is −972 µs (−5.8%) render-thread CPU at the Market
    (t −37) and −352 µs (−3.6%) at Hyrule Field (t −25); fog alone is −164 µs at the Market. Times
    the unused fraction, lazy lighting's ceiling is ≈470 / ≈200 µs, before its own overhead.
    Lazy fog (≈95 µs) is easy and exact, but not worth a new invariant.

0000. ~~**Interleave the vertex lighting across vertices, exactly.**~~ **Done (§8.28): exact, and
    no gain.** +43 µs at the Market (t 1.9, MDE 57); reverted. `tools/lightbench.cpp` shows the
    shipped loop at 53 ns/vertex, batching −5 ns, branchless +30 ns. Lighting alone is −740 µs in
    the game (t −27.6), 2× the isolated loop: the gap is unexplained.
    - Exact and small, if ever wanted: cache the light-direction recompute (239 per frame at the
      Market, ≈ 90 µs ceiling).

00000. **Size the remaining blocks by skipping them, before choosing one** (§8.28). All of these
    are sampled numbers, and §8.26–8.28 found sampling off by 2–3× in both directions: rest of the
    interpreter 2.2 ms (`Run` dispatch, tri2/tile handlers, texture-hash handler), game-side lookups
    by name 1.2 ms, interpolation 0.9 ms, malloc/free 0.8 ms. For each, find a skip that leaves the
    frame drawable (even if wrong) and A/B it, as `ProbeSkipLight` did. For a question about how the
    core runs a loop, a microbenchmark on the device first.
    - ~~**Interpolation first.**~~ **Done (§8.29): −2700 µs Market (t −142, MDE 49), −647 µs
      Hyrule Field (t −34, MDE 48), pixel-exact.** `gSettings.ProbeSkipInterp=2` split it:
      recording ≈ 2.15 ms, the walk ≈ 0.55 ms, the interpreter's lookups below the MDE.
      ~~**Next for it: microbenchmark an arena-backed recording store.**~~ **Done and shipped
      (§8.30): `LeanInterp=1`, −1365 µs Market, −266 µs Hyrule Field, exact in motion.**
      What is left of interpolation is ~1.3 ms at the Market: the record-side map lookups and
      `Data` copies (~0.8 ms) and the walk (~0.55 ms). Taking the rest means keying ops by
      position, which changes the matching rule. Size the other blocks first.
    - ~~Lookups by name: count per entry point, then a frozen-only memo on the name pointer.~~
      **Done (§8.31): ~112 per frame, ~9.5 µs each; the memo skip is −1052 µs at the Market.
      `gSettings.LeanGameLookup=1` is the exact form, −1160 µs (−7.3%) Market, −428 µs Hyrule Field.**
      **Real play done: −528 µs (−5.6%, t −3.5, 46 pairs, nine scenes), clean log. Default 1.**
    - ~~malloc/free~~ **Counted (§8.31)** with `scripts/pyra/allocshim.c` (the m4 kernel has no
      uprobes): 3202 per frame at the Market, 1776 at Hyrule Field; CVar keys 711, lookups ~1430,
      texture paths 441 (gone with `LeanTexLoad`, now the default). What is left after the two
      lookup changes is ~1550, 711 of them CVar keys. **With `LeanCVar` (§8.32): 291 per frame at
      the Market**, ~225 of them frame interpolation's replacement map.
    - ~~**The interpolation replacement map**~~ **Done (§8.36).** Ceiling (`ProbeSkipInterp=3`, no map)
      −370 µs (−2.1%, t −18.1) at the Market. Half the allocations were a copy on return;
      `LeanInterpMap=1` (default) moves it: −145 µs Market, −37 µs Hyrule Field. Mode 2 recycles the
      nodes (no map allocations left, exact in motion) and is no better. The ~210 µs left of the ceiling
      is inserts and the interpreter's lookups; a flat table would change libultraship's interface.
      Not worth it before the draw work below.
    - ~~**CVar reads without the key string**~~ **Done (§8.32): exact, default 1.** −257 µs (−1.5%)
      Market, −186 µs (−1.7%) Hyrule Field at 1000 MHz. §8.31's picture change was the camera
      override (TESTING.md trap 19), not the CVar code.
    - ~~**§8.30's window regression**~~ **Closed (§8.34).** Re-measured with the camera fix in:
      +74 µs wall (t 1.6, MDE 107), of which +44 µs is window CPU (t 4.0, MDE 26). `ProbeTouchKB=230`
      on the original recorder costs the window +91 µs of CPU, so the L2 guess fits the 44 µs. Buffer
      sizing and dropping the unused recorder cannot change the lines touched; recycling the arena's
      abandoned vector storage could, with a 44 µs ceiling. Not built.
    - **Earlier visual checks after §8.32.** Same-binary, in-process comparisons from before the
      camera fix still stand where they read identical: a flipped camera shows as a difference, it
      cannot hide one. Comparisons *across builds* (e.g. "within launch noise of the unmodified
      build") are not trustworthy; redo one only if something rests on it.
    - ~~**Split the render side**~~ **Done (§8.36).** At the Market the window's ~14.3 ms of CPU is
      ~4.1 ms `glDrawArrays`, ~0.26 ms other per-draw GL, ~0.29 ms flush and swap, ~9.6 ms interpreter.
      A GL thread would take ~4.7 ms off the main thread (~27%) before its hand-off cost; §8.33's logic
      split takes 2.5–4 ms per tick. **Still no design**: first find out whether the draw cost can
      simply be removed (§8.37: it cannot, below).
    - ~~**Price one draw call**~~ **Done (§8.37):** a Market draw at 1000 MHz is ~4.3 µs of call,
      ~2.2 µs of vertices and ~9.3 µs of validation after a state change (`ProbeSkipDraw` 3: +1685 µs
      window CPU, t 97.5; 4: +1120 µs, t 59.8; Hyrule Field 3: +771 µs, t 56.5). Both levers exist and
      neither is the whole of it. *Corrected in §8.38:* the 15.8 µs average included 33 no-change draws;
      a state-changing draw is ~17.1 µs, so its validation is ~10.6 µs.
    - ~~**Count avoidable draws**~~ **Done (§8.37), `BENCH batch` line.** 225 of the Market's 261 draws
      change a texture; 33 (13%) were split only by item H's same-texture reload, ~13 of 167 at Hyrule
      Field. No batch fills the buffer; matrices never split one.
    - ~~**`gSettings.LeanTexReuse=1` (item H): real-play A/B, then default it.**~~ **Done: clean in real
      play (16 pairs, seven scenes, nothing seen), default 1; the Market reads 228 draws.** −158 µs (−1.1%, t −18.2,
      MDE 22) window CPU at the Market, 261 → 228 draws; Hyrule Field −24 µs (t −1.3, MDE 49), 167 → 155;
      0 pixels at both in-process. Real play (watch mode, the pause menu, framebuffer effects and ImGui
      covered): +85 µs, t 0.17, MDE 1054 — a correctness check, not a measurement; the log was clean.
    - ~~**Does a texture bind alone cost the ~9 µs of validation?**~~ **Done (§8.38): ~5.5 µs, and a bind
      of the texture straight back costs the same (5.3 µs, t 56.0) as a real change (5.8 µs, t 63.9).** A
      state-changing Market draw is ~17.1 µs: call 4.3, vertices 2.2, texture bind 5.5, the rest ~5.
    - ~~**Price a uniform upload the same way**~~ **Done (§8.39): ~1.2 µs at the next draw, same value or
      not (t 11.5 / 9.9).** 85 per frame at the Market, ~0.1 ms: not the missing ~5 µs, of which ~4.5 is left
      (~3 after §8.40's pointer and program prices).
    - ~~**Price an attribute-pointer re-specification**~~ **Done (§8.40):** unchanged pointers are free
      (mode 9, +40 µs, t 0.5, MDE 220); pointed away and back ~2.4 µs per draw (mode 10, +561 µs, t 20.0,
      MDE 72). The game changes them only with its 51 program switches: ~0.1 ms. One vertex layout for
      all programs is not worth building. Also priced: a program switch and back, ~5.8 µs per draw (mode
      11, +1335 µs, t 45.3, MDE 76), ~0.3 ms per frame. ~14.2 of a 17.1 µs draw is accounted for.
    - ~~**Could opaque draws be grouped by texture?**~~ **Counted (§8.40), `BENCH group` line: yes, at the
      Market.** 159 of 228 draws are in 11 runs of opaque, depth-written, non-decal draws; 85 share
      texture set and program with an earlier draw of the run; texture changes in runs 148 → 62 if
      sorted. Hyrule Field: 48 draws in runs, 15 could merge. Estimate 0.9-1.3 ms Market, ~0.2 ms Field.
    - ~~**Build the grouping: `gSettings.LeanGroup`, default 0, in the GLES2 backend.**~~ **Built and
      measured (§8.41).** Market 228 → 145 draws, window CPU −840 µs (−5.8%, t −41.1, MDE 53); Hyrule
      Field 155 → 140, −143 µs (−1.6%, t −8.9, MDE 41); ~10 µs per removed draw, so §8.37-8.40's prices
      add. Market pixel-identical in-process; Hyrule Field 26 pixels at ±1 level from depth ties; clean
      in real play with the owner (pause menu, ImGui, pre-rendered rooms), speed unresolved there.
    - ~~**Decide `LeanGroup`'s default.**~~ **Default 1 on GLES2** (owner's call). Checked: with the key
      absent, a soaked Market `run` draws 145 and reads 13.51 ms window CPU at 1000 MHz.
    - **Maybe: sort a run's buckets by program too.** Grouping kept 48 of 51 program switches (~5.8 µs
      each, §8.40); ordering buckets by program within a run would take some at the price of more
      reordering. Count first: how many switches a program-major order would leave.
    - **Then the two-core design, gated on task 0T (3): only if it is needed to reach a lower clock.** §8.36's ~4.7 ms of GL per Market frame, less item H's ~0.16 ms,
      less grouping's ~0.84 ms (§8.41): ~3.7 ms.
    - Loose ends, not tasks: the camera code's uninitialised reads are the original game's and
      real in play too (any build can move them); the host build ignored `SOH_BENCH_FULLSCREEN=0`
      on 2026-09-24 and took over the desktop at 1920×1080 — check before the next host run.
    - ~~Interpreter rest: count commands per opcode first.~~ **Counted and timed (§8.31):** 10,927
      commands per frame; geometry dominates; no non-geometry handler reaches 1 ms
      (`G_SETTIMG_OTR_HASH` ~0.64 ms, `G_LOADBLOCK` ~0.47 ms before `LeanTexLoad`). Not skip-probed.

0. ~~**A/B the texture-by-hash lookup path.**~~ **Done (§8.22): mode 3 is −3787 µs (−12.1%) at
   the Market, t≈−123 on the three clean pairs; −3549 µs (−14.6%) at Hyrule Field at 1000 MHz.**
   Every `LoadResourceProcess` in a frozen frame was the interpreter's, and mode 3 takes them from
   1364 to 0. What is left of it:
   - ~~Real play before it becomes the default.~~ Done (§8.23): −11.7% render-thread CPU over 50
     real-play pairs, 0 flushes across nine scenes, no visual change. Default is now 3.
   - A session with a texture pack, reading `bench.sh watch blocks` for `lookup_flush`.
   - Hyrule Field 0 → 3 at 1250 MHz (both runs there were thermally capped to 1000).
   - ~~The `Gfxs2dexBg*` path lookups (pre-rendered backgrounds) are not cached; check with a fixture
     that has one before assuming they matter.~~ In real play in five pre-rendered rooms (§8.35) there are
     no misses or resource loads after the room's first block. No frozen fixture has one.
   - **Next profile:** re-take §8.21's flat profile with mode 3 on. The frame's composition has
     changed by 12%, and the ranking below it was made against the old one.

~~**Find out what Hyrule Field's 30 ms is actually spent on.**~~ **Closed 2026-09-23 (§8.19): it
was the frame cap.** The harness never uncapped — the game resets the target FPS every tick — so
Hyrule Field read "33.3 ms minus the untimed work" whatever changed. Uncapped it runs **21.1 ms
(47 FPS)**; the Market, at **31.5 ms (31.7 FPS)**, is the scene near the 30 FPS line. Internal
resolution 0.5→0.25 buys only 6.3% / 4.9%, so neither is fill-bound at 640×360.

In order:

1. ~~**Log the effective CPU frequency per segment.**~~ **Done 2026-09-24 (§8.20)**, and it found
   the pin leaking: `pyra-thermal.service` rewrote the ceiling every 2 s. The pin is now the
   userspace governor.
2. ~~**Re-measure `ShaderOpts` 0→3 uncapped.**~~ **Done (§8.20): −4.85% Market, −4.55% Hyrule
   Field**, both t ≤ −13. See Item A below.
3. ~~**What the non-swap timed work is waiting on.**~~ **Answered 2026-09-24 (§8.21): mostly
   nothing — it is ≈ 90% on-CPU.** Render thread 75–78% on-CPU over the whole period across 17
   segments; the ~7 ms off-CPU is roughly the 5 ms swap plus ~2 ms. IPC 0.52; ~27 context
   switches and ~75 DDK bridge ioctls per frame. Kept below for the record:
   **What the non-swap timed work is waiting on — at the Market.** It is **20.7 ms** there and
   13.7 ms at Hyrule Field (§8.20; this line used to say "~13 ms", which is the Field's figure).
   The draw counts are modest (261 / 167). Candidates: driver synchronisation per draw, the
   tiler, memory. Two things to carry in: §8.19's "barely follows the CPU clock (+~2% for −20%)"
   was taken on the leaky pin and is **unverified**; and the Market's frame period creeps +4%
   across a five-minute run at a fixed clock, mostly in untimed work (§8.20) — any profile should
   cover the start and end of a run. **The probe ladder is section 2a below**; it is the largest
   unattributed block in the project and everything else on this page is ranked against a guess
   until it is opened.
4. **The ~3.4 ms of the swap that does not scale with pixels** (extrapolated, §8.19). **Most likely
   found (§8.42): the present pass, 2.87 ms of 3D for one blended 1280×720 quad. See item I.** That is the
   playbook's open "does Present flip or copy" question (`~/git/dragonbox-pyra/docs/improvements.md`
   §6) showing up as frame time.
5. ~~**Is the frame pipelined with the GPU at all?**~~ **Answered (§8.46): no, uncapped.** The swap
   returns when the frame's GPU work is done: blend-off's −0.32 ms of GPU came out of the swap
   (−317 µs), and the uncapped swap (5.1-5.4 ms) is about the frame's 3D plus tiling. Deferring the swap
   to the next frame's start (`gSettings.LeanSwap=1`) takes 1.2-1.5 ms off the frozen fixtures uncapped,
   but at the cap the frame-cap sleep (before `SDL_GL_SwapWindow` in `SwapBuffersBegin`) already
   overlaps the GPU, and in real play LeanSwap was worse (>40 ms 0.97% → 1.54%). Off; a probe. Not
   established: whether the wait is WSEGL's or X's. The original entry:
   **Is the frame pipelined with the GPU at all?** Never checked, in any report. **Now answerable:**
   a PVRTune timeline puts the TA/3D kicks against the frame, and §8.42 shows the scene's TA kicks spread
   through the CPU's frame, with 3D starting at the flush. How the swap waits on it is still unread. On a tile-based
   deferred renderer the CPU should be a frame ahead; if `SwapBuffersBegin` blocks until the tile
   pass retires, the 5 ms swap is serialised dead time rather than overlapped work, and the EGL
   buffer count and `EGL_SWAP_BEHAVIOR` decide which. Cheap to establish (buffer count, and
   whether swap time tracks the *previous* frame's GPU work), and it is the one hypothesis on
   this page that would make a large number available without changing what is rendered. The
   comment at `interpreter.cpp:4505` already reads swap time as "waiting for the GPU to catch
   up" — that is an assumption nobody has tested.
6. **The 6.1 ms outside the timed window at the Market** (§8.20) — 19% of the frame, on a fixture
   that *skips* `Play_Update`, so it is not game simulation. It also grows 5.67 → 6.55 ms across
   five minutes at a fixed clock while the swap stays flat. In real play that block is strictly
   larger, and report 7's real-play absolutes have never been re-taken on a trustworthy rig, so
   the gap between the frozen 31.4 FPS and what the game actually does is itself unmeasured.
   `period_us − mean_us` is already logged per segment; what is missing is any breakdown of it.
   **Now task 0 (a) above.**

### Worth doing

| | Item | Expected | Why it is here |
|---|---|---|---|
| **B** | **Item 8 — program binary cache** (§8.6) | hitching, not FPS | `GL_OES_get_program_binary` and `GL_IMG_program_binary` are both present; 317 shader variants game-wide. The playbook says "take it, for feel". **The one item a probe cannot gate**, because its benefit is not steady-state frame time — the fixture will report zero and be right. Judge it by shader-compile stalls on scene entry, not by the rig. |
| **C** | **Item 9 — NEON the three scalar audio routines** (§8.6, §8.11) | heat, not FPS | §8.11 undersells this. The part is fanless and walks 1.5 → 0.75 GHz under sustained load; CPU work removed is thermal headroom returned, which shows up as frame time much later in a session than any benchmark run measures. |

### Worth a prototype, not a conversion

| | Item | Expected | Why the caution |
|---|---|---|---|
| **D** | **Item 3 — sub-rect texture upload** (§8.4, §8.15) | ~1.5% ceiling | Upload a tile's sub-rectangle as its own texture so `tex_width2 == tex_width`, moving the remaining shader clamps to hardware. Capped by the non-mirror share of the 3.6% §8.15 found, so the ceiling is low and the work is not. **Prototype exactly one texture format** and measure before converting anything. |
| **E** | **`GL_EXT_shader_framebuffer_fetch`** | unknown | Present, unexplored, and nearly free on a tile-based deferred renderer — `gpu-optimization.md` calls it the most interesting unexplored extension on the device. No Shipwright use case has been identified yet, which is the first thing to establish. Do not build machinery for it before finding one. |
| **F** | **Present 640×360 and let the display controller scale** | unknown, ≤ the 3.4 ms of item 4 above | The game renders at 640×360 to `mGameFb` and then draws it as a 1280×720 quad. §8.7 calls that pass "inherent to the internal-resolution feature" — inherent to the *feature*, not to the hardware: the OMAP5 DSS can scale a plane in the display controller. It attacks the part of the swap that does not scale with pixels. Establish that a scaled plane is reachable through SDL/EGL on this stack **before** touching the renderer; if it is not, this is dead. **§8.42 sized the pass it would remove: 2.87 ms of 3D per frame, more than the scene.** |
| **G** | ~~**One `glBufferData` per frame with offsets**~~ | **superseded (§8.24)** | Client-side arrays — report 5's best row — took the per-draw `glBufferData` out entirely: −13.4% at the Market, several times what this row estimated. Report 5's "within noise, needs no change" was wrong for this game. The single-upload variant is no longer worth building unless mode 3 fails in real play. |
| **H** | ~~**Count redundant batch splits on texture change**~~ **Counted and built (§8.37): 33 of 261 Market draws; `LeanTexReuse=1` −158 µs (−1.1%) window CPU, pixel-identical; clean in real play, default 1** | measured | `interpreter.cpp:1604` flushes *before* it knows whether the texture actually changes, so a display list re-issuing a load for an already-bound cache entry splits the batch for nothing. Every other flush site (`:1495-1692`) is properly guarded, which makes this the one candidate for a draw that need not exist. A `BENCH_` counter next to the existing ones answers it; only then is it a change. Gated on P2 for the same reason as G. |
| **I** | **The present pass (§8.42)** — **done (§8.46)** | was 2.87 ms of 3D per frame | Blend off for the game quad (`gSettings.LeanPresent=1`): −0.32 ms of 3D, exact (0 stable pixels on screen), **the GLES2 default**. No colour clear (bit 2): **+1.6 ms**, the tiler reloads every tile. `GL_NEAREST` (bit 4): nothing. What is left (~1.85-2.45 ms) is the 1280×720 pass itself; only item F removes it. Correction: it is frame time too, uncapped, because the swap waits for it (item 5). |

### Do not build

- **Item 7, VBO ring buffer.** Measured at 221 µs/draw against 68 for the code today (§5.3, §8.6).
  The comment at the call site in `gfx_opengl.cpp` says so; leave it alone. This is
  `glBufferSubData` into a persistent buffer, and it is **not** item G above — the same table that
  refutes this one measured that one as the best of the four.
- **Item 1, filtering into the shader key.** Already obtained by the `FILTER_LINEAR` default —
  `o_three_point_filtering` is a compile-time substitution, so the branch is not in the shipped
  shader at all (§8.16). The branch-flattening A/B that both documents wanted is moot with it.
- **Item 4, `GL_EXT_discard_framebuffer`.** Implemented, measured at no gain (§8.10). Decide
  keep-or-revert on commit `e1fae43d`; it is dead weight either way, not a regression.

### Item A is measured, confirmed and shipped

**Re-measured uncapped 2026-09-24 (§8.20)**, 6 ABBA pairs each, CPU at 1250 MHz in every segment,
`gSettings.ShaderOpts` 0 → 3:

| fixture | Δ frame period | t | MDE | Δ timed window | fps 0 → 3 |
|---|---|---|---|---|---|
| Market Day | **−1622 µs (−4.85%)** | −40.1 | 104 µs | −1553 µs (−5.69%) | 29.90 → 31.42 |
| Hyrule Field | **−1025 µs (−4.55%)** | −13.3 | 198 µs | −1033 µs (−5.25%) | 44.45 → 46.58 |
| Kokiri Forest | not re-measured | | | | |

About 87% of the saving is inside the swap — the fragment pass on this tiler.

~~Capped (§8.17, §8.18): Market −3.59%, Kokiri −2.40%, Hyrule Field −0.07% "a clean null".~~ The
Market figure was about a quarter low and the Hyrule Field null was the cap entirely (§8.20). The
change has never measured negative.

Visually identical at every fixture checked, against an in-session control each time (§9.11):
Kokiri 9,973 against a control of 10,004; Hyrule Field 10,361 against 10,216; Market as recorded
in §8.18. Confirmed in real play by the device's owner across Kokiri, Hyrule Field and the Market
— no artefacts, including at the fire, water and dark-interior cases where a mis-analysed wrap
would show first.

One caveat survives: **the shader is an asset.** Shipping this means shipping a matching
`soh.o2r`; a binary that meets an old archive logs `soh.o2r is older than this binary` and falls
back. The benchmark install's `soh.o2r` is a real file, deliberately, and the real install at
`~/soh-pyra` is still on the old archive.

## 2a — Opening the 65%: the probe ladder

The frame at the Market, `ShaderOpts 3`, uncapped, split from §8.20's per-arm table:

| block | time | share | what is known about it |
|---|---|---|---|
| non-swap timed window | **20.69 ms** | **65%** | nothing. Never profiled. |
| swap | 5.05 ms | 16% | contains the tiler's fragment pass — 87% of the `ShaderOpts` saving landed here (§8.20) |
| outside the timed window | 6.08 ms | 19% | nothing, on a fixture that *skips* `Play_Update` |

Every code item in report 8 was aimed at the 16%. That is why the catalogue looks exhausted: it
is exhausted *for that slice*. The two large slices have been sized three times and profiled
zero times, and "there is nothing big left" is not a claim the evidence supports about a budget
two thirds of which nobody has opened.

One derived number, not a finding: non-swap work ÷ draws is **79.3 µs** at the Market
(20.69 ms / 261) and **81.9 µs** at Hyrule Field (13.67 / 167) — two scenes 56% apart in draw
count agreeing to 3%. Two points can be a coincidence; it is also the exact shape a large
per-submission constant would have. P2 tests it directly and cheaply, so nothing should be built
on it first.

**The first question is not "where" but "doing what".** If that block is the thread *blocked*,
a sampling profiler will show almost nothing, because a sleeping thread takes no samples and a
spinning driver looks like work (`profiling-guide.md` §2.3 makes the same point about the 8.5%
PowerVR share in report 7). P1 and P2 settle computing-versus-waiting before any profile is worth
recording.

### P1 — On-CPU time per segment *(code, ~10 lines, permanent)* — **done, §8.21: running, ~77%**

Read `CLOCK_THREAD_CPUTIME_ID` for the render thread at each segment's first and last measured
frame and print it next to `cpu_mhz=`, exactly as the temperature and clock fields already are
(`benchmark.cpp`, the §8.20 change). Then every future run carries the diagnostic for free.

Read it as: on-CPU ≈ `period_us` means the thread is **running**, and `perf stat -e cycles,instructions`
then splits compute from memory stalls by IPC — with the interpreter back in play, since §8.20
retracted the clock bracket that had ruled it out. On-CPU well below `period_us` means the rest is
**blocked**, and P2/P3 find where. It must be per *thread*: audio is on core 1
(`OTRGlobals.cpp:1113`) and a whole-process number folds it in.

### P2 — ioctls per frame *(no code, one run)* — **done, §8.21: ~75/frame, all the PVR bridge**

**The recipe below is wrong about the device node**: there is no `/dev/pvrsrvkm`; the DDK uses
`/dev/dri/renderD130`. `strace` is not installed. What works:
`sudo perf record -e raw_syscalls:sys_enter --filter 'id == 54' -p $PID -- sleep 10`, then
`perf script`, and read `args[0]` (fd) and `args[1]` (request) from each line.

The SGX DDK submits through ioctls on `/dev/pvrsrvkm`. Count them against `draws=` from the same
run:

```sh
perf trace -e ioctl -p $(pidof soh.elf)          # or: strace -c -f -e trace=ioctl -p ...
```

~261 per frame at the Market is one per draw and the submission-shape work below becomes live.
Two or three per frame means the driver batches, the 80 µs/draw ratio above is a coincidence, and
that whole line of enquiry dies for the price of one run.

### P3 — Where it blocks *(no code, if P1 says "blocked")*

```sh
perf list | grep sched:                      # confirm the tracepoints exist first
perf sched record -p $(pidof soh.elf) -- sleep 20 && perf sched timehist
```

Off-CPU time with the stack that went to sleep, which names the call site rather than the
symptom. `ftrace` is the fallback the playbook already uses for scheduler questions.

### P4 — On-CPU profile of the frozen Market *(no code, if P1 says "running")*

```sh
perf record -F 499 -g -p $(pidof soh.elf) -- sleep 30 && perf report --sort dso,sym
```

Sort by DSO before symbols — `soh` against `libGLESv2` against the kernel is the split that
decides the ranking. ARM32 call graphs need `-fno-omit-frame-pointer` in the profiling build (or
`--call-graph dwarf` if this `perf` has libunwind). Report 7 §7.6's profile does not substitute:
it was real gameplay, whole process, pre-DSP audio, three-point filtering on, capped.

### P5 — In-frame accumulators *(code, mirrors an existing pattern)*

`gBenchSwapMicros` already does exactly this around `SwapBuffersBegin`
(`interpreter.cpp:4516-4524`, drained per segment). The same treatment around `glBufferData` and
`glDrawArrays` separately (`gfx_opengl.cpp:974-975`), `LoadShader`/`SetPerDrawUniforms`
(`:141-198`) and `SelectTexture` (`:827`) prices each part of a draw directly.

Cost: ~522 `clock_gettime` calls per frame at the Market, roughly 25–50 µs, or 0.1–0.2% against
an `mde_us` of 0.31%. Close enough to matter — gate it behind an env var as `SOH_BENCH_FINISH`
already is, and price the instrumentation itself with one A/B of instrumented against not before
trusting any split it produces.

### P6 — The subtractive ladder *(code, crude, immune to the computing/waiting question)*

Skip `glDrawArrays`; then `glBufferData` as well; then all GL inside `Flush()`; then the
traversal. Each arm's wall-clock delta bounds one layer from above. The driver may short-circuit
when nothing is drawn, so watch `swap_us` to catch the arms where GPU work changed rather than
CPU work.

### ~~Not available, so do not plan around it~~ Available since 2026-09-26

**Withdrawn:** PVRTune's HWPerf timing and apitrace both work on the device
(`~/git/dragonbox-pyra/experiments/gpu-tools/`). Frozen Market, 30 fps cap: GPU busy 20.5% (6.9 ms
per frame); the present pass, one blended 1280×720 ImGui quad, is 2.87 ms of 3D against the scene's
2.64 ms. Item F and item 4 above are that pass. The old text, kept for the record:

There is **no GPU-side tracing on this part** — no PVRTune, no frame debugger, nothing beyond
`GL_EXT_occlusion_query_boolean` and A/B (`~/git/dragonbox-pyra/docs/profiling-guide.md` §3.2,
already established). `ls /proc/pvr /sys/kernel/debug/pvr` costs ten seconds if someone wants to
be certain, but "attach a GPU profiler" is not a step any plan here can contain.

**Status 2026-09-24:** P1, P2 and a flat P4 done (§8.21). P3 as written is broken — `perf sched
record -p` loses the idle transitions; use `-a` if it is ever needed. User-space call graphs do not
work on this stack at all (§8.21), so P5 and P6 are how call sites get priced from here.

**Order: P1 and P2 in one session.** Neither needs a theory, together they split the hypothesis
space, and P1 leaves the harness permanently better instrumented. P3 or P4 follows from which way
P1 went; P5 only once P2 says draws are worth pricing.

## 3 — Measurement debt

- [ ] *(low priority)* **A visual check on the build host.** Not a gap in what can be tested —
      the Pyra launches the game routinely, and the fixture already puts the player at a fixed
      spot, so the visual check for a shader change has a home. This would only make it cheaper by
      not costing device time. `oot.o2r` is on the build host after all
      (`~/Games/ComboShip-orig/`, `~/Downloads/SoH-Ackbar-Delta-Linux/`), so the "no ROM on the
      host" line several documents repeat is wrong about the archive — but a full host link and a
      virtual display are still missing, and llvmpipe would answer "did the colours change" and
      nothing else. Worth an hour only if device time becomes the bottleneck.
- [ ] **Re-baseline report 7's absolute FPS figures** on the report 9 harness, **with
      `SOH_BENCH_SOAK_C`**. *Partly discharged:* §8.17 is the first absolute taken at a recorded
      temperature — **29,282 µs / 34.15 FPS** at the §9.9 fixture, unmodified shader, every segment
      between 80.6 and 83.6 °C. Report 7's own gameplay figures are still un-retaken, and this one
      is a frozen frame, so it does not replace them. The fixture and the CPU pinning landed; the absolutes themselves were
      never re-taken, so every number in report 7 is still stale and still quoted. Absolutes are
      exactly the measurement the soak exists for: until this rig started recording temperature
      (§9.10) no absolute figure in any of these documents came with the die temperature it was
      taken at, on a part whose frame rate moves ~30% across the range one run covers. That
      includes the 33.9 FPS the `InterpolationFPS` decision below now rests on.
- [ ] Add the **Hyrule Field** and **Market/Kakariko** fixtures. Easy now via `SOH_BENCH_WATCH`.
      One indoor fixture is not a workload.

## 4 — Decisions waiting on the device's owner

- [x] **Default `gSettings.LeanResLookup` to 3** — done 2026-09-24 after the owner's real-play
      session (§8.23). Original entry, for the record: (§8.22). −12.1% of the Market frame, −14.6% at
      Hyrule Field, identical picture in-process. Needs, before it is a default: a real-play session
      with `gSettings.LeanResLookup=3` across scene loads, the pause and equipment menus, and the
      alt-asset toggle — watching for a wrong or stale texture, which is what a broken invalidation
      would look like. The known risk is a texture pack that streams assets during play (upstream
      #989's failure case); on the stock assets it cannot arise. Ship no archive change with it.

- [x] **Default `gSettings.ShaderOpts` to 3.** Done. Uncapped: −4.85% of the frame at the Market,
      −4.55% at Hyrule Field (§8.20); visually identical at all three fixtures and confirmed in
      real play (§8.18). Ship a matching `soh.o2r` with any binary that carries it.
- [ ] **`InterpolationFPS`**: the speed/smoothness trade (§7.5, §7.5b). 20 gives correct speed and
      a choppier picture; 30 gave 0.92× slow motion at report 7's 27.7 FPS. **That number is stale
      and the trade may have dissolved** — the fixture now runs 33.9 FPS on the shipped
      `FILTER_LINEAR` default (§8.14), which is above 30 and would make `InterpolationFPS = 30`
      correct speed *and* smoother. Do the re-baseline in section 3 before putting this to anyone;
      it may already be a free choice rather than a trade. **Now measured at a known
      temperature: 34.15 FPS at 80.6–83.6 °C** (§8.17, unmodified shader), and 34.99 with item A
      on. So the margin over the 30 cap survives equilibrium at this fixture, and the trade does
      look dissolved — but this is a frozen frame with `Play_Update` skipped, which is precisely
      the measurement that flatters a speed-cap question. Confirm in real play before deciding.
      **Retracted 2026-09-23 (§8.19): those FPS figures are 1/timed-window, capped, and omit the
      untimed work.** The real frame period uncapped is 31.5 ms (31.7 FPS) at the Market and
      21.1 ms (47 FPS) at Hyrule Field, frozen. The Market margin over 30 is ~5%, in a frame with
      `Play_Update` skipped — so the trade is probably **not** dissolved in busy scenes. Measure
      in real play with `LUS_FPS_LOG` before putting it to anyone.
- [ ] **The staged `GfxSpVertex` hoist patch** — apply or drop. **Warning: it was staged in a
      session scratchpad and is not in the repo.** Nothing named `hoist.py` survives here, so this
      is "rewrite or forget", not "apply or drop". Treat it as forgotten unless someone has it.
- [ ] **Raising the GPU OPP.** The SGX544MP2 runs at 425 MHz on the NOM rail; a higher OPP exists
      and is worth roughly 25% of GPU throughput. Voltage readback is unavailable on this board, so
      the playbook classes this as research rather than a change. It is the only item on the list
      that is not a code change.
      **No longer "the single largest available number", 2026-09-24.** That ranking dates from when
      the workload was believed fragment-bound. The GPU time anyone can currently point at is the
      5.05 ms swap (§8.20), and 25% of it is ~1.26 ms — about 4% of the Market frame. It is the
      largest number again only if the 20.7 ms non-swap block turns out to be GPU-side waiting,
      which is exactly what §2a's P1 and P2 decide. Voltage research on an unreadable rail before
      that probe is the wrong order.
      **§8.21 decided it: that block is ≈ 90% on-CPU, not GPU waiting.** This item is worth at
      most ~4% of the Market frame and sits behind every CPU-side item.
      **§8.42 measured the GPU directly: 20% busy at the cap.** A faster GPU buys nothing at 30 fps.

## 5 — Owed to other people

- [ ] Report the 32-bit ARM guard upstream (§1.2) — never buildable on any 32-bit ARM target.
- [ ] Report the colour-combiner gap upstream (§5.2) — latent on every platform.

## 6 — Not performance

- [ ] Phase 6: Pyra packaging.
