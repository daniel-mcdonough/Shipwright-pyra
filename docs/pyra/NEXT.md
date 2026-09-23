# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-24**, after re-measuring `ShaderOpts` uncapped and finding the CPU pin leaked
(§8.20).

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
6. **Read every segment's `cpu_mhz`, `cpu_min_mhz`, `cpu_max_mhz`.** `pyra-thermal.service` moves
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

### Do this first

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
3. **What the non-swap timed work is waiting on — at the Market.** It is **20.7 ms** there and
   13.7 ms at Hyrule Field (§8.20; this line used to say "~13 ms", which is the Field's figure).
   The draw counts are modest (261 / 167). Candidates: driver synchronisation per draw, the
   tiler, memory. A `perf` profile of the frozen Market fixture, uncapped and on the new pin, is
   the next probe — split interpreter from driver time before estimating anything. Two things to
   carry in: §8.19's "barely follows the CPU clock (+~2% for −20%)" was taken on the leaky pin and
   is **unverified**; and the Market's frame period creeps +4% across a five-minute run at a fixed
   clock, mostly in untimed work (§8.20) — the profile should cover the start and end of a run.
4. **The ~3.4 ms of the swap that does not scale with pixels** (extrapolated, §8.19). That is the
   playbook's open "does Present flip or copy" question (`~/git/dragonbox-pyra/docs/improvements.md`
   §6) showing up as frame time.

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

### Do not build

- **Item 7, VBO ring buffer.** Measured at 221 µs/draw against 68 for the code today (§5.3, §8.6).
  The comment at the call site in `gfx_opengl.cpp` says so; leave it alone.
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
      the playbook classes this as research rather than a change. It is the single largest
      available number on the list and the only one that is not a code change.

## 5 — Owed to other people

- [ ] Report the 32-bit ARM guard upstream (§1.2) — never buildable on any 32-bit ARM target.
- [ ] Report the colour-combiner gap upstream (§5.2) — latent on every platform.

## 6 — Not performance

- [ ] Phase 6: Pyra packaging.
