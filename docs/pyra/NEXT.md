# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-24 (night)**, after §8.28: interleaving the lighting across vertices is
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

00. ~~**Real-play A/B of `gSettings.LeanGLState` 0 vs 3, then default it.**~~ **Done (§8.25):
    −2020 µs (−13.6%) of render-thread CPU over 25 real-play pairs, t −4.7. No GL errors and no
    visible glitches. Now the GLES2 default; desktop GL keeps 0 because a core profile rejects
    client arrays.** Still open:
    - Kokiri, and scenes with pre-rendered backgrounds.
    - The pause menu was exercised in play; the ImGui menu is unrecorded.
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
    - **Interpolation first:** keep `is_recording` false with `InterpolationFPS` untouched. A/Bing
      the FPS 30 vs 20 changes frames per tick, not just cost. In a frozen fixture interpolation is
      a no-op, so the skip should be pixel-exact.
    - Lookups by name: count per entry point, then a frozen-only memo on the name pointer.
    - malloc/free: count with a `perf probe` uprobe (untried here; unwinding does not work, counting should).
    - Interpreter rest: count commands per opcode first, re-rank after interpolation.

0. ~~**A/B the texture-by-hash lookup path.**~~ **Done (§8.22): mode 3 is −3787 µs (−12.1%) at
   the Market, t≈−123 on the three clean pairs; −3549 µs (−14.6%) at Hyrule Field at 1000 MHz.**
   Every `LoadResourceProcess` in a frozen frame was the interpreter's, and mode 3 takes them from
   1364 to 0. What is left of it:
   - ~~Real play before it becomes the default.~~ Done (§8.23): −11.7% render-thread CPU over 50
     real-play pairs, 0 flushes across nine scenes, no visual change. Default is now 3.
   - A session with a texture pack, reading `bench.sh watch blocks` for `lookup_flush`.
   - Hyrule Field 0 → 3 at 1250 MHz (both runs there were thermally capped to 1000).
   - The `Gfxs2dexBg*` path lookups (pre-rendered backgrounds) are not cached; check with a fixture
     that has one before assuming they matter.
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
4. **The ~3.4 ms of the swap that does not scale with pixels** (extrapolated, §8.19). That is the
   playbook's open "does Present flip or copy" question (`~/git/dragonbox-pyra/docs/improvements.md`
   §6) showing up as frame time.
5. **Is the frame pipelined with the GPU at all?** Never checked, in any report. On a tile-based
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
| **F** | **Present 640×360 and let the display controller scale** | unknown, ≤ the 3.4 ms of item 4 above | The game renders at 640×360 to `mGameFb` and then draws it as a 1280×720 quad. §8.7 calls that pass "inherent to the internal-resolution feature" — inherent to the *feature*, not to the hardware: the OMAP5 DSS can scale a plane in the display controller. It attacks the part of the swap that does not scale with pixels. Establish that a scaled plane is reachable through SDL/EGL on this stack **before** touching the renderer; if it is not, this is dead. |
| **G** | ~~**One `glBufferData` per frame with offsets**~~ | **superseded (§8.24)** | Client-side arrays — report 5's best row — took the per-draw `glBufferData` out entirely: −13.4% at the Market, several times what this row estimated. Report 5's "within noise, needs no change" was wrong for this game. The single-upload variant is no longer worth building unless mode 3 fails in real play. |
| **H** | **Count redundant batch splits on texture change** | unknown; a counter, not yet a change | `interpreter.cpp:1604` flushes *before* it knows whether the texture actually changes, so a display list re-issuing a load for an already-bound cache entry splits the batch for nothing. Every other flush site (`:1495-1692`) is properly guarded, which makes this the one candidate for a draw that need not exist. A `BENCH_` counter next to the existing ones answers it; only then is it a change. Gated on P2 for the same reason as G. |

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

### Not available, so do not plan around it

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

## 5 — Owed to other people

- [ ] Report the 32-bit ARM guard upstream (§1.2) — never buildable on any 32-bit ARM target.
- [ ] Report the colour-combiner gap upstream (§5.2) — latent on every platform.

## 6 — Not performance

- [ ] Phase 6: Pyra packaging.
