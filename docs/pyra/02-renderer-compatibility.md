# Report 2 — Renderer Compatibility (Fast3D → GLES 2.0 on SGX544MP2)

All line numbers verified against this tree: `libultraship` @ `911fd17d`.

## 2.1 Architecture trace

```
soh/src/**/*.c  (F3DEX2 display lists)
  → soh/soh/OTRGlobals.cpp, GbiWrap.cpp        (OTRGetPixelDepth and friends)
  → Fast::Fast3dWindow                          libultraship/src/fast/Fast3dWindow.cpp
        owns a GfxWindowBackend* and a GfxRenderingAPI*
        InitWindowManager() at :130-156 switches on Ship::WindowBackend
  → Fast::Interpreter                           libultraship/src/fast/interpreter.cpp (4759 lines)
        the "gfx_pc" equivalent: RSP/RDP emulation, triangle batching into mBufVbo,
        framebuffer management, texture cache, colour-combiner → shader-id derivation
  → Fast::GfxRenderingAPI  (pure virtual)       libultraship/include/fast/backends/gfx_rendering_api.h:30-85
  → GfxRenderingAPIOGL                          libultraship/src/fast/backends/gfx_opengl.cpp (1036 lines)
  → GfxWindowBackendSDL2                        libultraship/src/fast/backends/gfx_sdl2.cpp (742 lines)
  → shader templates                            libultraship/src/fast/shaders/opengl/default.shader.{vs,fs}
        run through "prism", a @if/@for text macro preprocessor — NOT a GLSL transpiler
  → ImGui 1.91.9b-docking via imgui_impl_sdl2 + imgui_impl_opengl3
```

Two facts that make this port tractable:

1. **There is no GLX/EGL code at all.** Window and context creation are entirely SDL2's job
   (`SDL_GL_CreateContext` at `gfx_sdl2.cpp:397`). No new window backend is needed for EGL —
   SDL2 already speaks EGL on X11. This is purely an attribute-negotiation change.
2. **The shader generator is already dialect-parameterised.** `gfx_opengl.cpp:279-300` (fragment)
   and `:342-357` (vertex) feed prism a table of `GLSL_VERSION` / `attr` / `out` / `texture` /
   `vOutColor` per platform. The existing legacy desktop branch (`#version 130`, `attribute`,
   `varying`, `texture2D`, `gl_FragColor`) is *already structurally GLSL ES 1.00* — it is missing
   only the version token and the precision statement.

## 2.2 Prior art: upstream PR #1288 already does most of this

[Kenix3/libultraship#1288](https://github.com/Kenix3/libultraship/pull/1288) — **open**, 1 commit
(`dbb0435`), 9 files, +280/−33. Adds `USE_OPENGLES2` (implies `USE_OPENGLES`), a `#version 100`
shader dialect, a textured-quad replacement for `glBlitFramebuffer`, `mMaxMsaaLevel` pinned to 1,
`mask_width`/`mask_height` uniforms replacing `textureSize()`, and `IMGUI_IMPL_OPENGL_ES2`.

**Caveat: it is written against upstream `main`, which has migrated to SDL3** (`gfx_sdl.cpp`,
`find_package(SDL3)`). This fork's libultraship is still **SDL2** (`gfx_sdl2.cpp`) and sits
**43 commits behind / 3 ahead** of `upstream/main`. Only ~5 lines of the PR touch SDL, so
**backporting the PR onto the SDL2 fork is far cheaper and lower-risk than taking the SDL3
migration and the GLES2 work in the same step.** Recommend backport, not rebase.

## 2.3 Feature table — GLES3/desktop-only usage vs. this device

Legend: **PR** = already handled by upstream PR #1288. **DEV** = device measurement changes the answer.

| # | Feature | Site | GLES2 status on SGX544 | Fix | Effort |
|---|---|---|---|---|---|
| 1 | GLES3 headers | `include/fast/backends/gfx_opengl.h:19` → `<GLES3/gl3.h>` | n/a | `<GLES2/gl2.h>` + `gl2ext.h` | **PR** |
| 2 | `USE_OPENGLES` means GLES**3** | `src/CMakeLists.txt:10` `option(USE_OPENGLES "Enable GLES3" OFF)` | n/a | add `USE_OPENGLES2` | **PR** |
| 3 | No ES context requested from SDL | `gfx_sdl2.cpp:333-344` — profile/major/minor set only under `#if defined(__APPLE__)` | SDL would hand back a GLX/desktop context → **Mesa llvmpipe** | `SDL_GL_CONTEXT_PROFILE_ES` + 2.0 | **PR** (as `gfx_sdl.cpp`; trivial to port to `gfx_sdl2.cpp`) |
| 4 | VAOs | `gfx_opengl.cpp:685` `glGenVertexArrays` under `__APPLE__ \|\| USE_OPENGLES` | **`GL_OES_vertex_array_object` IS present** — but not needed | skip VAO on ES2 (attribs are re-specified per draw anyway at `:45-57`) | **PR** |
| 5 | Sized internal formats | `:554` `GL_RGBA8`, `:730,:773` `GL_RGB8` | **`GL_OES_required_internalformat` + `GL_OES_rgb8_rgba8` present → legal here** | PR's `#define GL_RGBA8 GL_RGBA` is unnecessary but harmless | **PR / DEV** |
| 6 | MSAA renderbuffers | `:709` `GL_MAX_SAMPLES`, `:778,:790` `glRenderbufferStorageMultisample` | not core ES2; **`GL_EXT/IMG_multisampled_render_to_texture` present** (implicit resolve) | pin `mMaxMsaaLevel = 1`. Do **not** chase MSAA — fill rate is the budget here | **PR** |
| 7 | `glBlitFramebuffer` | `:846` (MSAA resolve), `:909`, `:926` (`CopyFramebuffer`), `:993` (depth staging) | absent; **no `NV_`/`ANGLE_framebuffer_blit` either** | textured full-screen quad | **PR** |
| 8 | `glReadBuffer` | `:921,:923,:930` | not in ES2 | drop (single attachment implied) | **PR** |
| 9 | Depth readback | `:962`, `:1002` `glReadPixels(GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8)` — **already `#ifndef USE_OPENGLES`, i.e. already dead on the existing GLES3 build** | ES2 forbids depth readback entirely | PR skips the staging blit and returns zeros — matches existing GLES3 behaviour, **not a regression**. Real fix needs a depth→colour encode pass; `GL_OES_depth_texture` is present, so it is possible later | **PR** (degraded) |
| 10 | Colour readback format | `:944` `glReadPixels(..., GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, ...)` | **device reports `IMPLEMENTATION_COLOR_READ_FORMAT = GL_BGRA_EXT`, `TYPE = GL_UNSIGNED_BYTE`** — `5_5_5_1` is **not** a legal pair here | read `RGBA`/`UNSIGNED_BYTE` (always legal) and repack to 5551 on CPU | **DEV — not in PR** |
| 11 | Combined depth/stencil attachment | `:704,:796,:798,:974` `GL_DEPTH_STENCIL_ATTACHMENT`; `:699,:741,:788,:973` `GL_DEPTH24_STENCIL8` | `GL_DEPTH_STENCIL_ATTACHMENT` is ES3-only; **`GL_OES_packed_depth_stencil` + `GL_OES_depth24` present**, so the *format* is fine | attach the same RBO to `GL_DEPTH_ATTACHMENT` and `GL_STENCIL_ATTACHMENT` separately (PR's `AttachDepthStencil()`). Note: **no `glStencilFunc`/`glStencilOp` exists anywhere in this backend** — stencil is entirely unused and could simply be dropped | **PR** |
| 12 | `GL_MIRROR_CLAMP_TO_EDGE` | `:560` `#define GL_MIRROR_CLAMP_TO_EDGE 0x8743`, used at `:570` in `gfx_cm_to_opengl()` | **absent on this device** (no `EXT_texture_mirror_clamp`) — will silently become an invalid enum | emulate mirror-then-clamp in the fragment shader, or fall back to plain `GL_MIRRORED_REPEAT` and accept edge artefacts on affected tiles | **NOT in PR** |
| 13 | NPOT + repeat/mirror wrap | implicit at `:586-587` | **`GL_OES_texture_npot` present → no restriction** | none | **DEV — risk removed** |
| 14 | `mix(vec3, vec3, bvec3)` | `default.shader.fs:63` in `fromLinear()` (sRGB path) | GLSL ES 1.00 has no bool-selector `mix` | `mix(higher, lower, vec3(cutoff))` | **PR** |
| 15 | `textureSize()` | `default.shader.fs:110,112` (mask size) | ES 1.00 has none | `mask_width[2]`/`mask_height[2]` uniforms, mirroring the existing `texture_width[2]` plumbing at `gfx_opengl.cpp:64-75` | **PR** |
| 16 | Float suffix in GLSL | `default.shader.vs` — `gl_Position.z *= 0.3f;` | `0.3f` is illegal in GLSL ES 1.00 **and** GLSL 1.10 | `0.3` | **PR** |
| 17 | Missing `precision` statement | `gfx_opengl.cpp:279-300` — only the GLES3 branch emits `precision mediump float;` | ES 1.00 **requires** a default float precision in the fragment shader | emit it; **`GL_OES_fragment_precision_high` is present**, so `highp` is available where needed | **PR** |
| 18 | `gl_FragDepth` | `default.shader.fs` `@if(o_prim_depth)` | **`GL_EXT_frag_depth` is ABSENT on this device** | **PR resolves this to `gl_FragDepthEXT` + `#extension GL_EXT_frag_depth : enable`, which will fail to compile here.** Needs a third case: omit the depth write entirely | **NOT in PR — device-specific defect** |
| 19 | ImGui GLES2 backend | `src/ship/window/gui/Gui.cpp:214` picks `#version 300 es`; vendored `imgui_impl_opengl3.h:56-58` auto-selects ES3 on Android | ES2 branch exists in the vendored file but is dormant | `IMGUI_IMPL_OPENGL_ES2` + `ImGui_ImplOpenGL3_Init("#version 100")` | **PR** |
| 20 | Everything else | — | **none used** | Repo-wide grep found **zero** uses of: UBOs, `glVertexAttribIPointer`, `glDrawBuffers`, instancing, query objects, `glMapBufferRange`, PBOs, sampler objects, `glTexStorage2D`, sRGB textures, `GL_CLAMP_TO_BORDER`, anisotropic filtering, `glPolygonMode`, `glLineWidth`, `glPrimitiveRestart`, `GL_TEXTURE_LOD_BIAS`, `glGenerateMipmap`, texture swizzle, `glDebugMessageCallback` | nothing to do | **None** |

## 2.4 THE blocker — varying budget

**Measured: `MAX_VARYING_VECTORS = 8`.**

Worst case emitted by `default.shader.vs`:

| Varying | Count | Vectors (with ES 1.00 packing) |
|---|---|---|
| `vInput1..vInput7` (vec4 when `o_alpha`) | 7 | 7 |
| `vFog` (vec4) | 1 | 1 |
| `vGrayscaleColor` (vec4) | 1 | 1 |
| `vTexCoord0`, `vTexCoord1` (vec2) | 2 | 1 (packed) |
| `vTexClampS0/T0/S1/T1` (float) | 4 | 1 (packed) |
| | | **≈ 11 — against a hard limit of 8** |

`numInputs` is bounded by `SHADER_INPUT_7` (`include/fast/interpreter.h:45-51`) and computed at
`src/fast/interpreter.cpp:4701-4709`. Anything above roughly **5 inputs + fog + grayscale** will
fail to link on this GPU.

`MAX_VERTEX_ATTRIBS = 16` is likewise **exactly** the worst-case attribute count
(1 position + 2 texcoord + 4 clamp + fog + grayscale + 7 inputs = 16). No headroom, but it fits.

PR #1288 cannot have caught this: it validated with `glslangValidator`, which checks syntax
against the dialect, not a specific device's resource budget.

> **Refined in report 5 §5.4.** Static analysis of the slot allocator narrows the failure
> envelope considerably: it requires `numInputs >= 5` **and** fog **and** grayscale at once.
> The mitigation order below is revised there.

### Mitigations, cheapest first

1. **Measure the real distribution before designing anything.** Add a one-line log of
   `(numInputs, o_fog, o_grayscale, o_alpha, used clamps)` at shader-creation time
   (`gfx_opengl.cpp:463-472`) in the existing **x86 build**, play a representative slice of the
   game, and histogram it. OoT's combiners are not arbitrary — the realistic ceiling is probably
   3–4 inputs, in which case this is a narrow tail, not a wall. **This is the single most
   valuable thing to do before writing any renderer code.**
2. **Hand-pack the varyings.** Fold the 4 clamp floats into one `vec4` and the 2 texcoords into
   one `vec4` explicitly rather than trusting the driver's packer. Saves nothing if the packer is
   good, guarantees the floor if it is not.
3. **Move per-draw-constant inputs to uniforms.** Several combiner inputs (prim colour, env
   colour) are constant across a batch. Passing them as uniforms instead of per-vertex attributes
   removes them from the varying budget entirely. `MAX_FRAGMENT_UNIFORM_VECTORS = 64` has ample
   room (current use is ~13 vectors + 6 samplers). This is the real fix for the tail, and it is
   a change to the *shader generator*, not the renderer.
4. **Graceful degradation as a backstop.** If a variant still exceeds the budget, drop
   `o_grayscale` (a cosmetic enhancement) before dropping anything gameplay-visible, and log it.

## 2.5 Features that downgrade cleanly (no work)

- Vertex submission: one dynamic VBO, `glBufferData(..., GL_STREAM_DRAW)` per flush
  (`gfx_opengl.cpp:672`), `glDrawArrays(GL_TRIANGLES, ...)` (`:673`). Pure ES2.
- All attributes are `GL_FLOAT` via `glVertexAttribPointer` (`:52-53`). No integer attributes.
- Blend / depth / scissor / viewport / `glPolygonOffset` (`:661-665`) — all core ES2.
- Filtering is `GL_LINEAR`/`GL_NEAREST` only (`:582-584`); the N64 three-point filter is done in
  the fragment shader (`default.shader.fs:66-73`), so no hardware mip/aniso dependency exists.
- Combiner math emits only ES2-legal builtins (`mod`, `clamp`, `abs`, `sign`, `step`, `fract`,
  `dot`, `sin`, `pow`, `mix` with float/vec).
- `discard` (`default.shader.fs:183,198`) — core ES2.
- **No runtime loops or deep branching.** All `@for`/`@if` are resolved at shader-*generation*
  time by prism. The only runtime branch is a single-level `if (texture_filtering[id] == ...)`.
  This matters: SGX544's USSE is a poor fit for dynamic control flow, and this generator
  produces none.
- Resolution scaling / internal-resolution multiplier / texture cache are all CPU-side and
  backend-agnostic.

One defensive note: `default.shader.fs:75-84` indexes uniform arrays (`texture_filtering[id]`,
`texture_width[tex]`) with a **function-parameter** index. That is spec-legal in ES 1.00, but the
PowerVR SGX GLSL compiler has a long history of poor codegen for non-constant uniform-array
indexing. Since the surrounding `@for` is unrolled at generation time anyway, rewriting those
call sites to pass literal constants costs nothing and removes a class of driver-quirk risk.

## 2.6 Features needing real redesign

1. **Depth readback (`GetPixelDepth`)** — consumers are `soh/src/code/z_lights.c:399` (light glow
   occlusion) and `Environment_GetPixelDepth` / `soh/src/code/z_kankyo.c:232-242`. Already broken
   on the existing GLES3 build, so shipping it degraded is not a regression. `GL_OES_depth_texture`
   is available, so a depth→RGBA encode pass is possible in a later phase if the visual difference
   turns out to matter.
2. **`CopyFramebuffer` rescale/flip** — PR #1288's quad blit handles this, but it must reproduce
   the existing Y-flip and MSAA-resolve special-casing at `gfx_opengl.cpp:866-936`. Worth close
   review during backport; this is where render-to-texture effects will break if it is wrong.
3. **Mirror-clamp wrap emulation** (table row 12) — needs per-fragment UV folding.
4. **`gl_FragDepthEXT` fallback** (table row 18) — needs a third dialect case.

## 2.7 Minimum viable renderer

Backport PR #1288 onto the SDL2 fork, then add four device-driven deltas the PR does not have:

1. `gl_FragDepth` → **omit entirely** when `GL_EXT_frag_depth` is unavailable (row 18).
2. `glReadPixels` 5551 → `RGBA`/`UNSIGNED_BYTE` + CPU repack (row 10).
3. `GL_MIRROR_CLAMP_TO_EDGE` → shader emulation or documented fallback (row 12).
4. Varying-budget handling (§2.4) — driven by the Phase 0 measurement.

Everything else in the OpenGL backend — vertex batching, texture cache, state management,
combiner codegen — needs no change.
