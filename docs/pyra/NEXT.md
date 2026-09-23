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

### Item A is measured and done — 2.4%, pending one decision

**Measured 2026-09-22** (§8.17), five ABBA pairs per arm at the §9.9 fixture, CPU pinned, at
thermal equilibrium via the new soak:

| arm | | mean Δ | t (df=4) | effect |
|---|---|---|---|---|
| `1` | item 5, elide the no-op `mod()` | **−696.4 µs** | −12.45 | **−2.37%** |
| `2` | item 6, `int` → `vec2` size uniforms | −158.8 µs | −4.06 | −0.54% |
| `3` | both | **−704.2 µs** | −11.04 | **−2.40%** |

34.15 → 34.99 FPS at the fixture. Visually identical to the shipped shader — `shotcheck.sh`
against an in-session control (§9.11). The two halves are not additive: the wrap elision is
essentially the whole win.

**The decision below is yours: `gSettings.ShaderOpts` still defaults to 0, so none of this is on.**
Recommend defaulting it to **3**. Both halves are positive and neither changes the picture; item 6
earns little per fragment but removes a `glUniform` per draw, and keeping both on means one code
path gets exercised rather than two. Nothing else in the catalogue that is not a fidelity trade
comes close to 2.4%.

Two caveats before it ships as a default:

- **This is a frozen frame at one fixture.** Ground rule 4 applies: confirm in real play, and on a
  second scene, before treating 2.4% as the game's number. The wrap elision's share depends on
  which combiners a scene reaches for, and `wrapscan.py`'s 95.2% is a static command count, not a
  fragment-weighted one.
- **The shader is an asset.** Shipping this means shipping a matching `soh.o2r`; a binary that
  meets an old archive logs `soh.o2r is older than this binary` and falls back. The benchmark
  install's `soh.o2r` is no longer a symlink into `~/soh-pyra` — it is a real file, deliberately,
  and the real install is still on the old archive.

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

- [ ] **Default `gSettings.ShaderOpts` to 3.** Measured at −2.40% and visually identical
      (§8.17); it is off by default, so the win is not being had. Recommended, with the two
      caveats in section 2 — confirm in real play first, and ship a matching `soh.o2r`.
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
