# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-22**, after measuring item A on the device.

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

**Find out what Hyrule Field's 30 ms is actually spent on.** Not a catalogue item — a gap the
catalogue does not know it has. The open field is where the game is slowest and where players
spend the most time, and §8.18 established that fragment work there is free: three-point filtering
costs 0.11%, and eliding the combiner wrap in 25 of 26 shader variants costs nothing measurable.
That leaves geometry through the tiler, per-draw and display-list cost on the CPU, or bandwidth —
and **every remaining shader-side item below is aimed at a bottleneck that is not present in that
scene.** Ranking them without knowing this is ranking them blind.

The first probe is cheap and needs no code: an A/B on internal resolution at the Hyrule Field
fixture. If frame time barely moves when the pixel count halves, the cost is per-triangle or
per-draw and the whole fragment queue is misdirected for open scenes; if it scales, the cost is
per-pixel but not in the combiner, which points at bandwidth or overdraw instead. Either answer
reorders the table below.

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

The null is not a failure of the optimisation: Hyrule Field elides the wrap in 25 of its 26 shader
variants, and is simply not fragment-bound — three-point filtering is free there too (§8.18). The
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
