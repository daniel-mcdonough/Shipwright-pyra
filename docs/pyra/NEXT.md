# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-23**, after finding the harness had been capped at 30 FPS all along (§8.19).

## Ground rules that keep being learned the hard way

1. **Probe before estimating.** Three of the nine catalogued optimisations were closed by reading
   the C++ call site after having estimated from the shader or the GL call — items 1, 2 and 7.
   That is one method failure, not three items (§8.16). Before building anything below, spend the
   twenty minutes to confirm the thing you think is happening is happening.
2. **The rig resolves 0.57%** at the current fixture with the CPU pinned (§9.5). Effects smaller
   than about 1% need the pin and an ABBA design, and anything claimed below 0.57% is noise.
3. **Frame rate and game speed are the same number** (§7.5b). An FPS win above the
   `InterpolationFPS` cap buys slow motion, not smoothness.
4. **The fixture is a frozen frame.** It tells you a frame got cheaper to draw, not that the game
   got faster. Confirm in real play.
5. **One fixture does not measure a change, it samples one.** The same shader optimisation is
   worth −3.59% in the Market, −2.40% in Kokiri and nothing at all in Hyrule Field (§8.18). Scene
   choice moved the answer by a factor of seven; the rig's 0.5% resolution never came close to
   mattering that much. Run shader-side changes against **Market and Hyrule Field** at minimum.

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

Of the nine catalogued items: **2 measured and won** (5, 6 — §8.17, 2.4% together), **3 closed**
(1, 2, 7), 1 implemented at no gain (4), 1 premise weakened (3). What is actually left:

### Do this first

~~**Find out what Hyrule Field's 30 ms is actually spent on.**~~ **Closed 2026-09-23 (§8.19): it
was the frame cap.** The harness never uncapped — the game resets the target FPS every tick — so
Hyrule Field read "33.3 ms minus the untimed work" whatever changed. Uncapped it runs **21.1 ms
(47 FPS)**; the Market, at **31.5 ms (31.7 FPS)**, is the scene near the 30 FPS line. Internal
resolution 0.5→0.25 buys only 6.3% / 4.9%, so neither is fill-bound at 640×360.

In order:

1. **Log the effective CPU frequency per segment** (`scaling_cur_freq`, start and end, like
   `temp_c`). One uncapped run came out 15% slow at the same pin with nothing in any log to say
   why. Until this exists, a CPU-clock result is not trustworthy. Ten lines in `benchmark.cpp`.
2. **Re-measure `ShaderOpts` 0→3 uncapped at Market and Hyrule Field.** It is shipped on numbers
   taken under the cap (§8.18). Direction is not in doubt; size is, and every ranking below leans
   on it.
3. **What the ~13 ms of non-swap timed work is waiting on.** It barely follows the CPU clock
   (+~2% for −20%, tentative) and the draw counts are modest (167 / 261). Candidates: driver
   synchronisation per draw, the tiler, memory. A `perf` profile of the frozen Market fixture,
   uncapped, is the next probe — split interpreter from driver time before estimating anything.
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

### Item A is measured, confirmed and ready to ship

**Measured 2026-09-22, confirmed across three scenes and in real play 2026-09-23** (§8.17, §8.18).
Same harness, CPU pinned, thermal soak, `gSettings.ShaderOpts` 0 → 3:

| fixture | mean Δ | t | pairs | effect |
|---|---|---|---|---|
| Market Day | **−1167.9 µs** | −23.74 | 6 | **−3.59%** (30.71 → 31.90 FPS) |
| Kokiri Forest | **−704.2 µs** | −11.04 | 5 | **−2.40%** (34.15 → 34.99 FPS) |
| Hyrule Field | −20.5 µs | −0.35 | 8 | −0.07%, a clean null resolving 0.46% |

> **All three rows were measured capped at 30 FPS (§8.19).** The Hyrule Field null is the cap
> absorbing the change; the other two are real in direction, not reliable in size. Re-measure
> uncapped (item 2 above) before quoting any of them.

~~The null is not a failure of the optimisation: Hyrule Field elides the wrap in 25 of its 26 shader
variants, and is simply not fragment-bound — three-point filtering is free there too (§8.18).~~ The
change is positive in two scenes and neutral in the third, and has never measured negative.

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

- [x] **Default `gSettings.ShaderOpts` to 3.** Done. −3.59% in the Market, −2.40% in Kokiri,
      neutral in Hyrule Field, visually identical at all three and confirmed in real play
      (§8.18). Ship a matching `soh.o2r` with any binary that carries it.
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
