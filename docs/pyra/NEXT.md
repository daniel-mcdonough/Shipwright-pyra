# What to do next

The numbered reports are a record of what happened. This file is the opposite: a live queue,
rewritten as things land. If it disagrees with a report, this file is newer.

Last revised **2026-09-22**, after staging item A for measurement.

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

Of the nine catalogued items: 1 measured small, **3 closed** (1, 2, 7), 1 implemented at no gain
(4), 1 premise weakened (3). What is actually left:

### Worth doing

| | Item | Expected | Why it is here |
|---|---|---|---|
| **A** | **Items 5 + 6 as one combined A/B** — eliding the combiner's no-op `mod()`, and `int` → `vec2` size uniforms (§8.5) | small each, now resolvable | **Written and staged; needs the device.** See "Item A is staged" below. |
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

### Item A is staged

Both halves are implemented behind `CVAR_SHADER_OPTS`, a bitmask so the pair can be split
afterwards without a second CVar: bit 0 `SHADER_OPT_ELIDE_NOOP_WRAP`, bit 1
`SHADER_OPT_FLOAT_TEX_SIZE`. Default 0, which is the shader upstream generates. The harness knows
to drop the shader cache when this is the A/B CVar, the same as the filtering mode.

**To run it** (device must be free; the CPU pin and ABBA are the harness's):

```sh
./scripts/pyra/build.sh build -j"$(nproc)"          # done
cmake --build build-cmake --target GenerateSohOtr   # done - the shader lives in soh.o2r, not the binary

# soh-bench2/soh.o2r is a symlink into soh-pyra. Writing through it would replace the real
# install's assets with a benchmark build's. Break the link first, deliberately.
ssh pyra@pyra.local 'rm -f soh-bench2/soh.o2r'
scp soh.o2r pyra@pyra.local:soh-bench2/soh.o2r

export PYRA_HOST=pyra@pyra.local PYRA_DIR=/home/pyra/soh-bench2
./scripts/pyra/bench.sh deploy
./scripts/pyra/bench.sh ab gSettings.ShaderOpts 0 3 \
    SOH_BENCH_ENTRANCE=0x00EE SOH_BENCH_POS=-592.8,0.0,-195.9 SOH_BENCH_YAW=0x369E \
    SOH_BENCH_EYE=-707.3,55.3,-222.8 SOH_BENCH_AT=-592.8,34.7,-195.9 SOH_BENCH_SETTLE=400
```

`reps` defaults to 5, which is the ten segments every other run in these documents used. Then
`0 1` and `0 2` if the pair clears 0.57%, to find out which half earned it.

**The visual check runs on the same harness**, before the benchmark, with three variables changed.
`SOH_BENCH_EXIT=0` leaves the window up instead of closing it; the short segment lengths flip
between arms every few seconds instead of every twenty-five; and because the scene is frozen and
the camera pinned, *the two arms should be the same picture*. Watch the screen across a few
segment boundaries:

```sh
# EXIT=0 means the harness never closes the window, so the run ends at PYRA_TIMEOUT instead.
# Four short pairs is about eighty seconds of flipping; the rest of the timeout is looking time.
PYRA_TIMEOUT=240 ./scripts/pyra/bench.sh ab gSettings.ShaderOpts 0 3 \
    SOH_BENCH_ENTRANCE=0x00EE SOH_BENCH_POS=-592.8,0.0,-195.9 SOH_BENCH_YAW=0x369E \
    SOH_BENCH_EYE=-707.3,55.3,-222.8 SOH_BENCH_AT=-592.8,34.7,-195.9 SOH_BENCH_SETTLE=400 \
    SOH_BENCH_FRAMES=150 SOH_BENCH_SEG_WARMUP=60 SOH_BENCH_REPS=4 SOH_BENCH_EXIT=0
```

A correct elision is invisible: `mod()` then `clamp()` and `clamp()` alone agree to within a
float rounding on values that were already in range. A wrong one is not subtle — a channel that
should have wrapped to near-zero saturates to full instead, so the failure is a patch of the
scene changing colour in step with the arm, not a shade nobody can call. If nothing flickers,
run the benchmark above.

This is an eyeball test and it is deliberately not automated. A `glReadPixels` capture and a
pixel diff would be stricter and would make it repeatable, but there is no capture path in the
harness today and the swap happens inside `Fast3dWindow::DrawAndRunGraphicsCommands` — adding one
means a hook before the swap, not a call from `Benchmark_FrameEnd`, where the back buffer is
already undefined on a tile-based renderer. Worth building if shader changes become routine.

**The shader is an asset, not part of the binary.** `default.shader.fs` is copied out of
libultraship into `soh/assets/custom/shaders/` and packed into `soh.o2r` at build time, so
deploying `soh.elf` alone leaves the device running the old template. A binary that finds the old
template logs `soh.o2r is older than this binary` and falls back to the int uniforms rather than
rendering with unset texture sizes — but the wrap elision would then silently measure nothing, so
check for that line before believing a null.

**What was learned writing it, before any measurement:**

- §8.5's premise was wrong. The wrap is not a no-op in 1-cycle mode — one cycle computes
  `(A-B)*C+D`, which reaches `[-1,2]` — it is a no-op when the combiner *reduced* to `D`, `A*C` or
  `mix(B,A,C)`, which is a different and cycle-count-independent test. Guarding on `o_2cyc` as the
  report asked would have changed what the screen shows. The corrected analysis is in §8.5 and in
  `CombinerCycleInRangeFor`.
- That test is worth more than the report's version: it also covers the second wrap, between the
  two cycles, which §8.5 never mentions.
- **Almost every combiner in the game qualifies.** This was written down as "not knowable from
  the host", which was wrong twice over: `oot.o2r` is on this machine, and `tools/ccscan.py`
  already decodes the commands that answer it. `tools/wrapscan.py` reproduces `GenerateCC` and
  the range test over the archive:

  | assumption | unique variants | `G_SETCOMBINE` commands |
  |---|---|---|
  | 1-cycle, alpha on | 46/162 — 28.4% | 11305/11878 — **95.2%** |
  | 1-cycle, alpha off | 93/162 — 57.4% | 11544/11878 — 97.2% |
  | 2-cycle, alpha on | 103/224 — 46.0% | 10914/11878 — 91.9% |
  | 2-cycle, alpha off | 150/224 — 67.0% | 11510/11878 — 96.9% |

  Alpha-on is the conservative row. The two columns disagree because the combiners the game
  *reaches for* are the simple ones: under a third of the distinct shaders qualify, but they
  account for nineteen commands in twenty. The tool agrees with report 5 §5.6 exactly on the two
  figures that report already published — 11,878 commands, 317 unique combine modes — which is
  the check that it is decoding the same thing.

  **This is a static count, not a fragment-weighted one.** A combiner set once for a full-screen
  quad counts the same as one set for four pixels of HUD, and the display list does not say
  which. It bounds how *broadly* the elision applies, not what it is worth. Each compiled variant
  still logs its verdict when bit 0 is on, which is the check that the runtime agrees with this.
- Item 6 also removes a `glUniform` call per draw, so a win there is not attributable to the
  per-fragment conversion alone — and the PowerVR compiler may hoist that conversion into the
  secondary attribute program anyway, making it zero. See §8.5.

Checked without the device by `tools/offline-shader-checks.sh`: four template expansions rendered
through the real prism processor, and a twelve-case table run against the range functions
extracted verbatim from `gfx_opengl.cpp`. Both are in the repo rather than in a scratchpad, which
is where the last staged patch went to be forgotten.

Neither is a substitute for looking at the screen — **the first device run should be a visual
check, not a benchmark.** If the analysis is wrong the failure is a colour that saturates where it
should have wrapped, which the harness cannot see and a screenshot can.

## 3 — Measurement debt

- [ ] *(low priority)* **A visual check on the build host.** Not a gap in what can be tested —
      the Pyra launches the game routinely, and the fixture already puts the player at a fixed
      spot, so the visual check for a shader change has a home. This would only make it cheaper by
      not costing device time. `oot.o2r` is on the build host after all
      (`~/Games/ComboShip-orig/`, `~/Downloads/SoH-Ackbar-Delta-Linux/`), so the "no ROM on the
      host" line several documents repeat is wrong about the archive — but a full host link and a
      virtual display are still missing, and llvmpipe would answer "did the colours change" and
      nothing else. Worth an hour only if device time becomes the bottleneck.
- [ ] **Re-baseline report 7's absolute FPS figures** on the report 9 harness. The fixture and the
      CPU pinning landed; the absolutes themselves were never re-taken, so every number in report 7
      is still stale and still quoted.
- [ ] Add the **Hyrule Field** and **Market/Kakariko** fixtures. Easy now via `SOH_BENCH_WATCH`.
      One indoor fixture is not a workload.

## 4 — Decisions waiting on the device's owner

- [ ] **`InterpolationFPS`**: the speed/smoothness trade (§7.5, §7.5b). 20 gives correct speed and
      a choppier picture; 30 gave 0.92× slow motion at report 7's 27.7 FPS. **That number is stale
      and the trade may have dissolved** — the fixture now runs 33.9 FPS on the shipped
      `FILTER_LINEAR` default (§8.14), which is above 30 and would make `InterpolationFPS = 30`
      correct speed *and* smoother. Do the re-baseline in section 3 before putting this to anyone;
      it may already be a free choice rather than a trade.
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
