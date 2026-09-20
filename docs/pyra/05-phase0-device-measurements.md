# Report 5 — Phase 0 Device Measurements

Measured on the target hardware on 2026-09-20. Probe sources live on the device at
`~/pyra-probe` (`sdlprobe.c`, `fill.c`, `draws.c`, `modes.c`, `abl.c`, `abl2.c`).

## 5.1 Phase 0.2 — can SDL2 reach the SGX driver? **YES**

Test: SDL2 2.32.4 creates an `SDL_GL_CONTEXT_PROFILE_ES` 2.0 context on X11 and resolves
`glGetString` through `SDL_GL_GetProcAddress` (i.e. exactly what libultraship does — the probe
deliberately does **not** link `libGLESv2` itself).

| Configuration | Result |
|---|---|
| Defaults, no env | `SDL_CreateWindow: **GLX is not supported**` — hard failure |
| `SDL_VIDEO_X11_FORCE_EGL=1` | context created, but **`glGetString` returns NULL** — silent soft failure |
| `+ SDL_VIDEO_GL_DRIVER=<DDK>/libGLESv2.so.1` | **`PowerVR SGX 544MP`, OpenGL ES 2.0** ✅ |
| `+ SDL_VIDEO_EGL_DRIVER` as well | identical — **the EGL override is not needed** |
| `SDL_VIDEO_GL_DRIVER` + `SDL_VIDEO_EGL_DRIVER`, no `FORCE_EGL` | `GLX is not supported` |

### Minimum required environment — exactly two variables

```sh
export SDL_VIDEO_X11_FORCE_EGL=1
export SDL_VIDEO_GL_DRIVER=/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1
```

The system `libEGL.so.1` (libglvnd) routes to the DDK correctly on its own. It is only the GLES
**client** library that must be forced, because SDL2's built-in default is `libGLESv2.so.2` while
the DDK's SONAME is `libGLESv2.so.**1**`.

### Correction to report 3 §3.5

That section predicted SDL would silently fall through glvnd to **Mesa llvmpipe**. That is
**wrong** — there is no working GLX on this system at all, so the desktop-GL path hard-fails
rather than degrading to software. The two real failure modes are:

1. **No env** → `GLX is not supported`, loud and obvious.
2. **`FORCE_EGL` alone** → a context is created but every GL entry point resolves to NULL. This
   is the dangerous one: it looks like success and then crashes or draws nothing.

The remedy predicted in §3.5 was right; the mechanism was not. Failure mode 2 is the reason the
Phase 2 `GL_RENDERER` startup check is still worth adding — it just needs to check for NULL as
well as for a non-PowerVR string.

## 5.2 Fill rate — far more headroom than expected

Textured, alpha-blended full-screen quads, vsync off:

| Resolution | Overdraw 1 | Overdraw 4 | Overdraw 8 | Peak |
|---|---|---|---|---|
| 640×360 | 851 fps | 526 fps | 347 fps | **639 Mpix/s** |
| 800×450 | 695 fps | 387 fps | 256 fps | **736 Mpix/s** |
| 1280×720 | 353 fps | 192 fps | 117 fps | **862 Mpix/s** |

At the target of 640×360 / 30 fps, even a pessimistic 8× overdraw costs ~55 Mpix/s against a
~860 Mpix/s ceiling — roughly **6% of available fill rate**. Fill rate is not a constraint, and
notably *1280×720 native at 30 fps with 8× overdraw is also within budget* (221 Mpix/s).

## 5.3 Draw-call overhead — ~14 µs, not a blocker

Measured with a 1×1 scissor so fragment cost is ~zero, isolating driver/CPU cost.
16 triangles per batch, 300 batches per frame:

| Per-draw state changes | µs/draw | draws/s |
|---|---|---|
| draw only | 11.4 | 88,000 |
| + `glVertexAttribPointer` ×2 | 10.8 | 92,000 |
| + `glBindTexture` | 14.5 | 69,000 |
| + `glUniform4f` | 11.9 | 84,000 |
| **all three (realistic)** | **14.2** | **70,000** |

Scaling batches per frame at full state changes holds ~11–14 µs/draw from 300 to 1000 batches,
so the cost is genuinely per-call and does not degrade with batch count.

**Budget:** at 30 fps (33 ms/frame), 14 µs/draw allows ~2,300 draw calls per frame before the
driver alone consumes the entire frame. A realistic OoT scene is a few hundred. Driver overhead
is therefore **not** the limiting factor.

### Vertex upload strategy — one clear thing to avoid

| Strategy | µs/draw |
|---|---|
| `glBufferData` per draw (**what Fast3D does today**, `gfx_opengl.cpp:672`) | 68 |
| single `glBufferData` per frame + offsets | 60 |
| client-side vertex arrays, no VBO | 58 |
| **`glBufferSubData` into a ring buffer** | **221 — avoid** |

(These four numbers were taken without the scissor, so they include fragment cost and are not
comparable to the table above; only their *relative* ordering is meaningful.)

Fast3D's existing `glBufferData`-per-flush is within noise of the alternatives and needs no
change. The actionable finding is negative: **do not "optimise" this into `glBufferSubData`** —
it is ~4× worse on this driver. Worth a comment in the code so nobody tries it later.

### Correction to report 4 §4.6

That section predicted performance would "depend mostly on fill rate and draw-call count, not on
the CPU". The measurements say the opposite: fill rate has ~15× headroom and draw-call overhead
has ~6× headroom at the target. **The bottleneck is therefore expected to be CPU-side** — Fast3D
display-list interpretation, the game's own simulation, and audio mixing on 2 × Cortex-A15
@ 1.5 GHz.

Consequences for Phase 5, which should be reprioritised:
- The NEON audio mixer (`-mfpu=neon-vfpv4`, report 1 §1.7) moves **up** — it is CPU work.
- Disabling three-point filtering moves **down** — it is fragment work, and fragments are cheap.
- Internal resolution can likely be **800×450**, possibly native 720p, rather than 640×360.
  Resolution costs fill rate, which is the resource we have most of.
- `GL_EXT_discard_framebuffer` is still worth doing (it removes memory-bandwidth traffic, which
  is shared with the CPU on this SoC) but is no longer expected to be transformative.
- CPU profiling (`perf`) on the device becomes the primary Phase 5 tool, not GPU tracing.

## 5.4 Phase 0.1 — varying budget, refined statically

The runtime histogram could not be collected: this working tree has `soh.o2r` but **no
`oot.o2r`**, so the game has no ROM assets to boot. The same question was answered statically
instead, from `Interpreter::CreateColorCombiner` (`libultraship/src/fast/interpreter.cpp:256-328`).

Input slots are allocated from exactly **7 distinct RGB combiner sources**:
`G_CCMUX_PRIMITIVE`, `PRIMITIVE_ALPHA`, `PRIM_LOD_FRAC`, `SHADE`, `ENVIRONMENT`, `ENV_ALPHA`,
`LOD_FRACTION` (`:306-317`). Alpha uses a separate table with 5 distinct sources (`:370-377`).
`numInputs = max(rgb, alpha)`, so the hard ceiling of 7 is reached only by a 2-cycle combiner
referencing all seven distinct RGB sources across its 8 slots.

Varying cost under GLSL ES 1.00 packing is therefore **`numInputs + 4`** worst case
(N × vec4 inputs, `vFog`, `vGrayscaleColor`, texcoords packed into one row, clamps packed into
one row) against the measured limit of **8**:

| Configuration | Vectors | Fits in 8? |
|---|---|---|
| N=4, fog, grayscale, 2 tex, clamps | 8 | ✅ exactly |
| N=5, fog, grayscale | 9 | ❌ |
| N=5, fog, **no grayscale** | 8 | ✅ |
| N=6, no fog, no grayscale | 8 | ✅ |
| N=7, fog, grayscale | 11 | ❌ |

**This is much narrower than report 2 §2.4 feared.** Failure requires `numInputs ≥ 5` *and* fog
*and* grayscale simultaneously. `opt_grayscale` is an SoH enhancement rather than stock OoT
rendering, so the overlap should be rare.

Revised mitigation order for Phase 4 item 4:
1. Hand-pack clamps and texcoords explicitly rather than trusting the driver's packer (this is
   what makes the table above hold).
2. Drop `o_grayscale` from the varying set when the budget would be exceeded — it is cosmetic,
   and dropping it alone recovers every N ≤ 5 case.
3. Only if N ≥ 6 variants actually appear in play: migrate per-draw-constant combiner inputs
   (prim/env colour) from attributes to uniforms. `MAX_FRAGMENT_UNIFORM_VECTORS = 64` has ample
   room.

This remains worth confirming empirically once `oot.o2r` is available — a one-line log at
`gfx_opengl.cpp:463` during a play-through would settle whether step 3 is ever needed.

## 5.5 Phase 0 verdict

| Check | Result |
|---|---|
| SDL2 → hardware GLES2 on SGX544 | ✅ **GO**, two env vars |
| Fill rate at target resolution | ✅ ~15× headroom |
| Draw-call throughput | ✅ ~6× headroom |
| Varying budget | ✅ **resolved in §5.6** — max numInputs is 4 game-wide; fits in 8 exactly |
| Toolchain / dev packages on device | ✅ installed (gcc 14.2, cmake 3.31.6, ninja, all `-dev`) |
| Shader-variant distribution | ✅ **resolved in §5.6** — scanned exhaustively from `oot.o2r` |

No blocker survived Phase 0. Proceed to Phase 1.

## 5.6 Phase 0.1 RESOLVED — varying budget measured across the whole game

With `oot.o2r` available, the question was settled exhaustively rather than by play-through.
`oot.o2r` is an uncompressed zip of serialised resources; display lists carry the magic `ODLT`
and store `Gfx` commands as pairs of little-endian 32-bit words with the opcode in bits 31..24.
`docs/pyra/tools/ccscan.py` walks every display list, decodes each `G_SETCOMBINE` (0xFC) with the
same bit layout the interpreter uses (`gfx_set_combine_handler_rdp`,
`libultraship/src/fast/interpreter.cpp:3774-3781`), and replays the slot allocator from
`Interpreter::GenerateCC` (`:185-328`).

```
$ python3 docs/pyra/tools/ccscan.py /path/to/oot.o2r
```

### Results — every display list in the game

| | display lists | `G_SETCOMBINE` commands | unique combine modes |
|---|---|---|---|
| `oot.o2r` | 9,099 | 11,878 | **317** |

| `numInputs` | 1-cycle | 2-cycle (upper bound) |
|---|---|---|
| 0 | 0.37% | 0.06% |
| 1 | 87.78% | 4.03% |
| 2 | 10.83% | 92.51% |
| 3 | 1.02% | 3.34% |
| 4 | — | **0.06%** (7 commands) |
| **≥5** | **0** | **0** |

Cross-checks:
- `soh.o2r` (port-authored assets) contains **no display lists at all** — only audio, textures
  and JSON. Nothing to scan.
- 106 `gDPSetCombineLERP` call sites in the decompiled C source (`soh/src`, `soh/soh`) reach a
  maximum of **N=3**.

### Verdict: the varying budget fits — exactly

**`numInputs` never exceeds 4 anywhere in the game.** Worst-case varying cost is therefore:

| Component | Vectors |
|---|---|
| 4 × `vInput` (vec4) | 4 |
| `vFog` | 1 |
| `vGrayscaleColor` | 1 |
| `vTexCoord0` + `vTexCoord1` (2 × vec2, packed) | 1 |
| 4 × clamp floats (packed) | 1 |
| **Total** | **8 — exactly `MAX_VARYING_VECTORS`** |

Without grayscale (an SoH enhancement, not stock rendering) it is 7, leaving one spare.

**Blocker 1 from the executive summary is closed.** The mitigations in report 2 §2.4 items 2–4
(uniform migration, feature degradation) are **not needed**.

One item does get promoted, though: because the worst case lands on exactly 8 with **zero
headroom**, it depends on the driver achieving the optimal packing described in GLSL ES 1.00
Appendix A. Item 1 — **explicitly hand-packing the two texcoords into one `vec4` and the four
clamp floats into another** — therefore moves from "defensive, optional" to **required**. It
removes the dependence on the packer entirely and costs nothing.

### Two incidental findings

1. **Only 317 unique combine modes exist in the whole game.** The runtime shader-variant count is
   small and bounded, which makes the `GL_OES_get_program_binary` shader cache (report 5 §5.3 /
   plan Phase 5) both cheap to implement and highly effective — the entire game's shader set
   could be pre-warmed.
2. **36 `G_SETCOMBINE` commands use ccmux values the interpreter's switch does not handle**
   (`interpreter.cpp:322`, `fprintf(stderr, "Unsupported ccmux: %d\n", ...)`). Most likely
   `G_CCMUX_SHADE_ALPHA` (11) or `G_CCMUX_K5` (15). This is pre-existing upstream behaviour on
   every platform, not Pyra-specific, but it is a latent correctness gap worth reporting upstream
   separately.
