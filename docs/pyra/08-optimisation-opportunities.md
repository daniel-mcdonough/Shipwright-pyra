# Report 8 — Code-level optimisation opportunities

Everything in report 7 was reachable from the settings menu. This report covers what is *not*:
changes to the renderer and the Fast3D interpreter themselves.

Nothing here is implemented. This is a survey with evidence and estimates, written so the work
can be picked up in any order. Magnitudes marked *estimated* have **not** been measured on the
device — see §8.9 for why that distinction is load-bearing here.

## 8.1 The premise

Report 7 §7.2 established that the workload is **strongly fragment-bound**: halving internal
resolution gains +75%. Report 7 §7.6 established that the CPU side has no dominant hotspot —
the largest single bucket is ~13%.

That combination points the work in one direction. The GPU is a **PowerVR SGX544MP2, a
tile-based deferred renderer (TBDR)**, and Fast3D was written against immediate-mode desktop
OpenGL. Most of the items below are places where that mismatch costs something specific.

The three Tier 1 items are all consequences of the same mismatch. They are not independent
micro-optimisations; they are the same architectural gap seen from three angles.

## 8.2 Tier 1.1 — Three-point filtering is a per-fragment runtime branch

**Evidence**

- `libultraship/include/fast/backends/gfx_opengl.h:138` — `FilteringMode mCurrentFilterMode = FILTER_THREE_POINT;`
- `libultraship/src/fast/Fast3dWindow.cpp:107` — `CVAR_TEXTURE_FILTER` defaults to `FILTER_THREE_POINT`
- `libultraship/src/fast/backends/gfx_opengl.cpp:324` — `{ "o_three_point_filtering", mCurrentFilterMode == FILTER_THREE_POINT }`
- `libultraship/src/fast/backends/gfx_opengl.cpp:685` — per-texture: `filtering = !linear_filter ? FILTER_LINEAR : FILTER_THREE_POINT;`
- `libultraship/src/fast/shaders/opengl/default.shader.fs:68-84`

`filter3point()` performs **three `texture2D` fetches** plus `fract`/`step`/`sign`/`abs` maths per
texture unit. In 2-cycle mode with two texture units that is six fetches where bilinear does two.

**The part that is a settings issue, and the part that is not**

The *default* is UI-reachable (`CVAR_TEXTURE_FILTER`, exposed at
`soh/soh/SohGui/SohMenuSettings.cpp:422`), so choosing bilinear is a settings decision and belongs
in report 7's territory. The *implementation* is a separate, code-level problem:

```glsl
vec4 hookTexture2D(in int id, sampler2D tex, in vec2 uv, in vec2 texSize) {
@if(o_three_point_filtering)
    if(texture_filtering[id] == @{FILTER_THREE_POINT}) {
        return filter3point(tex, uv, texSize);
    }
@end
    return @{texture}(tex, uv);
}
```

The choice is made **per fragment, by a dynamic branch on a `uniform int` array**. The
compile-time `@if` only decides whether the branch exists at all; when the global mode is
three-point — the default — every shader variant carries both paths and the runtime test.

SGX544's USSE has weak flow control. If the driver's compiler flattens this branch, **every
textured fragment pays all three fetches**, including tiles the N64 explicitly marked unfiltered
and which take the `FILTER_LINEAR` path at `gfx_opengl.cpp:685`. Whether it flattens is a
property of the IMG shader compiler and must be checked, not assumed (§8.9).

**Fix.** Move filtering into the shader variant key rather than a uniform. Variants then contain
exactly one sampling path with no branch, and the common case compiles to a single fetch. This
helps every GLES target, not just this one, so it is upstreamable on its own merits.

**Estimated gain:** large but unquantified. **Risk:** low; increases variant count (currently 317
game-wide, so headroom is ample). **Upstreamable:** yes.

## 8.3 Tier 1.2 — Dependent texture reads defeat the SGX prefetcher

PowerVR SGX can only prefetch texel data when the texture coordinate arrives **directly from a
varying with no fragment-side arithmetic**. Any computation on the coordinate before the fetch
makes it a *dependent read*, which stalls the prefetch pipeline. This is the best-known
performance characteristic of the architecture and Fast3D violates it nearly everywhere.

**Evidence** — `libultraship/src/fast/shaders/opengl/default.shader.fs`

```glsl
// :53  — three-point path computes the coordinate inline
#define TEX_OFFSET(off) @{texture}(tex, texCoord - off / texSize)

// :101 — the clamp path, taken whenever o_clamp is set for a tile
vec2 vTexCoordAdj@{i} = clamp(vTexCoord@{i}, 0.5 / texSize@{i}, vec2(vTexClampS@{i}, vTexClampT@{i}));
```

Both produce dependent reads. The clamp path is the more damaging of the two because it applies
to ordinary bilinear draws, so it is *not* fixed by resolving §8.2.

**Fix.** The shader clamp exists because N64 clamp semantics clamp at an arbitrary S/T value
rather than at the texture edge. **When the N64 clamp value coincides with the texture edge — the
common case — hardware `GL_CLAMP_TO_EDGE` is exact**, and the coordinate can be passed straight
through from the varying. Detect that case at tile-setup time and emit a variant with no
fragment-side coordinate maths.

**Estimated gain:** potentially the largest item in this report, and the least certain.
**Risk:** medium — needs care to confirm the edge case is genuinely equivalent before using it.
**Upstreamable:** yes, though the motivation is SGX-specific.

## 8.4 Tier 1.3 — Every texture is uploaded as RGBA8888

**Evidence**

- `libultraship/src/fast/backends/gfx_opengl.cpp:642-643` — the only upload path:
  ```c
  void GfxRenderingAPIOGL::UploadTexture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
  }
  ```
- `libultraship/src/fast/interpreter.cpp:483-949` — every `ImportTexture*` expands to 32 bpp

N64 source textures are 4, 8 or 16 bpp. All of them are widened to 32 bpp before upload. The
following reductions are **lossless**, not approximations:

| N64 format | Current | Exact GLES2 equivalent | Saving |
|---|---|---|---|
| RGBA16 (5551) | 32 bpp | `GL_RGBA` + `GL_UNSIGNED_SHORT_5_5_5_1` | 2× |
| I4 / I8 | 32 bpp | `GL_LUMINANCE_ALPHA` | 2× |
| IA4 / IA8 / IA16 | 32 bpp | `GL_LUMINANCE_ALPHA` | 2× |
| CI4 / CI8 (RGBA16 TLUT) | 32 bpp | `GL_RGBA` + `GL_UNSIGNED_SHORT_5_5_5_1` | 2× |

The I-format case is worth stating precisely, because it is the one that looks wrong and is not.
`ImportTextureI4` (`interpreter.cpp:676`) and `ImportTextureI8` (`interpreter.cpp:726`) write
`r = g = b = a = intensity` — alpha is the intensity, not 1.0. `GL_LUMINANCE_ALPHA` reproduces
that exactly: luminance to RGB, alpha to A. Plain `GL_LUMINANCE` would **not** (it forces
alpha = 1) and must not be used.

`GL_EXT_texture_rg` is also available on the device, but `GL_LUMINANCE_ALPHA` is core GLES2 and
gives the correct swizzle without a shader change, so it is the better choice here.

**What this buys.** This is not about upload cost — the texture cache holds 1024 entries
(`interpreter.cpp:65`), which is ample for OoT, so uploads are not happening per frame. It is
about **sampling bandwidth and texture cache residency** on an SoC where the GPU shares DDR3L
with both CPU cores. Halving texel size doubles effective cache hit rate.

**Estimated gain:** moderate to large on a bandwidth-starved part. **Risk:** low — lossless and
mechanical. **Upstreamable:** yes; benefits any memory-constrained target.

## 8.5 Tier 2 — Cheap and certain

### `GL_EXT_discard_framebuffer` is present and unused

Depth and stencil are resolved out to main memory at the end of every tile pass, for data nothing
ever reads back (see §8.7 — the one consumer is dead code).

Per frame at the current defaults: 640×360×4 B for the game framebuffer plus 1280×720×4 B for the
default framebuffer ≈ **4.6 MB of writeback per frame**, ~140 MB/s at 30 FPS, entirely wasted.
Discarding depth/stencil before swap and before each framebuffer switch removes it.

Small patch, no visual risk, extension confirmed present in the device string (report 0).

**Measured: no benefit. See §8.10.**

### Unconditional `mod()` in the fragment shader

`default.shader.fs:170`:

```glsl
texel = WRAP(texel, -0.51, 1.51);   // #define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)
texel = clamp(texel, 0.0, 1.0);
```

Applied to every fragment. In 1-cycle mode the combiner output is already in range and the
following `clamp` makes the `mod` a no-op — but it is still executed. `mod` on a vec3/vec4 costs
several cycles. Guard it with `@if` for 1-cycle variants.

> **The premise is wrong as written, and the fix is a different one. 2026-09-22.** Cycle count
> does not bound the combiner output. One cycle computes `(A - B) * C + D`, which reaches
> `[-1, 2]` from operands in `[0, 1]`, and the wrap is load-bearing there: an output of 1.8 has
> to come out as −0.22 and clamp to black, not saturate to white. Guarding on `o_2cyc` would
> have changed what the screen shows.
>
> What does bound it is the *shape* of the formula. `gfx_cc_get_features`
> (`interpreter.cpp:4752`) already reduces a degenerate combiner to one of three shorter forms,
> and none of them can leave `[0, 1]`:
>
> | Condition | Emitted | Range |
> |---|---|---|
> | `C == 0` → `do_single` | `D` | `[0, 1]` |
> | `B == 0 && D == 0` → `do_multiply` | `A * C` | `[0, 1]` |
> | `B == D` → `do_mix` | `mix(B, A, C)` | `[0, 1]` |
> | otherwise | `(A - B) * C + D` | `[-1, 2]` |
>
> Every operand a cycle can name is itself in `[0, 1]`: the constants, the shade/prim/env inputs
> (`uint8_t` divided by 255 at `interpreter.cpp:1835`, and barycentric interpolation does not
> extrapolate), a texture sample, and the noise term. Three-point filtering is not an exception —
> its weights are `1-|ox|-|oy|`, `|ox|`, `|oy|` with `|ox| + |oy| <= 1`, so it is a convex
> combination of three samples. The one operand that is not bounded is `SHADER_COMBINED`, the
> previous cycle's output, which is in range only if that cycle was.
>
> So the guard is per cycle and per formula shape, not per cycle count, and it extends to the
> second wrap the report does not mention — the one between the two cycles at `:143-152`.
> Implemented that way; see `CombinerCycleInRangeFor` in `gfx_opengl.cpp` and `NEXT.md` item A.
>
> **How much it covers: 95.2% of the `G_SETCOMBINE` commands in `oot.o2r`** (1-cycle, alpha on —
> the conservative assumption), against 28.4% of the distinct shader variants. Measured with
> `tools/wrapscan.py`, which reproduces `GenerateCC` over the archive and agrees with §5.6 on the
> two figures that report published. Static command counts, not fragments — it bounds how broadly
> the elision applies, not what it is worth.

### `int` uniforms converted to float per fragment

`default.shader.fs:47-51` declares `texture_width[2]`, `texture_height[2]`, `mask_width[2]`,
`mask_height[2]` as `int` arrays, and `:86-87` converts them per fragment:

```glsl
#define TEX_SIZE(tex)  vec2(texture_width[tex],  texture_height[tex])
#define MASK_SIZE(tex) vec2(mask_width[tex], mask_height[tex])
```

The SGX fragment ALU is float. Pass these as `vec2` uniforms and the conversion disappears.
Trivial, free, no behaviour change. (Note the `mask_*` uniforms are ours — added in the GLES2
port to replace `textureSize()`, report 2 row 15 — so at minimum we should not be adding to the
problem.)

> **Implemented, unmeasured. 2026-09-22.** Behind `SHADER_OPT_FLOAT_TEX_SIZE`. Two things to know
> when the number arrives. It also removes one `glUniform1iv` per draw per texture pair, because
> one `glUniform2fv` now carries width and height together — so a win cannot be attributed to the
> per-fragment conversion alone. And the conversion may already be free: the PowerVR compiler
> hoists expressions that depend only on uniforms into the secondary attribute update program,
> which would make this exactly zero. Neither can be settled from the host.

## 8.6 Tier 3 — CPU side

### Per-draw `glBufferData` orphaning

`gfx_opengl.cpp:772-773` — a single VBO, fully respecified per batch:

```c
glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
```

Measured draw-call overhead is ~11–14 µs (report 5, corrected). `GL_OES_mapbuffer` and
`GL_OES_vertex_array_object` are both available on the device. A ring buffer with
`glBufferSubData` into a pre-allocated VBO would avoid the per-draw reallocation.

~~Needs a draw-call-per-frame count before it can be sized — that number is not yet measured.~~

> **Refuted — do not build this. 2026-09-22.** It does not need a draw-call count. Report 5
> §5.3 had already measured this exact proposal on this exact driver, in the same report that
> supplied the µs/draw figure quoted above:
>
> | Strategy | µs/draw |
> |---|---|
> | `glBufferData` per draw — what the code does today | 68 |
> | single `glBufferData` per frame + offsets | 60 |
> | client-side vertex arrays | 58 |
> | **`glBufferSubData` into a ring buffer** | **221** |
>
> Four times worse, and report 5 said so in as many words: *"do not 'optimise' this into
> `glBufferSubData`."* The one variant report 5 does not refute is a single `glBufferData` per
> frame with offsets, at 60 against 68 — but that is 12% of a path costing ~14 µs per draw in a
> frame-bound workload, well inside the noise of what building it would take.
>
> This is the worst kind of error in the catalogue: not an over-estimate, but a proposal to
> build something the project had already measured as harmful. It survived because this report
> was written from the code rather than from the measurements, and because nothing checked the
> two reports against each other. Caught by the external triage in
> `~/git/dragonbox-pyra/docs/optimization-playbook.md` §4.
>
> Report 5 also asked for a comment in the code so nobody would try it later. That was never
> done either; it is now at `gfx_opengl.cpp` next to the `glBufferData` call.

### Program binary cache

`GL_OES_get_program_binary` and `GL_IMG_program_binary` are both present. Fast3D compiles variants
lazily and the SGX GLSL compiler is slow. Only **317 unique variants exist game-wide** (report 5
§5.6), so the whole set can be cached on disk or even pre-warmed.

This removes first-encounter hitching. It does **not** affect steady-state throughput — worth
doing for feel, not for the FPS number.

### Audio NEON is worth less than it first appears

Report 7 §7.6 lists `aEnvMixerImpl`, `aADPCMdecImpl` and `aResampleImpl` at ~6% combined, all
scalar. But these run on `OTRAudio_Thread` (`soh/soh/OTRGlobals.cpp:1113`) — **core 1, off the
critical path**, contending only with the resource thread pool
(`libultraship/src/ship/resource/ResourceManager.cpp:55`, which resolves to 1 worker on this
2-core part). Converting them to NEON would reduce total CPU and heat, which is not nothing on a
thermally throttled handheld, but it is unlikely to move frame rate.

The ~13% shared_ptr / resource-cache / malloc bucket is on core 0 and matters more. See report 7
§7.6 for why the obvious fix there (#989) is not free.

## 8.7 Ruled out

Two things that looked like large problems and are not. Recording them so they are not
re-investigated.

**Depth readback is dead code.** `GetPixelDepth` forces a full pipeline flush on a TBDR and would
be the single most expensive thing in the frame if it were called. It is not: a repo-wide search
found **no caller outside libultraship itself**. The 1×1 `mPixelDepthFb`/`mPixelDepthRb` created
in `Init()` (`gfx_opengl.cpp:798-812`) are harmless. The GLES2 readback path added during this
port (`gfx_opengl.cpp:1193-1262`) is correct but never exercised.

**MSAA resolve is correctly skipped.** With `MSAAValue = 1`, `interpreter.cpp:4391-4402` takes the
`else` branch and hands the game framebuffer texture straight to ImGui — no resolve blit. The
extra full-screen pass that does happen (render at 640×360 to `mGameFb`, then draw it as a
1280×720 quad) is inherent to the internal-resolution feature, not a defect.

## 8.8 Summary

**Read §8.9 before acting on this table.** The estimates are reasoning rather than data, the
tier ordering turned out to be the wrong way to sequence the work, and item 1's entry overstates
what the code change alone recovers. Current status is in the last column.

| # | Item | Tier | Est. gain | Risk | Upstreamable | Status |
|---|---|---|---|---|---|---|
| 1 | Filtering into shader key, not a uniform branch (§8.2) | 1 | **none on the shipped default** | low | yes | **CLOSED here — the `FILTER_LINEAR` default already strips the branch at compile time, §8.16** |
| 2 | Kill dependent texture reads via `GL_CLAMP_TO_EDGE` (§8.3) | 1 | largest / least certain | medium | yes | **CLOSED — already implemented, §8.15.** 97.6% of tiles already take the cheap path; the 3.6% left is correctness |
| 3 | Native texture formats instead of RGBA8888 (§8.4) | 1 | moderate–large | low | yes | confidence lowered again, §8.12 |
| 4 | `GL_EXT_discard_framebuffer` (§8.5) | 2 | **measured: none (§8.10)** | very low | device-specific | done, no gain |
| 5 | Conditional `mod()` in 1-cycle variants (§8.5) | 2 | small | very low | yes | now measurable, §8.9 |
| 6 | `int` → `vec2` size uniforms (§8.5) | 2 | small | none | yes | now measurable, §8.9 |
| 7 | VBO ring buffer (§8.6) | 3 | **negative** | medium | yes | **REFUTED — report 5 §5.3 measured it at 4× worse. Do not build. §8.6** |
| 8 | Program binary cache (§8.6) | 3 | hitching only | low | device-specific | not attempted |
| 9 | NEON for the three scalar audio routines (§8.6) | 3 | heat, not FPS | low | yes | still live — see §8.11 |

~~The one measurement to take next is `gSettings.TextureFilter` 0 vs 1 on the report 9 harness.~~
**Taken — see §8.12.** Three-point filtering costs 17.75% of frame time; nearest buys nothing
over bilinear. Item 2's premise survived it, item 1's code change is confirmed small, and the
large lever turned out to be a settings default rather than any of the nine items.

## 8.9 Measurement discipline

**The estimates above are reasoning, not data.** Items 1–3 are structural findings backed by code
references and by documented SGX architectural behaviour; their *magnitudes* are not.

Report 5 §5.2-5.3 concluded from a synthetic fill-rate benchmark that the GPU had ~15× headroom
and the bottleneck was CPU-side. Report 7 §7.3 records that this was **wrong in its ordering** —
the real workload is strongly fragment-bound. The synthetic test measured a best case and was
read as a prediction.

The same trap applies to everything here. In particular:

- **§8.2 rests on an untested assumption** — that the IMG compiler flattens the dynamic branch.
  If it does not, the win shrinks substantially. Check the generated binary or A/B a
  branch-free variant before investing in the shader-key rework.
- **§8.3 is the largest claim and the weakest evidence.** It should be prototyped crudely on one
  variant before the general case is attempted.
- ~~**§8.6 (VBO) cannot be sized at all** without a draw-call-per-frame count.~~ **Wrong — it
  was already measured, in report 5 §5.3, at 4× worse than the code today. See §8.6.** This
  bullet is the clearest illustration of the point the section is making: the report asked for
  a new measurement while the answer sat in an earlier report by the same author.

Every A/B must be taken at **thermal equilibrium** (report 7 §7.1) and with both frame caps
raised, or it measures the thermostat and the frame limiter rather than the change.

### Update: the ordering below was wrong, and the rig has been rebuilt

Two corrections to this section.

**The suggested order was `4 → 6 → 5 → 3 → 1 → 2`, cheapest-and-most-certain first. That is the
wrong objective when the measurement floor is ±13%.** Items 6 and 5 are worth low single digits
each and would have been unmeasurable — the work would have been done and nothing learned. Only
large-effect items are worth spending while the rig is coarse.

**§8.2 split the filtering problem the wrong way round.** It treated the three-point *default* as
a settings matter outside this report's scope and kept only the branch-flattening question. But
re-reading `gfx_opengl.cpp:722-725`, the bulk of the cost is the three texture fetches on tiles
the N64 genuinely marked filtered, and the shader-variant rework does **not** remove those — the
fragments still want three-point. What the rework recovers is the flattening penalty on
*point*-filtered tiles, which in OoT is largely HUD. So the code-level item is probably small and
the large lever is a one-line default. Measure `gSettings.TextureFilter` 0 against 1 before
building anything: it is a faithful proxy for item 1's best case, because with the global mode
set to linear the `@if` deletes the branch and leaves exactly one fetch — the same shader the
rework would emit. That single measurement also discriminates items 1, 2 and 3 at once: if
collapsing three fetches to one moves nothing, fragment fetch count is not the constraint and
item 2's premise goes with it.

The measurement rig itself has since been rebuilt — see **report 9**. The relevant change for
this report is that an A/B no longer has to run two processes minutes apart with the scene
drifting underneath it, so the items above can be screened at a resolution that makes them worth
attempting.

#### And that partly un-does the correction above

The rig was measured on hardware on 2026-09-21 and resolves **0.40%** (report 9 §9.7), against
the ±13% assumed when the paragraphs above were written. So the claim that items 5 and 6 are
"below the floor and would teach nothing" **is no longer true**. A change worth 2% is now
comfortably measurable; so is one worth 1%.

That does not promote them to the front of the queue. The argument for doing items 1, 2 and 3
first was never only about measurability — they are the items with a live mechanism behind them
(fragment-shader work, which is what the evidence says binds), and items 5 and 6 remain small by
their own estimates. What changes is that items 5 and 6 are now worth *doing* rather than worth
*skipping*: a 1-2% gain that can be demonstrated is a real gain, and both are nearly free.

The sequencing that follows from a 0.40% rig is:

1. `gSettings.TextureFilter` 0 vs 1 — no code, screens items 1, 2 and 3 together.
2. Whichever of items 1-3 that result leaves standing.
3. Items 6 and 5, which are cheap, safe, and now measurable.

Item 7 (VBO ring buffer) still cannot be sized without a draw-call-per-frame count, and that is
unchanged by the new rig.

## 8.10 Result: `GL_EXT_discard_framebuffer` (item 4) — no measurable gain

Implemented and measured first, as the cheapest and most certain item. **It gains nothing on this
hardware.** Recorded here so the idea is not revisited expecting a win.

### Method

Two binaries differing only in the change, deployed together and run **alternately at one thermal
state** so that drift hits both arms equally rather than being attributed to the change. Five
A/B pairs, 60 s each, after a 300 s warm-up to equilibrium (76–80 °C). 640×360 internal, vsync
off, MSAA 1, frame cap raised to 60 so the limiter could not mask a difference. FPS sampled in
5 s windows; the first three samples of each run were dropped as launch and shader-compile
transient.

### Result

| Pair | A (no discard) | B (discard) | Δ |
|---|---|---|---|
| 1 | 29.44 | 29.32 | −0.13 |
| 2 | 29.45 | 28.52 | −0.93 |
| 3 | 29.11 | 29.16 | +0.05 |
| 4 | 29.35 | 29.35 | +0.00 |
| 5 | 29.19 | 29.25 | +0.06 |
| **mean** | **29.31** | **29.12** | **−0.19 (−0.64%)** |

Paired t(4) = −0.99, **not significant** (critical ±2.776). The design could have resolved an
effect of ±0.52 FPS (1.8%); the true effect is smaller than that. The sign is negative but the
per-pair deltas change sign, so this is noise, not a regression.

### The null result is real, not a silent failure

A discard that raises `GL_INVALID_ENUM` is a no-op and would look exactly like this. That was
ruled out with a temporary probe calling `glGetError` after each discard:

```
GL_EXT_discard_framebuffer: in use
DISCARD-PROBE fb=fbo     (idx 1) glGetError=0x0000
DISCARD-PROBE fb=default (idx 0) glGetError=0x0000
```

Both call sites execute and the driver accepts both. The optimisation genuinely does not help.

### Why, most likely

The §8.5 estimate of ~140 MB/s of avoided writeback was right in magnitude but wrong in
relevance. Report 7 §7.2 shows the workload is **fragment-shader bound** — ALU and texture fetch
— not starved of raw memory bandwidth. Freeing bandwidth that was not the binding constraint
changes nothing. The SGX driver may also already elide the writeback on its own: the depth
attachment is a renderbuffer that is never sampled as a texture, and the default framebuffer's
ancillary buffers are covered by EGL's default `EGL_SWAP_BEHAVIOR` of `EGL_BUFFER_DESTROYED`.

### The measured configuration ran the game at 0.48x speed

The frame cap was raised to 60 for this test so the limiter could not mask a difference. That has
a consequence worth recording, because it was not anticipated: one call to `RunCommands`
(`soh/soh/OTRGlobals.cpp:1794-1825`) is **one game-logic tick**, and it renders N interpolated
sub-frames where N averages `InterpolationFPS / 20`. So

```
game speed = render FPS / InterpolationFPS
```

At 29 render FPS against a 60 cap the game logic ran at 9.7 Hz rather than 20 - roughly half
speed.

This does **not** invalidate the A/B. Both arms shared the configuration, and halving the
game-logic rate while holding rendering at 29 full display-list traversals per second makes the
workload *more* GPU-weighted than real gameplay, not less. If CPU contention were masking a
GPU-side win, this is the configuration that would have exposed it. The null result is therefore
conservative.

It does mean the measured workload is not the shipping one, and a confirmation pass at the
shipping cap would be worth doing before item 3 is judged on similar evidence.

### Consequence for the rest of this report

This is the second time a bandwidth-or-throughput argument has failed against this workload
(report 7 §7.3 was the first). It lowers confidence specifically in **item 3** (§8.4, native
texture formats), whose case is also "halve the memory traffic": if bandwidth is not the
constraint, halving texel size may likewise buy nothing. Item 3 should be prototyped on a
single format and measured before the full conversion is written.

It does **not** bear on items 1 and 2 (§8.2, §8.3), whose case is fragment-shader work — fetch
count and prefetch stalls — which is what the evidence says actually binds. Those now look like
the only Tier 1 items with a live rationale, and the order in §8.9 should be revised to put them
first.

## 8.11 Item 9 survives the audio DSP move

Audio playback on the Pyra now goes through the OMAP5 AESS DSP. It would be easy to read that as
retiring item 9, and it does not.

The DSP took over **playback** — moving finished samples to the codec, which is what PulseAudio
was spending ~15.3% of a core on. Item 9 is about **synthesis**: `aEnvMixerImpl`,
`aADPCMdecImpl` and `aResampleImpl` in `soh/soh/mixer.c` are the N64 audio microcode emulated
in-process to produce those samples in the first place. Nothing about a DSP downstream of them
changes that work, and all three are still scalar C on a core with NEON — `aMixImpl` already has
a NEON path (`soh/soh/mixer.c:689`) and correspondingly does not appear in the profile, which is
the direct evidence that the approach works on the three that do.

What *has* changed is the value of the item. Its original justification was "heat, not FPS" —
the workload is fragment-bound (§8.10, report 7 §7.3), so returning ~6% of a core was not
expected to move frame rate much. That argument is unchanged. What is no longer true is the
surrounding context in which it was ranked: the CPU is less contended than it was, which makes a
CPU-side saving *less* likely to show up as frame rate, not more.

So item 9 stays where it is — Tier 3, worth doing for thermal headroom and battery, not as a
performance fix — and the reasoning behind that placement is now firmer rather than weaker.

## 8.12 Result: three-point filtering costs 17.75% of frame time

Measured 2026-09-22 on the report 9 harness, Kokiri Forest (scene 85, entrance `0x00EE`), static
scene, 640×360 game framebuffer, 5 ABBA pairs of 600 frames each.

### Run 1 — `FILTER_THREE_POINT` (0) against `FILTER_LINEAR` (1)

| Arm | Mean frame | FPS |
|---|---|---|
| 0 — three-point | 34098.2 µs | 29.33 |
| 1 — linear | 28046.9 µs | 35.65 |
| **delta** | **6051.3 µs** | **+6.33 FPS** |

`paired n=5 mean_delta_us=-6051.28 sd_us=87.40 t=-154.818 df=4`, against `mde_us=108.66`. The
effect is **56× the detection threshold**. As a fraction of the three-point frame it is
**17.75%**; expressed as a speed-up over three-point it is **+21.6%**.

`FILTER_THREE_POINT` is the shipped default — `libultraship/src/fast/Fast3dWindow.cpp:107`
initialises `CVAR_TEXTURE_FILTER` to it.

### Run 2 — `FILTER_LINEAR` (1) against `FILTER_NONE` (2)

| Arm | Mean frame | FPS |
|---|---|---|
| 1 — linear | 28063.5 µs | 35.63 |
| 2 — nearest | 28036.8 µs | 35.67 |

`paired n=5 mean_delta_us=-26.74 sd_us=57.53 t=-1.039 df=4`, against `mde_us=71.52` (0.26%).
**No effect.** Nearest buys nothing over bilinear, so it costs quality for no gain and is off
the table.

### What the pair of results means

Run 2 is what makes run 1 interpretable. Bilinear reads four texels where nearest reads one, and
that difference is unmeasurable — because the SGX texture unit filters in fixed-function hardware,
so the extra taps never become shader work. Three-point costs 17.75% because Fast3D emulates it
in the fragment shader: three `texture2D` *instructions* plus `fract`/`step`/`sign`/`abs` per
texture unit (`default.shader.fs:68-84`), six fetches in 2-cycle mode where bilinear issues two.

So the expensive thing is **shader-issued fetch instructions and the ALU around them**, not
memory traffic. That bears on three items:

- **Item 2 (dependent texture reads, §8.3) survives.** §8.9 said this measurement would kill
  item 2's premise if collapsing three fetches to one moved nothing. It moved a great deal, so
  fragment fetch cost is confirmed as a real constraint on this part.
- **Item 3 (native texture formats, §8.4) is weakened, but not refuted.** Its argument is
  footprint and bandwidth. Run 2 shows the texture unit's own taps are free, which is evidence
  against a *bandwidth* bottleneck — but it says nothing about the working-set effect of halving
  every texture, because both arms used the same RGBA8888 textures. Treat item 3 as still open
  and lower confidence.
- **Item 1 (shader-variant key, §8.2) is confirmed small, as §8.9 predicted.** 17.75% is the
  gain from not wanting three-point at all. The code rework does not deliver that; it recovers
  only the flattening penalty on tiles the N64 marked point-filtered, which in OoT is mostly HUD.
  The large lever here is a default, not a refactor.

### This is a fidelity trade, and it should not be fixed by changing the default

Three-point is the N64's actual filter and upstream chose it deliberately. Switching
`Fast3dWindow.cpp:107` would put a device-specific preference into shared code for every platform,
which is exactly the kind of change this port is supposed to avoid. The 17.75% argues for:

1. A **Pyra default profile** shipping `gSettings.TextureFilter=1`, leaving upstream's default and
   the user's ability to change it untouched.
2. **Saying so in the menu.** On this hardware the setting is worth 6 FPS and its description does
   not hint that it is the most expensive option on the page.

Not yet checked: whether linear introduces visible seams on this content, which would make the
trade worse than the frame time suggests. That is a look-at-the-screen judgement, not a
measurement.

### Caveats on the number itself

- **17.75% is viewpoint-specific.** It was taken at the treehouse spawn — camera 139 units from
  Link, facing him, little scene behind. A fragment-cost percentage depends on what else is in
  the frame, and this frame is not representative. The magnitude is far too large for the sign to
  change, but the figure should not be quoted as scene-independent. The fixture needs a viewpoint
  with real depth before item 2 or 3 is judged on it.
- **16000 warm-up frames is still not thermal equilibrium.** The linear arm drifted
  27836 → 28236 µs across run 1 (1.44%) as the die went 46.1 → 76.8 °C. That drift is 3.6× the
  rig's resolution; ABBA cancelled it, the machine was not steady.

> **Correction (report 9 §9.9).** Every number in §8.12 was measured at a viewpoint that was not
> the one recorded. The player pins never held — they were applied once at settle frame 30 and
> then walked ~500 units away by ordinary player update — so the frame measured was not the
> "treehouse close-up" described here. It was a large ground plane with a ladder in the right
> foreground and the player out of frame behind the camera. The runs were reproducible and the
> A/B is internally valid; the *description* of what was measured was wrong, and the percentages
> belong to that frame rather than to a viewpoint anyone chose. Re-measured at the replacement
> fixture in §8.14.

## 8.13 Result: linear filtering is visually free on this content

§8.12 left the fidelity half of the trade unmeasured, and flagged it as the thing that could make
a 21% win a bad deal: three-point is the N64's own filter, and if dropping to bilinear introduced
visible seams where three-point was papering over texture-coordinate wrapping, the frame rate
would be bought with image quality.

It does not. The same frozen frame was captured under all three `gSettings.TextureFilter` modes at
the report 9 §9.9 fixture and compared at 3× zoom on a texture-heavy region — a stone pillar, a
fence, and distant terrain at several depths.

| Comparison | RMSE | By eye at 3× |
|---|---|---|
| three-point vs **linear** | 1.5% | no difference found |
| three-point vs **nearest** | 2.7% | obviously blockier and aliased |

The whole-frame difference image between three-point and linear is black at auto-level: mean
absolute difference 1.14/255 (0.45%), σ 3.25. What little structure it has sits on texture
boundaries, which is where the two filters genuinely differ, and the residual includes actor
animation phase — Navi orbits, so she is not in the same place in two separately launched runs.

The nearest row is the control, and it matters. It proves the comparison is sensitive enough to
see a filtering change: the same method that finds nothing between three-point and linear has no
trouble at all with nearest. A null from a blind instrument would be worthless.

**So the trade is not a trade.** On this content bilinear costs nothing visible. That strengthens
the §8.12 recommendation rather than qualifying it — though it remains a Pyra default profile
question, not a reason to touch upstream's default in `Fast3dWindow.cpp:107`.

One honest limit: this is one frame of one outdoor scene. Three-point's advantage over bilinear is
largest on low-resolution textures magnified close to the camera, and the place that bites in OOT
is more likely a dungeon wall or a text box than a village vista. The finding is "no visible cost
here", not "no visible cost anywhere".

## 8.14 Re-measured at a fixture someone chose: 19.88%

§8.12's 17.75% was taken at a viewpoint that turned out not to be the one recorded (report 9
§9.9). The measurement is repeated here at the replacement fixture — Kokiri Forest village square
looking down the valley, walked to by hand, verified to reproduce.

| Arm | Frame time | FPS |
|---|---|---|
| `FILTER_THREE_POINT` (0) | 36904 µs | **27.10** |
| `FILTER_LINEAR` (1) | 29548 µs | **33.85** |
| delta | **−7336.6 µs** | +6.75 FPS |

```
paired n=5 mean_delta_us=-7336.61 sd_us=302.17 t=-54.290 df=4
mde_us=375.68 (1.02% of frame)
pair deltas -20.78, -20.61, -18.87, -19.62, -19.58 %
arm sequence 0,1,1,0,0,1,1,0,0,1 (ABBA)   69.99 -> 79.36 C
```

**Three-point filtering costs 19.88% of frame time — more at a viewpoint with depth, not less.**
The effect is 19.5× the detection threshold. §8.12's caveat said the treehouse frame might be
understating or overstating the cost and that the direction was not knowable without a second
viewpoint; the answer is understating.

The practical form of that number matters more than the percentage. At this fixture three-point
runs at **27.1 FPS, under the 30 FPS target**, and linear clears it at **33.9**. The same change
that was "a fidelity trade worth considering" at the old viewpoint is, at this one, the difference
between missing and meeting the frame rate goal — and §8.13 established the fidelity side costs
nothing visible here.

### This run also exposed a confound the rig does not control

The pair standard deviation was 302.17 µs against 87.40 µs in §8.12, and the resolution fell from
0.26–0.40% to 1.02%. The cause is not the scene. The CPU governor is `ondemand`, with eleven
frequency steps from 250 MHz to 1.5 GHz, and it moved **1.4 GHz → 1.0 GHz across the run**. The
linear arm steps from ~29235 µs in its first two segments to ~29750 µs in its last three, a 1.8%
shift with no counterpart in the three-point arm.

ABBA cancels *linear* drift, which is why the effect survives intact. A step change part-way
through is not linear drift, and it lands unevenly across pairs — visible in the deltas, which run
−20.78, −20.61 then −18.87, −19.62, −19.58.

Every earlier run happened to sit at 1.25 GHz throughout, so this never showed up. It is luck, not
control. Pinning the governor to `performance` for the duration of a run would remove it; that is
a device-level change requiring root, so it is recorded here rather than done. Until then, treat
sub-1% effects at this fixture as unresolved rather than absent.

## 8.15 Item 2 was already implemented, and its remaining 3.6% is the price of being correct

§8.3 proposed detecting, at tile-setup time, the case where the N64's clamp coincides with the
texture edge and letting hardware `GL_CLAMP_TO_EDGE` handle it instead of the fragment shader. It
called this *"potentially the largest item in this report"*, and §8.12 promoted it to top code
candidate.

**It has been implemented the whole time.** `interpreter.cpp:1648`:

```c
if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
    tm |= 1 << 2 * i;      // shader clamp
    cms &= ~G_TX_CLAMP;    // and drop the hardware clamp
}
```

The shader path is taken *only* when mirroring is on or the tile's clamp rectangle differs from
the texture width. Otherwise the flag is never set, `cms` keeps `G_TX_CLAMP`, `gfx_cm_to_opengl`
maps it to `GL_CLAMP_TO_EDGE`, and the shader compiles
`vec2 vTexCoordAdj0 = vTexCoord0;` — a straight varying pass-through with no fragment-side
arithmetic. The equivalence is exact: the shader clamps to `[0.5/W, (tex_width2-0.5)/W]` and
`GL_CLAMP_TO_EDGE` clamps to `[0.5/W, (W-0.5)/W]`, the same interval when `tex_width2 == W`.

§8.3's load-bearing claim — *"the clamp path is the more damaging of the two because it applies to
ordinary bilinear draws"* — is false. Instrumented at the §9.9 fixture, over 300 frames:

```
tiles=480000  shader_clamp=11400 (2.38%)  hw_clamp=268500 (55.94%)  mirror_clamp=6300
```

**97.6% of tile setups already take the non-dependent path.**

### The residual is not available

Tile setups are not pixels, so 2.38% of setups could still be a large share of fragments. Probed
by forcing the shader clamp off entirely and measuring: **27629 µs against a 28673 µs baseline, a
3.6% saving.** So those tiles do carry more than their share of fill.

That 3.6% cannot be collected. Both surviving cases are genuinely inexpressible:

- **`MIRROR|CLAMP`** (6300 of 11400, over half). GLES2 has no `GL_MIRROR_CLAMP_TO_EDGE` and SGX544
  exposes no extension for it; `gfx_cm_to_opengl` already degrades the sampler to
  `GL_MIRRORED_REPEAT`, and the shader clamp is the only thing making it correct.
- **Clamp rectangle narrower than the texture.** The N64 clamps at a coordinate *inside* the
  texture; `GL_CLAMP_TO_EDGE` clamps at the edge. A different place.

The probe measures what could be saved by rendering incorrectly. **Item 2 is closed with zero
headroom.** The detection at `interpreter.cpp:1648` is already exact, and there is no
already-equivalent case being missed.

### What the 3.6% does suggest

The width-mismatch case exists because a tile addresses a sub-rectangle of a larger uploaded
texture. Uploading that sub-rectangle as its own texture would make `tex_width2 == tex_width`, move
the clamp to hardware, and make the read non-dependent. That is a change to texture upload and
cache policy — related to §8.4, not to §8.3 — and it trades texture-cache entries and upload churn
for fragment cost. It is a **new** candidate, unestimated, and it is capped by the non-mirror share
of that 3.6%.

### How this item was ranked wrong

§8.3 was written by reading `default.shader.fs` and never reading the call site that decides which
variant is compiled. The estimate *"potentially the largest item in this report, and the least
certain"* was half right: it was the least certain, and the uncertainty was not where it was
claimed to be. Every other estimate in §8.5-§8.7 was produced the same way. Items 3, 5, 6, 7 and 8
should be treated as unvalidated readings of code rather than as a work queue — each one needs the
§8.9 treatment of a crude probe before it is worth implementing.

## 8.16 Item 1 is dead on the shipped default, and the flattening question with it

Prompted by `~/git/dragonbox-pyra/docs/gpu-optimization.md` §4.2, which raises the
branch-flattening question independently and uses this exact shader as its worked example of a
pattern to avoid. Checking it against what we shipped turns the question inside out.

`o_three_point_filtering` is a **compile-time** substitution, not a uniform
(`gfx_opengl.cpp:364`):

```cpp
{ "o_three_point_filtering", mCurrentFilterMode == FILTER_THREE_POINT },
```

and it gates the whole block in `default.shader.fs`:

```glsl
vec4 hookTexture2D(in int id, sampler2D tex, in vec2 uv, in vec2 texSize) {
@if(o_three_point_filtering)
    if(texture_filtering[id] == @{FILTER_THREE_POINT}) {
        return filter3point(tex, uv, texSize);
    }
@end
    return @{texture}(tex, uv);
}
```

With `mCurrentFilterMode == FILTER_LINEAR` — the Pyra default since §8.14 — the `@if` block is
not compiled at all. `hookTexture2D` *is* `texture2D(tex, uv)`: one fetch, no `filter3point`, no
`texture_filtering` test, nothing for a compiler to flatten.

That is exactly what item 1's proposed fix was for. **It has already been obtained, for free, by
a settings default.** Item 1's remaining scope is users who turn three-point back on in the menu,
where the variant-key rework would still remove a branch — real, upstreamable good practice, and
worth nothing on the shipped configuration.

### The flattening question is answered where it matters and unanswerable where it does not

§8.9 and §8.2 both flagged "does the IMG compiler flatten this branch?" as an untested
assumption that item 1's magnitude rested on. Two things are now true:

- **On the shipped default it cannot arise.** There is no branch in the generated source.
- **On the three-point path, §8.14 does not isolate it.** That A/B compared a shader containing
  `filter3point` *and* the branch against a shader containing neither, so the 19.88% is the sum of
  the extra fetches and whatever the branch costs. Splitting them would need a third arm —
  unconditional `filter3point`, no branch — and the answer would inform only a configuration we
  do not ship.

**Verdict: do not build item 1 here, and do not run the flattening A/B for it.** If it is ever
offered upstream, it should be for correctness of style rather than for a number.

### What survives from §4.2 of the hardware document

The general rule does — *prefer shader variants over uniform branches* — and it applies to items
5 and 6, which are still live and still unmeasured. It also explains a piece of existing code
that looks odd: `ClearShaderCache` exists precisely because `o_three_point_filtering` is **not**
part of the variant key, so flipping the CVar at runtime has to throw the whole pool away. That
is the cost of the compile-time approach, and on a fork that ships one filtering mode it is a
cost we pay once at startup and never again.

### How this item was ranked wrong

Item 1 was written as "large but unquantified" from the same reading habit that mis-ranked item 2
(§8.15): the shader source was read, and the C++ that decides which shader source exists was not.
Three of the nine items have now been closed by reading the call site. That is not a coincidence,
it is a method failure, and the fix is the one §8.9 already named — probe before estimating.

## 8.17 Result: items 5 + 6 measured — 2.4%, and almost all of it is the wrap

Measured 2026-09-22 on the report 9 harness at the §9.9 fixture, CPU pinned, five ABBA pairs per
arm, with the new thermal soak (§9.10) so every run started measuring at equilibrium. Every
segment ran between 80.6 and 83.6 °C.

| arm | what it turns on | mean Δ | sd | t (df=4) | MDE | effect |
|---|---|---|---|---|---|---|
| `1` | item 5, elide the no-op `mod()` | **−696.4 µs** | 125.1 | −12.45 | 155.5 | **−2.37%** |
| `2` | item 6, `int` → `vec2` size uniforms | −158.8 µs | 87.4 | −4.06 | 108.7 | −0.54% |
| `3` | both | **−704.2 µs** | 142.7 | −11.04 | 177.4 | **−2.40%** |

29,282 → 28,578 µs, so 34.15 → 34.99 FPS at this fixture. Both halves are real — item 6 clears its
own MDE, if only just — but they are **not additive**: 696 + 159 = 855 against a combined 704. The
wrap elision is essentially the whole win, and turning item 6 on as well buys nothing measurable
on top of it. Item 6 is worth keeping for the `glUniform` call it removes per draw rather than for
anything it does per fragment; §8.5's note that the PowerVR compiler may already hoist the
conversion looks right.

This is the largest confirmed win in the catalogue that is not a fidelity trade, and it is
invisible: §8.5's item 5 was ranked "small".

**The premise in §8.5 was wrong and the correction is what made the item big.** The report said
the wrap is a no-op in 1-cycle mode. It is not — one cycle computes `(A-B)*C+D`, which reaches
`[-1,2]`. It is a no-op when the combiner *reduced* to `D`, `A*C` or `mix(B,A,C)`, which is a
different test, independent of cycle count, and one that also covers a second wrap between the two
cycles that §8.5 never mentions. `tools/wrapscan.py` puts 95.2% of the `G_SETCOMBINE` commands in
`oot.o2r` in the qualifying set (28.4% of distinct variants — the game reaches for the simple
combiners). Implementing the report as written would have guarded on `o_2cyc` and changed what the
screen shows.

### It was checked against the screen, and the first three checks were wrong

`scripts/pyra/shotcheck.sh` (report 9 §9.11) grabs frames off the device for each arm and diffs
them. Its first three runs all reported large, structured differences — Link's shield losing its
crest, his tunic changing colour — and all three were artefacts:

- **Leaked processes.** The game catches `SIGTERM` and shuts down slowly, so `pkill` returned
  before the process died and each run launched alongside its predecessors. Eight instances
  accumulated, all fullscreen, and `xwd -root` photographs whichever is on top — so the "arms"
  being compared were sometimes two launches of the same arm, and sometimes an arm from three runs
  earlier.
- **No control.** Two launches of the *same* arm differ by ~10,000–18,000 pixels at this fixture,
  with a worst channel near 240, because particles and fairies come to rest in different places
  each time and then hold still there for the whole frozen run. A mask built from frames within
  one launch does not remove them. Without a null, any pixel count looks alarming.

With the leak fixed and a third run of arm A as an in-session control, all four comparisons land
on the floor: control 17,557 pixels, both-arms 18,020 (and in an earlier clean session, control
10,004 against 9,999 for item 5, 9,809 for item 6, 9,973 for both). **The arms are visually
identical.** The tool now always captures the control, because the number means nothing without it.

## 8.18 The same optimisation is worth −3.59%, −2.40% and nothing, depending on the scene

§8.17 measured one frozen frame in Kokiri Forest and reported 2.40%. `NEXT.md` ground rule 4 said
to confirm that on a second scene before treating it as the game's number. It was right to, and
not for the reason expected — the second scene did not shrink the number, it produced three
different ones.

Measured 2026-09-23, same harness, same CPU pin, same soak, `gSettings.ShaderOpts` 0 → 3:

| fixture | what it is | mean Δ | t | pairs | effect |
|---|---|---|---|---|---|
| Market Day (§9.9c) | a dozen townspeople, near and far buildings | **−1167.9 µs** | −23.74 | 6 | **−3.59%** |
| Kokiri Forest (§9.9) | enclosed village square, layered combiners | **−704.2 µs** | −11.04 | 5 | **−2.40%** |
| Hyrule Field (§9.9b) | open plain, long draw distance, sky | −20.5 µs | −0.35 | 8 | **−0.07%** |

The Market result is the largest confirmed win in this catalogue: 30.71 → 31.90 FPS, and every one
of its six pairs landed between −3.1% and −4.0%. The Hyrule Field result is a clean null — eight
pairs, sd 164 µs, resolving about 0.46%, so 2.4% is firmly excluded rather than merely unproven.

> **Superseded by §8.19 (2026-09-23).** Every run in this section was capped at 30 FPS by a
> harness bug, so the Hyrule Field nulls below are the cap absorbing the change, not evidence
> about fragment cost. The Market and Kokiri effects are real in direction, distorted in size.

### Hyrule Field is not fragment-bound, and that is the finding

Two independent checks agree, and they rule out the obvious explanation.

**The elision is applied there.** The per-variant instrumentation in `gfx_opengl.cpp:731` logs
whether each compiled shader's final wrap was proved redundant. Hyrule Field compiles 26 variants
and elides the wrap in **25 of them** (96%), 17 of those two-cycle. The Market compiles 23 and
elides **all 23**, 14 two-cycle. Coverage is not the difference.

**Fragment work in general is free there.** Three-point filtering is pure fragment ALU — three
samples and their weights, computed per pixel — and §8.15 measured it at 3.6% where it bites. At
the Hyrule Field fixture, switching it on costs **+33 µs, +0.11%** (n=5, t=0.34, resolves 0.9%).
A frame that does not notice three extra texture samples per pixel will not notice one `mod()`
removed.

So roughly 30 ms of that frame is spent somewhere other than the fragment shader: geometry through
the tiler, per-draw and display-list cost on the CPU, or bandwidth. Which of those is unknown and
is now the most valuable open question in this document, because Hyrule Field is where the game is
slowest *and* where every remaining shader-side item in this catalogue — items 3, 5, 6 and the
whole of §8.6's shader queue — is aimed at a bottleneck that is not there.

### What this changes about how items get ranked

Every percentage in §8.10 through §8.17 was taken at one fixture, and §8.14 already warned that
changing the viewpoint invalidates comparisons across it. This is worse than that: it means a
single-fixture measurement does not estimate a change's value with error bars, it samples one
point from a distribution that spans zero to −3.6%. The rig's 0.5% resolution was never the
limiting factor in knowing what a change is worth — scene choice was, by a factor of seven, and no
amount of ABBA pairs at one spot would have revealed it.

Shader-side changes from here run against Market and Hyrule Field at minimum. They bracket the
answer: one frame where fragment cost dominates, one where it is free.

## 8.19 The frame cap was on all along — and Hyrule Field is the light scene

Measured 2026-09-23, evening. Started as §8.18's open question — what Hyrule Field's ~30 ms is
spent on — and ended by showing the ~30 ms was never there.

### The harness never uncapped

`Benchmark_FrameEnd` set the window's target FPS to 240 once, "so the limiter never sleeps".
`Graph_ProcessGfxCommands` calls `wnd->SetTargetFps(InterpolationFPS)` on **every game tick**
(`OTRGlobals.cpp:1837`), so from the next tick on `SyncFramerateWithTime` paced every run at
30 FPS — and it sleeps inside `SDL_GL_SwapWindow`'s caller, i.e. inside the timed window. With
`LUS_FPS_LOG=5` the game's own counter read **30.00 FPS** at Hyrule Field throughout every
measured segment, while the harness reported 30.7 ms / "32.5 FPS".

So under the cap the harness measured **33.3 ms minus the untimed work** (`Play_Draw` and the
rest of the tick, which sit outside `DrawAndRunGraphicsCommands`). A scene whose real work fits
under 33.3 ms returns that number regardless of what is changed. That is exactly what Hyrule Field
did, four times, before the cause was found:

| capped probe at Hyrule Field | change in harness frame time |
|---|---|
| internal resolution 0.5 → 0.25 (4× fewer pixels) | −0.10%, t=−0.77, MDE 0.32% |
| CPU pinned 1.25 → 1.0 GHz | −0.8% (the wrong direction) |
| 1280×720 fullscreen → 640×360 window (4× fewer presented pixels) | +0.2% |
| `glFinish` before the swap | GPU "done" in 0.16 ms, swap still 17.5 ms |

The last row is what gave it away: 17.5 ms blocked in the swap with the GPU reporting idle is a
sleep, not work. Fixed in `benchmark.cpp` (re-assert the target at every frame start,
`SOH_BENCH_UNCAP`, default on) and validated: the new `period_us` field — start of one frame to
start of the next — matches `LUS_FPS_LOG` to within 0.1 FPS in capped and uncapped runs alike.

### What this retracts

- **§8.18's Hyrule Field row and its conclusion.** "Hyrule Field is not fragment-bound" rested on
  two capped nulls (three-point filtering +0.11%, `ShaderOpts` −0.07%). Both are the cap absorbing
  the change. The statement is **unsupported**, not disproven — neither was re-run uncapped.
- **§8.18's Market and Kokiri effect sizes are real but distorted.** Those scenes overran the cap
  on some frames, so a change moved the mean, but the size mixes real savings with fewer overruns.
  Direction stands; magnitude needs re-measuring uncapped before anything is ranked on it.
- **Every absolute "FPS" the harness has printed** (`mean_fps`) is 1/timed-window: capped, and
  missing the untimed work. That includes the 33.9 / 34.15 / 34.99 FPS that `NEXT.md` §4 rests the
  `InterpolationFPS` decision on.
- The comment at `OTRGlobals.cpp:297` ("strongly fragment-bound", "+75%") describes report 7's
  1.0×→0.5× step. At the shipped 0.5× it is not true of either fixture (below).

### Uncapped, the picture reverses

Frozen fixtures, CPU pinned at 1.25 GHz, soaked to 70 °C (76 °C was not reachable this evening —
TESTING.md trap 9):

| | frame period | fps | timed window | of which swap | outside window | draws | tris |
|---|---|---|---|---|---|---|---|
| Hyrule Field | **21.1 ms** | **47.4** | 18.4 ms | 5.0 ms | 2.7 ms | 167 | 776 |
| Market | **31.5 ms** | **31.7** | 25.9 ms | 5.1 ms | 5.7 ms | 261 | 1670 |

**Hyrule Field is the light scene**, with headroom to ~47 FPS. The Market is the one near the
30 FPS line. No texture uploads, framebuffer copies, readbacks or reallocations at either fixture
(the new `BENCH work` line); two framebuffer binds and two clears per frame.

### Internal resolution, uncapped — the probe §8.18 asked for

`gSettings.InternalResolution` 0.5 → 0.25, six ABBA pairs each; `fb=` confirms every segment ran at
640×360 or 320×180 as intended.

| | Δ frame period | t | MDE | swap 0.5× → 0.25× |
|---|---|---|---|---|
| Hyrule Field | **−1319 µs (−6.3%)** | −15.0 | 226 µs | 5.03 → 3.82 ms |
| Market | **−1561 µs (−4.9%)** | −38.1 | 105 µs | 5.07 → 3.73 ms |

**Neither scene is fill-bound at 640×360**: four times fewer pixels buys 5–6%. Report 7's
0.5→0.25 figure (+31%) does not survive. Nearly all of the saving lands inside the swap, which
means the swap contains the tile-based deferred renderer's fragment pass: **`glFinish` on this
DDK does not wait for it** (0.14 ms at both resolutions while the swap moved 1.2 ms), so the
earlier capped reading of `finish_us` as "GPU idle" is withdrawn too. Extrapolating the pixel
dependence, roughly 3.4 ms of the 5 ms swap does not scale with pixels — reasoning, not
measured; it is the part a flip-vs-copy investigation (playbook `improvements.md` §6) would target.

### CPU clock, uncapped — tentative

| | 1.25 GHz | 1.0 GHz | 1.25 GHz |
|---|---|---|---|
| Hyrule Field period | 21.16 ms | 21.67 ms (+2.4%) | 24.34 ms — outlier |
| Market period | 31.93 ms | 32.30 ms | 31.61 ms |

A 20% slower CPU costs ~2%, so the ~13 ms of timed work outside the swap is not scaling with core
clock. The third Field run is 15% slower at the same pin with the die cooling; nothing in the
thermal service's journal explains it, and the harness does not record the effective CPU
frequency, so the Field pair is **not trusted** until that is logged. The Market bracket is
clean. If it holds, that 13 ms is waiting — on the driver, on the tiler, or on memory, none of
which follow the core clock — rather than computing, which would make per-draw state and
submission shape the next thing to measure, not shader arithmetic.

### What it changes

The question "what is Hyrule Field's 30 ms spent on" is closed: it was the cap. The live
questions are the Market's 31.5 ms (the scene actually near 30 FPS), what the ~13 ms of
non-swap timed work is waiting on, and the ~3.4 ms of the swap that does not scale with pixels.
Every shader-side ranking in this document was built on capped numbers and should be re-measured
uncapped before it is trusted.

## 8.20 `ShaderOpts` uncapped: −4.9% at the Market, −4.6% at Hyrule Field — and the pin leaked

Measured 2026-09-24, just after midnight. §8.19 left the shipped shader change (`gSettings.ShaderOpts`
0 → 3, items 5 + 6) standing on capped numbers. This re-measures it uncapped, after first making the
harness record the CPU clock, which turned out to be the more important half of the session.

### The CPU "pin" was not a pin

`benchmark.cpp` now reads cpu0's `scaling_cur_freq` at each segment's first and last measured frame
and every 30 frames between, and prints `cpu_mhz=start->end cpu_min_mhz= cpu_max_mhz=`. Checked
before use: 1250 under the default pin, 1000 under `PYRA_PIN_FREQ=1000000`.

The first Market A/B then showed segments 0 and 1 at **1500 MHz** under a "1.25 GHz pin". The cause
is `pyra-thermal.service`: it writes `scaling_max_freq` every **2 s**, to 1.5 GHz whenever the
hotter of `core_thermal`/`cpu_thermal` is below its 77 °C rung (and only gives a step back 5 °C
below the rung that took it), to 1.0 GHz above 82 °C, and lower still on the fuel-gauge die.
`bench.sh` pinned by setting `min == max == 1.25 GHz`, so within two seconds the daemon had widened
it to 1.25–1.5 GHz and ondemand took the top. The pin held only while the die was hot enough that
the daemon's own cap happened to equal it.

Fixed in `bench.sh`: the pin is now the userspace governor with `scaling_setspeed`. A daemon ceiling
above it is inert (verified at 73 °C: the daemon wrote 1.5 GHz throughout, the clock held 1250 in
all 17 samples); a thermal cap below it still bites, which is correct — the daemon stays running as
the thermal backstop and the charger's current limit — and shows up in `cpu_min_mhz`. TESTING.md
trap 12.

It bit in the other direction within the hour. At Hyrule Field the die sits at 80–81 °C on the
harness sensor, just under the daemon's 82 °C rung, and one run had segments 5–7 capped to
**1000 MHz**. Those two pairs were discarded and the fixture re-run; the re-run stayed at 1250.

What this does to earlier results: every pinned run below ~77 °C before today may have run partly at
1.5 GHz. That includes §8.19's CPU-clock bracket — the 1.0 GHz arm could have been lifted too, and
the 15%-slow Hyrule Field run is unexplained either way. **§8.19's "a 20% slower CPU costs ~2%" is
unverified** until re-taken on the new pin. Soaked A/Bs near 80 °C (§8.17, §8.18) were probably
genuinely at 1.25 GHz, since the daemon's cap sat there too — probably, not provably.

### Result

Frozen fixtures, 6 ABBA pairs each, uncapped (`uncap=1`), CPU 1250 MHz by all four fields in every
segment reported below, soaked to 70 °C (reached within the first second — the device was already
warm; segments ran 73–80 °C at the Market and 78–81 °C at Hyrule Field). `delta` is 3 minus 0.

| | Δ frame period | t | MDE | Δ timed window | t | MDE |
|---|---|---|---|---|---|---|
| Market | **−1622 µs (−4.85%)** | −40.1 | 104 µs (0.31%) | −1553 µs (−5.69%) | −33.8 | 118 µs |
| Hyrule Field | **−1025 µs (−4.55%)** | −13.3 | 198 µs (0.88%) | −1033 µs (−5.25%) | −23.0 | 116 µs |

| per arm (ms) | period | fps | timed window | swap | window − swap | outside window |
|---|---|---|---|---|---|---|
| Market, 0 | 33.44 | 29.90 | 27.29 | 6.42 | 20.87 | 6.15 |
| Market, 3 | 31.82 | 31.42 | 25.74 | 5.05 | 20.69 | 6.08 |
| Hyrule Field, 0 | 22.50 | 44.45 | 19.67 | 5.86 | 13.82 | 2.83 |
| Hyrule Field, 3 | 21.47 | 46.58 | 18.64 | 4.97 | 13.67 | 2.83 |

Replications, not used in the table: the first Market run (old pin, pairs 1–5 after discarding the
1500 MHz pair) gave −1653 µs period, t=−18.9; the first Hyrule Field run's four clean pairs gave
−1019 µs, t=−17.9. Both agree with the table within its MDE.

### Against the capped numbers

- **Market: larger than shipped on.** Capped it was −1168 µs / −3.59% of the timed window;
  uncapped it is −1553 µs / −5.69% of the same window and −4.85% of the real frame period. The cap
  was absorbing about a quarter of the saving.
- **Hyrule Field: the capped null was entirely the cap.** Capped −20.5 µs (−0.07%); uncapped
  −1025 µs (−4.55%), t=−13.3. The same order in absolute terms as the Market. §8.18's "worth
  −3.59%, −2.40% and nothing, depending on the scene" does not survive: in the two scenes
  re-measured it is worth about 1–1.6 ms and 4.5–5% of the frame in both. Kokiri was not
  re-measured.
- At the Market the change is what takes the frozen fixture over 30 FPS: 29.90 → 31.42.

### Where the saving lands

About 87% of it is inside `SDL_GL_SwapWindow`: swap falls 1.37 ms at the Market and 0.89 ms at
Hyrule Field, against 1.55 / 1.03 ms of timed window. The non-swap timed work moves by 0.15–0.19
ms and the untimed work not at all. That is what a cheaper fragment shader should look like on this
tile-based deferred renderer, where the fragment pass runs at the flush the swap forces (§8.19:
`glFinish` does not wait for it). It also means the fragment pass is not the small share §8.19's
"not fill-bound" might suggest: at `ShaderOpts` 0 the swap was 6.4 ms, not 5.

### An observation, not yet a finding

At the Market, arm 0's frame period rose steadily across the run — 32.80 → 34.12 ms (+4.0%) over
about five minutes at a fixed 1250 MHz, the swap flat (6.45 → 6.36 ms), the die 73 → 79 °C. The
first Market run did the same (33.84 → 34.26 over its clean pairs). Most of the growth is in the
untimed work (5.67 → 6.55 ms) and the rest in the non-swap window. Hyrule Field's arm 0 barely moved
(22.44 → 22.49). ABBA cancels a linear drift, so the deltas above stand; but something in the
Market's CPU-side work gets slower over a run with the CPU clock held, and heat, memory or
accumulating state are all still possible. It belongs to the next question.

### What it changes

- Item A (NEXT.md) is re-stated on these numbers. It stays shipped.
- Every ranking that used "−3.59% at best, nothing at Hyrule Field" as the scale for a
  shader-side item was using a number about a quarter low at the Market (−1168 against −1553 µs) and wrong at Hyrule Field. That
  includes the 3.6% item 3's ceiling is a share of (§8.15), which was also capped.
- The ~13 ms of non-swap timed work NEXT.md asks about is **Hyrule Field's** (13.7 ms at arm 3).
  The Market's is **20.7 ms**. The next profile should use the Market, where the frame is closest to
  30 FPS and the non-swap share is largest.

## 8.21 The 65% is computing, not waiting — and a tenth of the render thread is resource lookups

Measured 2026-09-24, 01:05–01:55 device time. NEXT.md §2a asked whether the Market's 20.7 ms of
non-swap timed work is the render thread running or blocked, and said to settle that before any
profile was worth reading. This is P1, P2, a failed P3 and P4 from that ladder.

All runs: Market fixture, `ShaderOpts` 3 (shipped default), uncapped, CPU 1250 MHz by all four
`cpu_*` fields in every segment, soaked to 70 °C (segments 70–77 °C). Seventeen segments across
three runs; the repeated segments came from `bench.sh ab gSettings.ShaderOpts 3 3`, because
`bench.sh run` measures exactly one segment whatever `SOH_BENCH_REPS` says (TESTING.md corrected).

### P1 — on-CPU time (new, permanent)

`benchmark.cpp` now prints a `BENCH oncpu` line per segment: the render thread's
`CLOCK_THREAD_CPUTIME_ID` over wall time for the whole frame period and inside the timed window,
and the process total. Two extra syscalls per frame, a few µs; not priced with an A/B.

| | range over 17 segments |
|---|---|
| render thread on-CPU, share of frame period | **75.3–77.7%** (≈ 23.7–24.5 ms of 31.3–32.6 ms) |
| render thread on-CPU inside timed window | 73.1–75.0% (≈ 18.7–19.2 ms of 25.3–25.9 ms) |
| whole process | 80.0–82.5% of one core |

`perf stat --per-thread` on the same process agrees independently: task-clock 15,398 ms in 20.0 s,
0.770 CPUs, for the render thread.

Off-CPU time inside the window is therefore ~6.8 ms per frame, and the swap alone is 5.0 ms. If the
swap is mostly blocked — which is what a swap on this stack should be — the **non-swap 20.7 ms is
roughly 90% on-CPU**. The 65% is computing. Outside the window (≈ 6.1 ms) the thread is ~86%
on-CPU. **The "is it GPU-side waiting" hypothesis that §2a and the GPU-OPP item hung on is
refuted for the large block**: it can explain at most ~2 ms of it.

Two more numbers from `perf stat`: the render thread runs at **0.52 instructions per cycle**
(10.0 G instructions, 19.2 G cycles in 20 s) — low for a Cortex-A15, which suggests memory, branch
or barrier stalls rather than arithmetic — and it context-switches **~27 times per frame**
(16,962 in 20 s). The audio thread, by contrast, runs at 1.39 IPC.

### P2 — ioctls per frame

NEXT.md's recipe named `/dev/pvrsrvkm`; **that node does not exist on this device**. The 1.14 DDK
submits through DRM: the process holds `/dev/dri/renderD130` (fds 9 and 11) and `/dev/dri/card0`.
`perf trace` is not needed and `strace` is not installed; `raw_syscalls:sys_enter` filtered on
`id == 54` (ARM EABI `ioctl`) works without either.

**23,588 ioctls in 10 s ≈ 75 per frame**, every one the same request, `0x401c6440` — DRM
driver-private command 0x40 with a 28-byte argument, i.e. the PVR services bridge entry point.
69/frame on fd 9, 7/frame on fd 11. That is neither one per draw (261) nor the two or three a
fully batching driver would issue. The bridge function id is inside the argument struct, so this
trace cannot say which calls the 75 are; the kernel profile below names some of them.

### P3 — not answered, and the method was wrong

`perf sched record -p <pid>` followed by `timehist` credited the render thread with 16.2 s of run
time in a 15 s window and 0.38 s of waiting — impossible next to P1's 77%. Task-filtered recording
loses the switch events needed to see the thread go idle. P3 needs `perf sched record -a`. It is
also no longer the priority, since P1 says the block is running.

### P4 — where the on-CPU time goes

`perf record -F 499`, whole process, 25 s, then `-F 149 -t <render tid>`, 12 s, twice (one run
with a `-g1` build). DSO split for the render thread:

| DSO | share |
|---|---|
| `soh.elf` | 53.5% |
| `libc.so.6` | 15.7% |
| `libGLESv2` (DDK) | 11.9% |
| kernel | 7.5% |
| `libsrv_um` (DDK services) | 4.7% |
| `pvrsrvkm` (DDK kernel module) | 3.1% |
| `libstdc++` | 2.8% |

Self time by leaf function, grouped (render thread, 1,496 samples, `-g1` build; the groups are
regex over symbol names, so treat edges as ±1 point):

| group | self time |
|---|---|
| interpreter: `GfxSpVertex`, `GfxSpTri1`, tri handlers, matrix | 12.0% |
| interpreter: everything else (`Run`, RDP handlers, …) | 12.2% |
| **resource lookup** — `ResourceManager`, `ArchiveManager::HashToCString`, `Context::GetInstance`, the `ResourceIdentifier` hash table | **10.7%** |
| PVR user-space mutex (`PVRSRVLockMutex`/`Unlock`, `pthread_mutex_lock`) | 6.0% |
| malloc / free | 5.1% |
| `shared_ptr` refcount (`_M_release`, `_M_add_ref_copy`) | 4.5% (8.0% in the non-`-g1` run) |
| memcmp / memset / `_Hash_bytes` / string | 4.1% |
| frame interpolation (`interpolate_branch`, its `_Rb_tree`) | 3.2% |
| CVar lookup by name | 1.4% |
| unresolved (stripped DDK and libc addresses) | 21.3% |

**The resource-lookup cluster is the finding.** It is one call site doing avoidable work every frame
in a frozen scene: `gfx_set_timg_otr_hash_handler_custom` (`interpreter.cpp:3453`), per texture
load, calls `Context::GetInstance()` three times, `GetResourceManager()` three times and
`GetArchiveManager()` twice — every one returns a `shared_ptr` **by value**, an atomic increment
and decrement with barriers on ARMv7 — calls `HashToCString` twice, builds a `std::string` and a
`ResourceIdentifier` from it, and with alt assets enabled concatenates `"alt/" + path` and makes a
second, failing hash-table lookup before the real one (`ResourceManager.cpp:96-121`). The comment
at the call site says upstream disabled the cache that used to short-circuit this and that "we need
to do proper profiling to see whether or not it is worth it". Lookup + refcount + a share of the
malloc/free and hashing it causes is in the region of **10–15% of the render thread's CPU time**,
≈ 2.5–3.5 ms of the Market frame. That is an upper-bound-ish estimate from self time, not a
measured saving; the A/B is the measurement.

Kernel-side, two things that should not be in a steady frame:

- **The driver allocates and frees GPU memory every frame.** `PVRSRVAllocDeviceMemBW` →
  `BM_Alloc` → `RA_Alloc`, plus the matching `FreeDeviceMem` and `mmap`/`munmap`: ~2.5–3.5% of the
  render thread. `fb_reallocs=0.00`, so it is not framebuffer storage. Unattributed; per-draw
  `glBufferData` orphaning is the obvious suspect and is item G's territory.
- **The SGX goes through a power-state transition inside the frame**: `PVRSRVSetDevicePowerStateKM`
  → `SGXPostPowerState` → `SGXInitialise`, ~0.9%. The GPU is idling off between frames and being
  re-initialised. The CPU cost is small; the latency it adds to the GPU side is unmeasured.

### What did not work: call graphs

Neither frame-pointer nor DWARF unwinding produces usable user-space call graphs here. The shipping
build has no `.debug_frame` (ARM uses `.ARM.exidx`, which perf's unwinder ignores). A `-g1` build
adds it (222 MB binary, identical timings — 31.7–32.6 ms against 31.3–32.1), but the unwinder still
stops at the stripped DDK and, inside `soh.elf`, invents callers: it reported `Interpreter::Run`
and the RDP handlers as called *by* `UpdateFramebufferParameters`. A lead built on that, "3.2% in
`StartFrame` → `UpdateFramebufferParameters`", was checked against the stacks and retracted. Kernel
chains are fine. **Price call sites with in-frame counters and A/Bs (P5/P6), not call graphs.**
Also: on-device `perf report` with inline resolution over the `-g1` binary ran for over half an
hour in `addr2line`; pass `--no-inline`.

Unverified: a `std::filesystem::filesystem_error` constructor appeared as a leaf in 0.27% of
samples. Probably a hidden `libstdc++` function resolved to the nearest exported symbol; if real, it
is an exception constructed every frame. Not chased.

### What it changes

- **§2a's premise is inverted.** The large block is on-CPU work in `soh.elf`, libc and the GL
  driver's CPU side, not the GPU. The GPU-OPP item drops further (NEXT.md §4); "is the frame
  pipelined with the GPU" (NEXT.md item 5) now concerns only the ~7 ms off-CPU share.
- A new first item: **the texture-by-hash lookup path.** Measure it with an A/B, not the profile.
- Item G (one `glBufferData` per frame) gains a mechanism to look for — the per-frame device-memory
  allocation — and should count allocations, not only time.
- `libPVRScopeServices.so` is present in the DDK. The playbook's "no GPU counters on this part"
  may be wrong; it has not been tried.

## 8.22 Caching asset lookups: −12% of the Market frame, −15% at Hyrule Field

Measured 2026-09-24, 02:30–04:10 device time. §8.21 found ~10–15% of the render thread's CPU in
resource-manager lookups. This counts them, then removes them.

### How many, and from where

New `BENCH work` fields (per frame, identical in every segment of a fixture — the frozen scene
issues the same display lists every frame):

| per frame | Market | Hyrule Field |
|---|---|---|
| `G_SETTIMG_OTR_HASH` (texture by hash) | 430 | 287 |
| `G_VTX_OTR_HASH` (vertices by hash) | 493 | 298 |
| `G_DL_OTR_HASH` (display list by hash) | 19 | 0 |
| `G_SETTIMG` on an `__OTR__` path string | 168 | 192 |
| `*_OTR_FILEPATH` commands | 0 | 0 |
| `ResourceManager::LoadResourceProcess` entries, whole period | **1364** | **1150** |
| … of which inside the timed window | 1364 | 1150 |

Every load is inside the timed window: the game side makes none in a frozen frame, so the lookups
are all the interpreter's. Each texture-by-hash command enters `LoadResourceProcess` twice (the
failed `"alt/"` attempt, then the real one — `AltAssets` defaults to on and the benchmark install
has no mods); each `__OTR__` path enters three times (the prefix strip first). 430 × 2 + 168 × 3 =
1364 exactly. Vertex and display-list lookups go through `LoadResource(crc)` and do not reach
`LoadResourceProcess` at all, so they are not in that count, but each is still a `HashToString`, a
string, a mutex and a hash-table find.

### The change

`gSettings.LeanResLookup`, in libultraship, read once per frame in `Interpreter::StartFrame`:

- **1** — the texture-by-hash handler fetches `Context`/`ResourceManager`/`ArchiveManager` once and
  calls `HashToCString` once (it did eight `shared_ptr`-by-value getter calls and two).
- **2** — texture, vertex, display-list and `G_BRANCH_Z_OTR` hash lookups answered from a per-hash
  cache in front of the resource manager.
- **3** — 2, plus `G_SETTIMG` `__OTR__` path lookups cached by the string's address, and verified by
  comparing the whole string, so a reused buffer holding a different path cannot hit.

The cache must return exactly what the manager would. `ResourceManager` and `ArchiveManager` gain
generation counters: the first is bumped on every write to the resource cache (the three inserts
and the one erase in `ResourceManager.cpp`) and on an alt-asset toggle; the second on every change
to the hash → path table (`AddArchive`, `WriteFile`, `ResetVirtualFileSystem`). The interpreter's
cache flushes whenever either moves, so an entry is only ever served while the manager's state is
identical to when it was filled. Generations are read before the manager is consulted, so a
worker-thread insert racing a fill leaves the cache tagged stale and the next call flushes it. In a
steady frame nothing is inserted and the cache stays hot; a scene load refills it once.

Upstream history this is not a repeat of: libultraship #989 ("Improve ResourceManager cache lookup
performance") changed the manager's internals and was reverted in #1028 for lag spikes with a
texture pack (#1025), cause never identified. This change leaves the manager's lookup path alone
and sits in front of it. **Its risk case is the same one, though**: a texture pack that streams
assets in during play bumps the generation on every insert, and a cache flushed every frame is
upstream's cost plus a refill. Not tested — there is no pack on the device.

### Results

All A/Bs: 6 ABBA pairs, uncapped, soaked to 70 °C, `delta` is B minus A.

| A → B | fixture | clock | Δ frame period | t | MDE | Δ timed window | t |
|---|---|---|---|---|---|---|---|
| 0 → 1 | Market | 1250 all 12 | −406 µs (−1.3%) | −2.49 | 420 | −404 µs (−1.6%) | −4.87 |
| 0 → 2 | Market | 1250 all 12 | **−2770 µs (−8.9%)** | −44.8 | 159 | −2771 µs (−10.9%) | −56.6 |
| 0 → 2 | Hyrule Field | 1250 all 12 | **−2048 µs (−9.8%)** | −43.8 | 120 | −2100 µs (−11.5%) | −57.6 |
| 2 → 3 | Market | 1250 all 12 | −630 µs (−2.3%) | −7.8 | 208 | −559 µs | −11.1 |
| 2 → 3 | Hyrule Field | **1000 all 12** | −1026 µs | −9.9 | 266 | −1104 µs | −17.1 |
| 0 → 3 | Market | 1250 in pairs 0, 1, 5 only | **−3787 µs (−12.1%)** | ≈ −123, n=3 | ≈ 132 | | |
| 0 → 3 | Hyrule Field | **1000 all 12** | −3549 µs (−14.6%) | −42.9 | 213 | −3458 µs | −57.3 |

Discards, stated: the 0 → 3 Market run had segments 4–9 capped to 1000 MHz by `pyra-thermal` (the
die at 81 °C after back-to-back runs), so only its three pairs with both halves at 1250 are used;
all six give −3931 µs, t=−7.4, and are not quoted. Both Hyrule Field runs after the Market ran
every segment at 1000 MHz. Their deltas are internally consistent — both arms were at the same
clock — but they are not at the rig's standard clock and are labelled so.

Absolutes at the Market, 1250 MHz: **31.3 → 27.5 ms, 31.9 → 36.3 FPS** with mode 3. The render
thread's on-CPU share falls 76% → 72.5%; the swap does not move (5.1–5.3 ms), as it should not for
a change that draws the same thing. 0 → 2 → 3 steps (−2770, −630) sum to −3400 against −3787
measured directly, in different runs at different temperatures — consistent, not exact.

This is the largest single change in the project so far: about 2.3× the `ShaderOpts` win at the
Market (−3787 against −1622 µs, §8.20), and it lands where §8.21 said the frame is actually spent.

### Visual check, and a flaw in the visual check

Market, mode 2, `shotcheck.sh`: **PASS**, 12,882 stable pixels against a control of 20,992;
the difference is a plant and two NPCs placed differently per launch, textures identical.

Hyrule Field, mode 3, `shotcheck.sh`: **DIFFER**, 8,617 against a control of 1,831, concentrated on
Link. Examined: Link's textures are identical, his *pose* differs — the launch froze him at a
different point of his idle. Re-checked **inside one process**: an A/B with `SOH_BENCH_FRAMES=2400`
and frames grabbed with `xwd` during each segment, so the frozen pose cannot differ. Link's region
differs in 0 pixels between arms. What remains is a band of sky/horizon pixels (y ≤ 272, ≤ 22/255)
that differs as much between two grabs of the *same* arm 150 s apart (596 pixels) as between arms
(556 and 254) — slow drift in the sky, not the change. **Mode 3 renders identically.**

The flaw: `shotcheck.sh`'s control is launch-to-launch, and a launch that settles faster or slower
freezes animated actors at a different phase. A change that alters frame time alters the pose, so
for exactly the changes worth making, the three-launch check can fail on geometry that is not
wrong. The in-process grab is the better test for anything that changes speed. TESTING.md has the
recipe.

### What is not yet known

- **Real play.** The fixture never loads a scene mid-run, which is exactly when the generations
  move and the cache refills. The invalidation logic has been reasoned about and not exercised.
  Needs play across scene transitions, pause/equipment menus (which draw by path), the alt-asset
  toggle, and ideally a texture pack.
- Hyrule Field at 1250 MHz for modes 2 → 3 and 0 → 3.
- Kokiri, and anything with pre-rendered backgrounds (`Gfxs2dexBg*` have their own path lookups,
  not cached).

## 8.23 The lookup cache in real play: −11.7% of the render thread, zero flushes, and the new default

Measured 2026-09-24, 04:18–04:36 device time, with the device's owner playing. §8.22 measured the
cache on a frozen frame, which never loads anything. The question left was whether real play —
moving, loading new assets, changing scenes — would keep flushing it.

### A narrower invalidation rule first

§8.22 flushed the whole cache on *any* insert into the resource manager. SoH loads assets lazily, the
first time something references them, so walking into view of a new model would have flushed it.
That rule was stricter than correctness needs: once a lookup has filled the cache, the manager
already holds entries for both `path` and `alt/path`, and a `NotFound` cached for `alt/path` is never
overwritten (the lookup returns early). Only an erase, the alt-asset toggle, or a newly loaded `alt/`
asset — which overrides its base — can change a cached answer. The generation now moves on those
alone (libultraship `2170510b`). A texture pack streaming `alt/` assets would still flush, once per
new pack asset rather than once per frame.

### A real-play A/B

Watch mode now takes `SOH_BENCH_AB_CVAR`: the game runs normally — capped, game logic live —
while the arms alternate in ABBA blocks of 300 frames, and each block logs render-thread CPU time,
the timed window and the cache's hits, misses and flushes. Under the frame cap the period is the
cap, so the metric is **render-thread CPU per frame**, which measures work whatever the limiter
does with the time left over. CPU pinned at 1250 MHz (`PYRA_WATCH_PIN=1`); under ondemand the
lighter arm would get a lower clock and more CPU time per frame.

The owner played for ~17 minutes from the Market through Hyrule Field and on through nine scenes
(32, 81, 99, 82, 62, 80, 96, 1, 18), including scene transitions and interiors. 101 blocks, 1250 MHz
in every one, 69–80 °C.

| | |
|---|---|
| cache flushes, all 50 mode-3 blocks | **0** |
| hit rate | **99.97%** — ~418 hits/frame, misses 0.14/frame mean, 0.54 max |

| 50 ABBA pairs, 0 → 3 | Δ | t | ≈ MDE |
|---|---|---|---|
| render-thread CPU per frame | **−2145 µs (−11.7%)** | −6.0 | 720 µs |
| … inside the timed window | −2283 µs (−16.9%) | −7.4 | 620 µs |
| frame period | −229 µs (−0.6%) | −2.6 | 178 µs |

(MDE here is 2·se at df=49. The pair sd is large, 2.5 ms, because every block is different play; the
effect is still six standard errors out.)

The period barely moves because it is the cap: under 30 FPS play the saving is headroom, not frame
rate. The one place the headroom shows is how often a block ran slower than the cap on average
(period > 34 ms): 9 blocks of 50 with the cache off, 5 with it on — suggestive, not a measurement.

By scene, crude (each block is a different stretch of play): the Market −16% (25.4 → 21.4 ms),
Hyrule Field −13% (16.3 → 14.2), scene 1 −19%, 82 −17%, 96 −20%, 80 −18%, 18 −10%, and scene 99
only −2% on 4 and 6 blocks. The window saving in play (−16.9%) matches the frozen Market's
(18.8 → 15.0 ms of window CPU, about −20%).

**Visual:** the owner saw no graphical change at any point, with the cache toggling every ~10
seconds — a stale or wrong texture would have appeared and corrected itself on that rhythm. Together
with §8.22's in-process check, that is the evidence the default rests on.

### Decision

**`gSettings.LeanResLookup` now defaults to 3.** Setting it to 0 restores upstream behaviour. No
archive change goes with it — it is code only, unlike `ShaderOpts`.

Still untested: a texture pack (upstream #989's failure case). With the narrowed rule the expected
cost is one flush per newly loaded pack asset, which should be rare after the first minutes in a
scene — but "should" is reasoning, and the watch-block log would show it in one session with a pack.

Consequence for the rig: **every baseline taken from now on includes the cache.** A frozen Market
`bench.sh run` should now read ~27.5 ms, not ~31.4 ms, and `res_loads` should be 0.

## 8.24 Client-side vertex arrays: −13.4% of the Market frame, −7.3% at Hyrule Field — and GL calls are not what costs

Measured 2026-09-24, 04:44–06:00 device time. The session PROMPT.md briefed: the Hyrule Field
number §8.22 owed, a fresh profile of the frame the lookup cache left, and one probe from it.

### Hyrule Field, LeanResLookup 0 → 3 at 1250 MHz (owed from §8.22)

Run first, on a device at 60 °C; all 12 segments at 1250 MHz by all four `cpu_*` fields, 71–81 °C.

| 6 ABBA pairs | Δ | t | MDE |
|---|---|---|---|
| frame period | **−3203 µs (−15.1%)**, 21.20 → 17.99 ms, 47.2 → 55.6 FPS | −51.7 | 159 |
| timed window | −3186 µs (−17.5%) | −77.6 | 106 |

`res_loads` 1150 → 0, `lookup_hit` 479/frame, `lookup_flush` 0. The 1000 MHz figure in §8.22
(−14.6%) stands; at the standard clock it is −15.1%.

### The re-profile, cache on

Method as §8.21: `bench.sh ab gSettings.ShaderOpts 3 3 $MARKET SOH_BENCH_REPS=5`, ten identical
segments, all at 1250 MHz, 76–82 °C. `perf stat --per-thread` over ~segment 3, `perf record -F 499
-t <render tid>` for 25 s over ~segments 4–5 (10,352 samples, against §8.21's 1,496), reports after
exit. The device script is [`tools/prof.sh`](tools/prof.sh); the grouping is now
[`tools/perfgroup.py`](tools/perfgroup.py) so the next profile is grouped the same way. §8.21's
regexes were not saved, so the rows below were re-derived from its row names: edges ±1 point.

Frame: 28.0–28.6 ms in the segments before `perf` attached (§8.23 said ~27.5; the next run on the
same binary read 27.4–27.8, so this is run-to-run and temperature, not the new counters). Render
thread **0.742 CPUs, IPC 0.50, ~27 context switches per frame** — against 0.770, 0.52 and 27 in
§8.21. The cache took work out; it did not change the thread's character.

| DSO | §8.21 | now |
|---|---|---|
| `soh.elf` | 53.5% | **47.0%** |
| `libc.so.6` | 15.7% | 17.0% |
| `libGLESv2` | 11.9% | **13.9%** |
| kernel | 7.5% | 7.8% |
| `libsrv_um` | 4.7% | **6.7%** |
| `pvrsrvkm` | 3.1% | 3.1% |
| `libstdc++` | 2.8% | 1.7% |

| group (self time, render thread) | §8.21 | now |
|---|---|---|
| interpreter: `GfxSpVertex`, `GfxSpTri1`, tri handlers, matrix | 12.0% | **16.2%** |
| interpreter: everything else | 12.2% | 10.2% (+0.9% GL backend, counted separately now) |
| resource lookup | 10.7% | **5.6%** |
| PVR user-space mutex | 6.0% | 6.3% |
| malloc / free | 5.1% | 4.9% |
| frame interpolation | 3.2% | 4.3% |
| memcmp / memset / hash / string | 4.1% | 3.9% |
| `shared_ptr` refcount | 4.5% (8.0%) | 2.1% |
| CVar lookup by name | 1.4% | 0.9% |
| unresolved (stripped DDK / libc) | 21.3% | 25.1% |

What changed rank: the lookup cluster halved and fell from second to fifth; refcounting halved
with it. The interpreter's geometry (`GfxSpVertex` 6.3%, `GfxSpTri1` 6.4%) is now the largest named
item. The **driver's CPU side** — `libGLESv2` + `libsrv_um` + `pvrsrvkm` by DSO, which already
includes their unresolved addresses — rose from 19.7% to **23.7%**, plus part of the kernel's
7.8%; by DSO it is the largest block after `soh.elf` itself. (Most of the "unresolved" row is
`libGLESv2`: 13.0 of its 13.9% has no symbol.)

The 5.6% of lookup that is left is not the interpreter: `ResourceMgr_FileAltExists`,
`ResourceMgr_GetResourceByNameHandlingMQ`, `ResourceMgr_LoadGfxByName`, `ResourceMgr_OTRSigCheck`,
`LoadResourceAsync` (with a `std::promise` allocated per call) — the game's own C code resolving
assets by name, answered from the manager's cache (`res_loads` is 0, so none reaches
`LoadResourceProcess`). About 2.5%; not chased.

### The probe: GL calls per frame

The PVR mutex (6.3%) is taken per GL call, so the prompt's probe was to count GL calls. New
counters (`BENCH work` `gl_calls`, `gl_uniforms`, `vbo_uploads`, `vbo_kb`, and a `BENCH glfn` line
per entry point; macros over every GL entry point `gfx_opengl.cpp` uses, benchmark builds only):

| per frame | Market | Hyrule Field |
|---|---|---|
| GL calls | **2410** (9.2 per draw) | **2153** (12.9 per draw) |
| `glVertexAttribPointer` + `glEnable/DisableVertexAttribArray` | 792 | 999 |
| `glUniform2fv` + `glUniform1iv` | 518 | 329 |
| `glBufferData` / `glDrawArrays` | 261 / 261 (337 KB) | 167 / 167 (162 KB) |

A third to a half of all GL calls were the vertex-attribute arrays: every shader switch disabled
every attribute (`UnloadShader`) and re-enabled and re-pointed them all (`LoadShader`), about five
each, 51–63 switches a frame. GLES 2 has no VAOs, so that is global state reset to mostly the same
values.

`gSettings.LeanGLState` (libultraship `f011db32`, default 0):

- **1** — shadow the attribute arrays: enable, disable and re-point only what differs between the
  old and the new program. Re-established from nothing every frame (ImGui draws in the same
  context; its GL3 backend restores the three arrays it uses, but the shadow does not rely on it)
  and after the blit and shader-cache-reset paths.
- **2** — also skip uniform uploads that would resend what the program already holds (per-program
  shadow; only this backend writes these programs' uniforms).
- **3** — also draw from **client-side vertex arrays**: bind no buffer and point the attributes at
  the interpreter's own vertex array, which is allocated once and never moves, instead of one
  `glBufferData` per draw.

**Mode 2 answers the probe, and the answer is no.** It removes 1035 GL calls a frame (2410 → 1375,
−43%) and saves **−119 µs of the Market window (−0.54%), t=−3.3, MDE 93**; the period moved −136 µs,
t=−1.1, MDE 323, not resolved. Six pairs, all at 1250 MHz. About 0.1 µs per call. **State-setting
GL calls are nearly free on this driver; the mutex's share is not paid per call in any way that
matters.** Whatever the driver spends is per draw or per upload. The prompt's premise ("paid per GL
call, so count GL calls") is refuted.

That points at item G's mechanism rather than the mutex, and report 5 §5.3 already had the cheapest
test of it: client-side arrays measured best of four upload strategies (58 µs/draw against 68).
Mode 3 was that test.

### Mode 3: the result

Hyrule Field first, then the Market back to back; the second run heated the die past
`pyra-thermal`'s 82 °C rung and six of its twelve segments ran at 1000 MHz. Only pairs with both
halves at 1250 MHz are used: five of six at Hyrule Field, three of six at the Market.

| 0 → 3 | Δ frame period | t | MDE | Δ render-thread CPU | Δ swap |
|---|---|---|---|---|---|
| Market, 3 pairs | **−3685 µs (−13.4%)**, 27.5 → 23.8 ms, **36.3 → 42.0 FPS** | −51.3 | 309 | −3435 µs (−17.2%) | +74 µs |
| Hyrule Field, 5 pairs | **−1309 µs (−7.3%)**, 18.0 → 16.7 ms, **55.5 → 59.8 FPS** | −24.3 | 150 | −1648 µs (−14.4%) | **+377 µs (+7.4%)** |

Discards, stated: Market pairs 2 and 5 had both halves at 1000 MHz (−3592 and −4203 µs, consistent,
not quoted); pair 4 changed clock mid-segment. Hyrule Field pair 5 had both halves at 1000 MHz.
With only three clean pairs at the Market the MDE is wide (309 µs), but the effect is twelve times
it. An earlier unsoaked single pair, mode 2 → 3, read 27.5 → 23.8 ms: modes 1–2 contribute ~0.1 ms
of the total, mode 3 the rest.

**Why:** the ioctl count, P2's method, at Hyrule Field inside one process: **~28 bridge ioctls per
frame with `glBufferData`, ~15 without**, almost all of the difference on fd 9 (21.7 → 7.2 per
frame). With 167 uploads a frame that is not one ioctl per upload — the driver sub-allocates and
goes to the kernel for fresh device memory every so often, which is the `BM_Alloc`/`RA_Alloc`/
`mmap` churn §8.21 found in the kernel profile. The CPU saving is several times what report 5's
microbenchmark implied (8–10 µs × 261 ≈ 2.3 ms at most); that benchmark uploaded into a warm
driver with nothing else in flight. **Report 5 §5.3's conclusion, "within noise of the
alternatives and needs no change", is wrong for this game**; its ordering was right.

At Hyrule Field a quarter of the CPU saving comes back as swap time: the driver now copies vertices
at the draw and the GPU has less slack, or the frame simply reached the point where the GPU is the
next limit (16.7 ms with 5.5 ms of swap). The Market, heavier on the CPU, keeps nearly all of it.

### Visual check

In-process, per TESTING.md: a two-segment A/B with 1500-frame segments and `xwd` grabs inside each.

- **Market, mode 2 vs 3: 0 differing pixels** of 921,600.
- **Hyrule Field, mode 0 vs 3: 266,534 differing pixels, every one in the sky band (y ≤ 300) but
  4 on the horizon row, max 74/255.** A same-arm control — mode 0 against mode 0, grabs the same
  23 s apart — differs in **262,618** sky pixels, max 73, and 0 below it. The clouds scroll in the
  frozen fixture. **Identical picture at both fixtures.**

This contradicts §8.22's same-arm sky drift of 596 pixels over 150 s; today the sky moved a quarter
of a million pixels in 23 s. The frozen fixture's sky is not frozen, and how much it moves depends
on something not yet identified. Any Hyrule Field visual check must mask y ≤ 300 or carry a
same-arm control at the same spacing (TESTING.md).

### What is not yet known

- **Real play.** Mode 3 is stateful — a shadow of global GL state that anything else touching the
  context can invalidate. The paths that do (the blit, `ClearShaderCache`, ImGui) are handled by
  reading the code, and the frozen frame exercises none of them: no pause-menu framebuffer copy, no
  scene change, no menu. It needs the watch-mode A/B, which needs the owner holding the device.
- Whether the CPU saving at Hyrule Field survives in play, where the frame is capped at 30 FPS and
  the GPU has more slack than the uncapped fixture.
- Kokiri, and anything with pre-rendered backgrounds.

## 8.25 Client-side arrays in real play: −13.6% of the render thread, and the new GLES2 default

Measured 2026-09-24 afternoon, with the device's owner playing, the same way as §8.23. The device
started cold (~54 °C). Cold is fine here because the arms alternate every ~10 s, so warming affects
both equally. §8.24 left one question: mode 3 keeps a copy of global GL state, and the frozen
fixture never exercises the paths that can disturb it: the pause-menu framebuffer copy, scene
changes and ImGui.

Watch mode, `SOH_BENCH_AB_CVAR=gSettings.LeanGLState SOH_BENCH_AB_VALUES=0,3`,
`PYRA_WATCH_PIN=1 SOH_BENCH_UNCAP=0`, entered at the Market. The owner played ~9 minutes through
four scenes: 32 (Market), 80 (House of Skulltula), 81 (Hyrule Field) and 82 (Kakariko). That gave
55 blocks of 290 frames, 1250 MHz in every one, at 63–76 °C. 27 ABBA pairs, of which 2 were
dropped because a scene change fell inside the pair, leaving 25.

| 25 ABBA pairs, 0 → 3 | Δ | t | ≈ MDE |
|---|---|---|---|
| render-thread CPU per frame | **−2020 µs (−13.6%)** | −4.7 | 890 µs |
| … inside the timed window | −1923 µs (−18.1%) | −6.2 | 640 µs |
| frame period | +103 µs (+0.3%) | +0.7 | 300 µs |

(MDE is t₀.₉₇₅,₂₄·se. The pair sd is 2.2 ms because every block is different play.)

The result is half the planned ~50 pairs, and still more than four standard errors out. It matches
the frozen result: −3435 µs of render-thread CPU at the Market (−17.2%) and −1648 µs at Hyrule Field
(−14.4%). The period is the 30 FPS cap, so the saving shows up as headroom, not frame rate. Blocks
that averaged slower than the cap were 2 and 2, which is no signal.

By scene, crude (few pairs each): Market −11.6% (6 pairs), House of Skulltula −19.0% (8),
Kakariko −8.0% (9), Hyrule Field −19.0% (2).

**Correctness:** nothing appeared in the game log (no GL errors, no aborts). The owner saw no
flickering with mode 3 switching on and off every ~10 s. A broken attribute or buffer shadow would
have shown up as garbage geometry on that rhythm. The session did cover scene changes and interiors.
The owner confirmed afterwards that they opened the pause menu, which exercises the framebuffer
copy (the blit path). Whether they opened the ImGui menu was not recorded.

### Decision

**`gSettings.LeanGLState` now defaults to 3 in GLES2 builds and 0 elsewhere.** Client-side vertex
arrays are core GLES2, but a desktop GL core profile rejects them (`glVertexAttribPointer` with no
buffer bound is `GL_INVALID_OPERATION`), so the default is `#ifdef USE_OPENGLES2`. The default was
checked on device with a short run at the frozen Market fixture and no CVar set:
- `vbo_uploads=0`
- 1116 GL calls per frame, down from 2410
- frame period 24.1 ms, unsoaked, against 23.8 ms soaked in §8.24

The orphaning comment in `DrawTriangles` that told readers to leave it alone was rewritten. It
cited report 5's microbenchmark, which this section and §8.24 contradict for the real game.

Still untested: Kokiri and pre-rendered backgrounds, as in §8.24.

## 8.26 Re-profile with client arrays on; the geometry is mostly discarded, and its cost is divides

Measured 2026-09-24 afternoon, frozen fixtures, 1250 MHz.

### The re-profile

Same method as §8.24: `bench.sh ab gSettings.ShaderOpts 3 3 $MARKET SOH_BENCH_REPS=5`, ten identical
segments, all at 1250 MHz, 74–82 °C, with `tools/prof.sh` attached (9,980 samples). `LeanGLState=3`
is now the default. The frame period was 23.9–24.3 ms before `perf` attached. The render thread ran
at **0.70 CPUs, IPC 0.54, ~30 context switches per frame**, against 0.74, 0.50 and ~27 in §8.24.

Percentages alone would mislead here, because the total shrank and every other item's share rose.
The table below converts each share to µs per frame, using the render thread's measured on-CPU time
over the profiled segments: **21,103 µs in §8.24, 17,454 µs now, −3650 µs.** The frozen A/B in §8.24
measured −3435 µs, so the profile accounts for the saving.

| DSO | §8.24 µs/frame | now | Δ |
|---|---|---|---|
| `soh.elf` | 9927 | 9840 | **−87** |
| `libc.so.6` | 3583 | 2662 | −922 |
| `libGLESv2` | 2931 | 2576 | −355 |
| kernel | 1655 | 831 | −824 |
| `libsrv_um` | 1420 | 602 | −818 |
| `pvrsrvkm` | 658 | 94 | −564 |

The whole saving came out of the driver, the kernel and libc. The game side did not move. The
driver's CPU share by DSO (`libGLESv2` + `libsrv_um` + `pvrsrvkm`) fell from 23.7% to **18.8%**,
5.0 → 3.3 ms. The PVR user-space mutex fell from 1330 to 623 µs, with 40% of GL calls gone at ~0.1 µs
each, so it too was paid per upload, not per call.

| group (self time) | share now | µs/frame §8.24 → now |
|---|---|---|
| interpreter: `GfxSpVertex`, `GfxSpTri1`, tri handlers, matrix | **19.1%** | 3425 → 3339 |
| interpreter: everything else | 12.8% | 2153 → 2229 |
| resource lookup | 6.7% | 1190 → 1173 |
| frame interpolation | 5.3% | 914 → 925 |
| malloc / free | 4.8% | 1028 → 838 |
| kernel | 4.8% | 1680 → 831 |
| memcmp / memset / hash / string | 4.3% | 829 → 754 |
| PVR user-space mutex | 3.6% | 1330 → 623 |
| `shared_ptr` refcount | 2.2% | 441 → 386 |
| unresolved (stripped DDK / libc) | 23.5% | 5305 → 4098 |

`GfxSpVertex` (7.5%) and `GfxSpTri1` (7.5%) are the top two symbols, about 1.3 ms per frame each.

### The probe: how much of the geometry is drawn

New counters (`BENCH geom` line, benchmark builds only): vertices transformed, how many were lit,
how many were overwritten by a later vertex load with no drawn triangle having used them, and
triangles submitted, trivially clip-rejected (all three vertices outside one plane) and
face-culled. `tris=` is the number drawn.

| per frame | Market | Hyrule Field |
|---|---|---|
| vertices transformed | 6561 | 3498 |
| … lit | 6075 (93%) | 3010 (86%) |
| … never used by a drawn triangle | **3816 (58%)** | **2218 (63%)** |
| triangles submitted (`GfxSpTri1`) | 4333 | 2100 |
| … texture rectangles | 42 | 46 |
| … clip-rejected | 1473 (34%) | 980 (47%) |
| … face-culled | 1196 (28%) | 344 (16%) |
| … drawn | **1664 (38%)** | **776 (37%)** |

"Never used" is a lower bound: vertices still loaded at the end of the frame are not counted.
**Most of the geometry work is thrown away.** The game submits whole room and actor meshes, and
the interpreter transforms and lights every vertex before a triangle decides whether it is visible.
A NEON rewrite of the transform would mostly speed up work that is then discarded, so it is not the
next step.

### Where the time goes inside the two functions

Release builds have no line tables, so the samples inside each function were mapped to their
instructions in the disassembly of the build-id-cached binary (`perf script -F ip` plus the mmap
base, then `llvm-objdump`). The sample PCs land mid-instruction next to long-latency instructions,
which is skid, so they were binned by 16-byte region.

**`GfxSpVertex`** (532 samples):
- **~34%** in the fog block: `fabsf(w)` compare, `1/w` divide and an out-of-line
  `Ship::Math::clamp` call.
- **~38%** at `AdjXForAspectRatio`: `x * 4/3 / (width / height)`, two dependent divides per
  vertex. `width / height` is constant for the whole frame, but the compiler reloads it every
  iteration because the stores through `d` may alias `mCurDimensions`.
- The transform, lighting and clip flags together are about a quarter.

**`GfxSpTri1`** (595 samples): spread wider. The hottest regions are the per-vertex output loop,
which divides every color channel by 255.0f: 3–4 channels × 1–3 combiner inputs, plus four for fog,
per vertex, three vertices per drawn triangle. The compiler must emit these as `VDIV`, because
`1/255` is inexact and a multiply by it rounds differently. On the Cortex-A15, `VDIV.F32` takes
about 14 cycles and is not pipelined.

So the geometry's cost is not the arithmetic a NEON rewrite would target. It is divides the code
repeats needlessly: once per vertex for a per-frame constant, and once per channel for a conversion
a 256-entry table gives exactly.

### Taking the repeated divides out: −1.1%, a third of what the profile implied

`gSettings.LeanGeom=1` (libultraship):
- a 256-entry table of `n / 255.0f` for every color, fog and grayscale channel in `GfxSpTri1`;
- `width / height` computed once per `GfxSpVertex` call rather than once per vertex.

Both are **bit-exact by construction**: the same operations on the same operands, in the same order,
and a table filled with the same `i / 255.0f` expression. No in-process pixel check was taken. The
per-vertex `x * 4/3 / aspect` divide and the fog's `1/w` stay, because a reciprocal multiply would
round differently.

| LeanGeom 0 → 1 | Δ render-thread CPU | t | MDE | Δ period |
|---|---|---|---|---|
| Market, 6 pairs, all 1250 MHz | **−193 µs (−1.1%)** | −9.7 | 51 µs | −117 µs, t −0.8, MDE 371 — not resolved |
| Hyrule Field, 2 pairs at 1250 MHz | −119 µs (−1.2%) | −103 | 15 µs | — |
| Hyrule Field, 3 pairs at 1000 MHz | −187 µs (−1.6%) | −16 | — | — |

Hyrule Field hit the thermal cap from pair 3, even starting at 61 °C (TESTING.md trap 14). Pairs whose
two halves ran at the same clock are analysed per clock; the pair that straddled the cap is dropped.

**This contradicts the attribution above.** The attribution put ~38% of `GfxSpVertex` on the aspect
divides and most of `GfxSpTri1`'s hot regions on `/255` divides, about 0.9 ms between them. Removing
roughly 40,000 divides a frame saved 0.19 ms, about 6 cycles each rather than 14. The A15 runs other
work under a divide, and sampling skid piles samples onto the instructions around it. **Sample
counts next to a long-latency instruction on this core overstate what removing it saves; treat
in-function attribution as a pointer, not a size.**

**Decision:** `LeanGeom` defaults to 1. It is small, but it is exact and costs a table.

### What this leaves

- The geometry's real lever is the **62% of triangles and ≥58% of vertices that are discarded**.
  Lighting and fog run for every vertex before visibility is known. Deferring them to the first
  drawn triangle that uses a vertex is exact if the lighting state is captured at load, but it is
  stateful. Measure its ceiling first with a throwaway A/B that skips lighting and fog outright (a
  wrong picture): if that is under ~0.5 ms, lazy vertices are not worth their complexity.
- By µs per frame, the largest blocks left are unresolved driver/libc addresses (4.1 ms), the rest
  of the interpreter (2.2 ms: `Run`'s dispatch, tri2 and tile handlers, the texture-hash handler)
  and the game-side resource lookups by name (1.2 ms).

## 8.27 The ceiling of lazy vertex lighting: under the gate, and lighting is a latency chain

Measured 2026-09-24 evening, frozen fixtures, 1250 MHz, 6 ABBA pairs each, soaked to 70 °C, with
the die let down to ~65 °C between runs. Every segment of every run was on the pin (74–81 °C); none
discarded.

### The probe

`gSettings.ProbeSkipLight` (libultraship, **benchmark builds only**, read per frame in
`StartFrame`) skips work in `GfxSpVertex` outright: 1 = lighting, texgen and fog; 2 = lighting and
texgen; 3 = fog. The picture is wrong on purpose. Position, clip flags and the aspect adjustment
still run, because a lazy version would have to compute them at load. Draws, triangles, GL calls
and every geometry counter were identical between arms, and no branch in `GfxSpTri1` depends on a
vertex's colour, alpha or UV values, so the delta is the skipped work and nothing downstream.

| render-thread CPU per frame | Δ | t | MDE | Δ timed window | Δ period |
|---|---|---|---|---|---|
| Market, mode 1 (all) | **−972 µs (−5.8%)** | −37.1 | 67 | −1132 µs, t −15.0 | −1207 µs, t −8.2 |
| Market, mode 3 (fog only) | **−164 µs (−1.0%)** | −15.7 | 27 | −230 µs, t −3.1 | −299 µs, t −2.7 |
| ⇒ Market, lighting + texgen | ≈ −808 µs | by subtraction; mode 2 not run | | | |
| Hyrule Field, mode 1 (all) | **−352 µs (−3.6%)** | −25.1 | 36 | −327 µs, t −4.4 | −229 µs, t −1.4 (not resolved) |

MDE is 2.571 × se at df = 5, computed from the `oncpu` lines the same way `bench.sh` computes it
for the window.

### The ceiling

A lazy build still pays for every vertex a drawn triangle uses. Its ceiling is the skipped cost
times the unused fraction (≥58% Market, ≥63% Hyrule Field; lower bounds, §8.26):

| | Market | Hyrule Field |
|---|---|---|
| lazy lighting + fog | ≈ 560 µs | ≈ 220 µs |
| lazy fog alone | ≈ 95 µs | — |
| lazy lighting alone | ≈ 470 µs | ≈ 200 µs (assuming the Market's fog share) |

Those are ceilings before the lazy version's own cost: a pending flag per slot, a check per
triangle vertex, and a state snapshot. **Lazy lighting is under the ~0.5 ms gate at both
fixtures and is not built.** Lazy fog would be easy and exact (`d->z`/`d->w` are stored before the
fog block touches `w`, and `fog_mul`/`fog_offset` fit in `LoadedVertex`'s tail padding), but ~95 µs
does not pay for a new invariant either.

### This contradicts the §8.26 attribution, in the other direction

§8.26 put transform, lighting and clip flags together at about a quarter of `GfxSpVertex`'s
samples, ~0.33 ms. Lighting alone measures ~0.8 ms. In §8.26 the samples overstated the divides
~3×; here they understate a dependent chain ~2.5×. Skid moves samples onto whatever follows a
long-latency instruction, in either direction. **In-function sample attribution on this core is
not a size, in either direction.** Only the A/B is.

### What the lighting loop actually does

New counters (`BENCH light` line, benchmark builds only), per frame:

| | Market | Hyrule Field |
|---|---|---|
| lit vertices | 6075 | 3010 |
| directional light iterations | 12150 | 6020 |
| positional light iterations | 0 | 0 |
| texgen vertices | 17 | 17 |
| fogged vertices | 6105 | 3016 |
| light-direction recomputes (`lights_changed`) | 239 | 38 |

**Exactly two directional lights per lit vertex, in both scenes; no positional lighting, almost no
texgen.** The ~808 µs is then ~133 ns, **~165 cycles, per lit vertex** for two dot products and an
ambient term. ~~That is not arithmetic throughput.~~ **§8.28: wrong as a diagnosis — overlapping the
chains exactly gains nothing in the game, and isolated the loop runs 53–87 ns per vertex.** Per light the code is one dependent chain: three
int8 → float converts, a three-term dot product, `intensity /= 127.0f` (VDIV), a compare, then for
each channel `r += intensity * col` with `r` an `int`, so an int → float → int round trip per
channel per light. The two lights share `r`/`g`/`b`, so their chains serialise too. Fog, for
comparison, is ~27 ns per vertex.

### What this leaves

- **Exact, stateless: interleave the lighting across vertices.** Light several vertices together,
  doing the same operations in the same order for each, so their chains overlap. It is bit-exact
  only if the compiler contracts (`VFMA`) the same way in both versions, so the prototype needs an
  in-process equivalence check: a benchmark-build mode that runs both and counts differing colours
  over real frames. Unlike lazy lighting it also speeds up the vertices that are used. **Unproven:**
  §8.26 showed the A15 already hides some latency, so size it with an A/B before believing any
  cycle count.
- The ranking from §8.26 otherwise stands: the rest of the interpreter (2.2 ms), game-side lookups
  by name (1.2 ms), interpolation (0.9 ms). All three are sampled numbers, and this section is a
  second reason not to trust a sampled size.

## 8.28 Interleaving the lighting: exact, and worth nothing

Measured 2026-09-24 night. §8.27's diagnosis, that lighting costs ~165 cycles per vertex because each
light is one dependent chain, was tested and **is refuted**.

### The change, and proof it was exact

`gSettings.LeanLight=1` (libultraship `537c92cc`, reverted in `12e0a6ef`) lit a `GfxSpVertex`
call's vertices in a separate pass, four at a time. It used the same operations the inline loop
compiles to, spelled with `fmaf`, because GCC contracts to `VFMA` under its default
`-ffp-contract=fast` (checked in the disassembly: the dot product is
`fma(n2,c2, fma(n1,c1, fma(n0,c0, 0)))`, then `VDIV` by 127, and each channel
`(int)fma(col, I, (float)r)`). Mode 2 ran both and counted differing colours:

| fixture | lit vertices / frame | mismatches |
|---|---|---|
| Market | 6075 | **0** |
| Hyrule Field | 3010 | **0** |
| Kokiri Forest | 4120 (207 texgen) | **0** |
| Market, positive control (intensity × 1.01) | 6075 | 4030 |
| Market, `× (1/127)` instead of `/ 127` | 6075 | 0 |

The control shows the check can see a difference. The last row is a side result: at the Market the
inexact reciprocal never changed a truncated colour.

### The result

| `LeanLight` 0 → 1, Market, 6 pairs, all 1250 MHz | Δ | t | MDE |
|---|---|---|---|
| render-thread CPU | **+43 µs (+0.3%)** | 1.9 | 57 |
| timed window | +111 µs | 2.0 | 141 |

No gain; if anything a little slower. Hyrule Field was not run.

### Why: a microbenchmark

`tools/lightbench.cpp`, same compiler and flags, on the device pinned to 1.25 GHz, 6144 vertices,
two lights, outputs checked against the original:

| per lit vertex | coherent normals | random normals |
|---|---|---|
| A — the inline loop as shipped | **53 ns (67 cycles)** | 87 ns |
| B — inline, branchless (`I` cleared to +0 via its sign bit) | 85 ns | 85 ns |
| C — four at a time (`LeanLight=1`) | 48 ns | 56 ns |
| D — four at a time, branchless | 62 ns | 62 ns |
| light-direction recompute (`CalculateNormalDir`-like) | 92 ns per call | |

All four are bit-identical. The A15 already overlaps the chains of neighbouring vertices; batching
saves ~5 ns per vertex on coherent normals (~30 µs at the Market, under the MDE, as measured).
The branchless form is *slower*: moving a float to the integer side and back costs more than a
predicted branch, so `VMRS` was not the problem either. Only on random normals, where the `I > 0`
branch mispredicts, does anything gain much.

### The size of lighting, measured directly

`ProbeSkipLight` 0 → 2 (lighting and texgen only, fog left on), Market: **−740 µs render-thread CPU
(−4.4%), t −27.6, MDE 74**, over 5 pairs. The sixth pair was discarded because segment 11 dropped
to 1000 MHz. §8.27's −808 µs by subtraction is confirmed within its error.

That is still twice what the isolated loop predicts: 6075 × 53 ns plus 239 recomputes × 4 calls ×
92 ns is ~0.41 ms, or ~0.62 ms with random-normal branch behaviour. The gap is **not explained**.
Branch mispredictions on real mesh normals are the most likely candidate, but the in-game batch
would then have gained, and it did not. Nothing here measures it.

### What this leaves

- **The exact lighting line is closed.** Lazy lighting's ceiling is under the gate (§8.27), and
  interleaving recovers nothing. What remains exact is the light-direction recompute: 239 per frame
  at the Market (38 at Hyrule Field), each redoing 3 divides, a 3×3 multiply and a normalise for
  2 lights + 2 lookat vectors, ≈ 90 µs by the microbenchmark. A cache keyed on the light directions
  and the top of the modelview stack could skip repeats, but the ceiling is ~0.1 ms.
- The remaining blocks are sampled sizes: the rest of the interpreter (2.2 ms), game-side lookups
  by name (1.2 ms), interpolation (0.9 ms). §8.26–8.28 found sampled sizes off by 2–3× in both
  directions. Size each with a skip-it probe before choosing.
- **Method:** a microbenchmark on the device took seconds and predicted the in-game null. When a
  hypothesis is about how the core executes a loop, try it there before a 10-minute soaked A/B.

## 8.29 Frame interpolation is 2.7 ms, three times its sampled size; most of it is building the recording

Measured 2026-09-24 night, frozen fixtures, 1250 MHz, soaked to 70 °C, the die let down to
~63–65 °C between runs. First of the skip-it probes NEXT.md 00000 asked for.

### The probe

`gSettings.ProbeSkipInterp` (**benchmark builds only**, read per game tick):

- **1** — `FrameInterpolation_StartRecord` returns before setting `is_recording`, so `Play_Draw`
  records nothing, `FrameInterpolation_Interpolate` walks two empty trees and returns an empty map,
  and `GfxSpMatrix` never finds a replacement. `InterpolationFPS` is untouched, so every tick still
  emits 1.5 frames. Nothing else in the tree reads `is_recording`.
- **2** — record as usual, but `FrameInterpolation_Interpolate` returns an empty map at once. Mode
  1 minus mode 2 is the recording; mode 2 is the walk and the interpreter's map lookups.

New counters on a `BENCH interp` line: `GfxSpMatrix` calls (`mtx`), the ones answered from the
replacement map (`mtx_repl`), the map's size (`map`), and child nodes and ops recorded per frame
period (`children`, `ops`; recording happens in `Play_Draw`, outside the timed window, so these are
read at a segment's ends like `res_loads`).

| per frame period | Market | Hyrule Field |
|---|---|---|
| `GfxSpMatrix` calls | 366 | 81 |
| answered by an interpolated matrix | 233 (2 of 3 frames interpolate) | 42 |
| replacement map size | 110 | 24 |
| recorded child nodes | 313 | 93 |
| recorded ops | 820 | 199 |

Mode 1 zeroed the last four and nothing else. Draws, triangles and every `geom` counter were
identical between arms at the Market; at Hyrule Field `tris` moved 757.9 → 757.8, the scrolling
sky.

### Visual check: pixel-exact

In-process, Market, `SOH_BENCH_FRAMES=2400 SOH_BENCH_REPS=2`, two grabs in each of segment 0
(off), 1 (on) and 3 (off, the same-arm control). A-2, B-1, B-2, C-1 and C-2 are identical in every
pixel. A-1 differs from all of them, **including A-2 of its own arm**, in one pixel by 2/255 in one
channel. In a frozen scene the two recordings are the same, so the interpolated matrices are the
current ones, and the fixed-point round trip in `GfxSpMatrix` lands on the same value. The skip
changes the cost and not the picture, so the A/B below prices interpolation and nothing else.

### The size

Mode 0 → 1, 6 pairs each. Every segment was at 1250 MHz, none discarded:

| mode 1 (no interpolation) | Δ render-thread CPU | t | MDE | Δ period | Δ timed window |
|---|---|---|---|---|---|
| Market | **−2700 µs (−15.9%)** | −142 | 49 | −2697 µs (−11.0%), t −20.8 | +45 µs, t 0.6 (MDE 208) |
| Hyrule Field | **−647 µs (−6.6%)** | −34.3 | 48 | −582 µs (−3.5%), t −6.2 | −59 µs, t −0.6 (MDE 263) |

Mode 0 → 2, Market. Segments 9–11 fell to 1000 MHz (pyra-thermal) and are discarded, leaving 4
pairs:

| mode 2 (record, don't interpolate) | Δ render-thread CPU | t | MDE | Δ period |
|---|---|---|---|---|
| Market | −550 µs (−3.3%) | −21.2 | 83 | −657 µs, t −9.5 |

So at the Market: **recording ≈ 2.15 ms, the interpolate walk ≈ 0.55 ms, the interpreter's
replacement lookups below the MDE** (the timed window did not move in any run). All of it is
game-thread work between frames, which is why the period moves and the window does not.

The cost follows what is recorded. The Market records 3.9× Hyrule Field's items (1133 vs 292) and
costs 4.2× as much. Both come to **~2.3 µs per recorded item per frame period**, or ~2900 cycles.
Per game tick, ~470 child nodes and ~1230 ops cost ~3.2 ms to record at the Market. That is not
matrix arithmetic. Each child node is a `Path` holding a `std::map<label, vector<Path>>`, a
`std::map<Op, vector<Data>>` and a `vector`, and each op is a map lookup plus two `emplace_back`s of
a ~72-byte union. The whole tree is freed and rebuilt every tick (`previous_recording =
std::move(current_recording)` destroys the older one). *Reasoning, not measured:* allocation, frees
and pointer-chasing are the likely cost. The next step is to find out.

### This contradicts the §8.26 table, in the same direction as §8.27

§8.26 sampled interpolation at 925 µs. It measures 2700 µs, **2.9× more**, the largest miss of the
four sections. *Likely cause, from reading `tools/perfgroup.py`, not checked against the samples:*
the grouping is flat self time by symbol. The `malloc`/`free`, `memcpy` of `MtxF`s and `Matrix_*`
calls that interpolation makes land in the malloc/free, memcpy and unmatched-game rows, and only
code inside `frame_interpolation.cpp` is booked to it. So **the sampled malloc/free (0.8 ms) and
memcmp/memset (0.75 ms) blocks partly overlap this one.** Re-rank them after any change here, not
before.

### What this leaves

Interpolation is now the largest measured block after the two defaults: 16% of the Market's
render-thread CPU, 7% of Hyrule Field's. It ships on (`InterpolationFPS=30` is the Pyra default),
so it is paid in real play too, once per game tick.

- **An exact rewrite of the recording's storage** is the obvious candidate: the same tree, matched
  the same way, with its nodes and op vectors in a per-recording arena (`std::pmr` or a flat vector
  with indices) released wholesale each tick instead of freed node by node. The output would be
  the same replacement maps by construction. **Unproven:** allocation vs traversal is not
  separated, so microbenchmark it first (§8.28's method). Replay a recorded op stream of the
  Market's shape through both stores on the device.
- **Equivalence has to be checked in motion.** In a frozen fixture the two recordings are
  identical, so almost any bug in matching old to new would still produce the right matrices.
  The in-process check has to run with `SOH_BENCH_STATIC=0` or in watch mode, comparing the maps
  from both implementations.
- The ceiling of any storage fix is under 2.15 ms (Market) and ~0.5 ms (Hyrule Field, by the
  per-item rate). The walk's 0.55 ms mostly replays `Matrix_*` calls and is a separate question.

## 8.30 The recording in an arena: −8.0% of the Market's render thread, exact, now the default

Measured 2026-09-24 night, the device free for the session, frozen fixtures at 1250 MHz soaked to
70 °C, the die let down to ~65 °C between runs. §8.29's next step: microbenchmark an arena-backed
recording store, then build it only if it removes most of the per-item cost.

### The shape, from the game

`SOH_BENCH_INTERP_TRACE=<path>` (benchmark builds) writes two consecutive ticks' recording calls:
children opened and closed with their keys, each op, and the two flags that drive the actor-matrix
path. The Market was traced **on the host**, from the native benchmark build (`build-cmake`) with
the device's `oot.o2r` and the device's gameplay CVars at 800×480. Its `BENCH` counters matched the
device's (children 314.4 vs 313.2, ops 824.1 vs 820.0, identical geometry counters), so the shape
is the game's. Per tick at the Market:

| | |
|---|---|
| child nodes | 470, nested up to 14 deep, under a single root child |
| ops | 762: 162 TranslateRotateZYX, 161 ToMtx (157 on the actor-matrix path), 159 Push, 159 Pop, 31 Scale, 31 Rotate1Coord, 25 Translate, 17 SetTranslateRotateYXZ, … |
| recorded items | **1232** |

**Correction to §8.29.** Its `ops` counter counted every `append`, and `OpenChild` appends too, so
`ops` already included the children. The Market records ~820 items per frame period, not 1133;
per tick, 1232 items, not ~1700. The recording's 2.15 ms per frame period is 3.2 ms per tick, or
**~2.6 µs per item**, not 2.3. The TESTING.md description of the counter now says so.

### The microbenchmark

`tools/interpbench.cpp` replays the trace through three stores and times the reset, the recording
and the walk separately. It compares every replacement map entry by entry against A's, and has a
corrupt-one-entry positive control (2 mismatches per corrupted tick, as expected).

- **A** — the shipped `Path` of `std::map`/`std::vector`, copied verbatim.
- **B** — the same tree in `std::pmr` containers on a monotonic buffer per recording. The next tick
  but one reuses the buffer; the tree is never destructed.
- **C** — the same tree in `std` containers on a static bump allocator, dropped the same way.

The device, 1.25 GHz pinned, record + reset per item:

| ns per item | warm, 1 thread | warm, 2 threads | cold (256 KB touched per event), 1 thread | cold, 2 threads |
|---|---|---|---|---|
| A (shipped) | 852 | 1200 | 1149 | 1633 |
| B (pmr) | 277 | 283 | 471 | 501 |
| C (bump) | 255 | 247 | 437 | 552 |
| **B vs A** | −67% | −76% | −59% | −69% |

- **A allocates 3552 times per tick** (2.9 per item); B and C, never.
- **glibc takes its arena locks once a second thread exists.** A single-threaded benchmark reads A
  29% low on this core (852 against 1200 ns). The game has a dozen threads.
- **The benchmark does not fully model the game.** Its best model of A reaches ~1.6 µs per item
  against the game's ~2.6. Heap fragmentation and the real cache state are the untested remainder.
  What it did show is that the arena's share of the saving holds across every condition tried.
- The walk barely moves (449 → 416 µs per walk warm). Its cost is the result `unordered_map` and
  the matrix arithmetic, not the store.
- Arena high-water per recording: 226 KB on the device (336 KB on x86-64).

### The change

`gSettings.LeanInterp` (`soh/soh/frame_interpolation.cpp`, libultraship untouched):

- **0** — the original recorder.
- **1** — the pmr recorder: store B.
- **2** (benchmark builds) — both record every call, both are walked, and the replacement maps are
  compared entry by entry; the original's map is the one used. The comparison is counted on the
  `BENCH interp` line (`cmp`, `mismatch`). `gSettings.ProbeInterpCorrupt=1` perturbs the first ToMtx
  of each tick in the pmr recording, as the positive control.

Each buffer starts at 256 KB and, if a recording spills past it, grows to twice what that
recording used. Changing the mode clears both recorders, so one tick interpolates nothing, as
after a scene change.

### Equivalence, in motion

A frozen fixture's two recordings are identical and would hide a matching bug, so this ran with
`SOH_BENCH_STATIC=0` (the Market's NPCs walking; `ops` 907 per period against 820 frozen):

| run | entries compared | mismatches |
|---|---|---|
| device, Market, 2 × 600 frames | 139,324 | **0** |
| host, Market, 1500 frames | 178,480 | **0** |
| host, Market, `ProbeInterpCorrupt=1`, 2 × 600 frames | 139,844 | 399 + 399 |

### The result

`LeanInterp` 0 → 1, 6 pairs each, all 24 segments at 1250 MHz, none discarded:

| | Δ render-thread CPU | t | MDE | Δ period | Δ timed window |
|---|---|---|---|---|---|
| Market | **−1365 µs (−8.0%)** | −110 | 32 | −1343 µs (−5.5%), t −9.6 | **+143 µs (+0.8%), t 3.2, MDE 116** |
| Hyrule Field | **−266 µs (−2.7%)** | −13.1 | 52 | −257 µs (−1.5%), t −6.0 | −40 µs, t −1.1, MDE 94 |

At the Market that is **64% of the recording's 2.15 ms**, inside the 59–76% the microbenchmark
predicted. *Reasoning, not measured:* the remaining ~0.8 ms is the map lookups and the copying of
72-byte `Data` unions, which the arena does not touch.

**The Market's timed window got slower**, +143 µs, just past its MDE. The recording runs outside
the window, so the arena cannot be doing extra work there. *Guess, not tested:* two buffers of
~230 KB resident, where the old tree was freed back into the heap's hot free lists, cost the
interpreter some L2. Hyrule Field, with a quarter of the tree, shows nothing (−40 µs, t −1.1). The
net is −1.37 ms either way. **Not explained.**

### Visual check: within the same-arm control

In-process, Market, `LeanInterp` 0 1, `SOH_BENCH_FRAMES=2400 SOH_BENCH_REPS=2`, two grabs each in
segment 0 (0), 1 (1) and 3 (0, the control). A-1 = A-2 = C-1, and B-1 = B-2 = C-2. The only
difference anywhere is 8 pixels by 2/255 in rows 545–548, and **C-1 and C-2 differ by it too**:
the same arm, 2 s apart. The scene flickers between two states on its own. That is the §8.29 kind
of residue, larger here, and not caused by the change.

### Default

The change is exact by construction, verified entry by entry in motion, and a clear win at both
fixtures, so **`gSettings.LeanInterp` defaults to 1** (`2c4b3d4`). A single segment with the CVar
absent from the device's config read 15,600 µs render-thread CPU, the arm-1 value (15,692; arm 0
was 17,057).

### Real play: −8.1%

Watch mode, `LeanInterp` 0,1 alternating per 300-frame block, capped (`SOH_BENCH_UNCAP=0`), pinned
(`PYRA_WATCH_PIN=1`), the owner playing ~14 minutes across eleven scenes: the Market, Hyrule Field,
Kakariko, the Death Mountain trail and crater, and others. 81 blocks, all at 1250 MHz, the die at
63 → 74 °C. Pairs are adjacent blocks; 8 of the 40 were dropped for spanning a scene change,
leaving 32 (`tools/watchpairs.py`):

| real play, 32 pairs | A (render-thread CPU per frame) | Δ | t | MDE |
|---|---|---|---|---|
| `LeanInterp` 0 → 1 | 13,251 µs | **−1068 µs (−8.1%)** | −3.32 | 644 |

The saving holds in play, at the frozen Market's size in proportion, with every scene change and
lazy load of a real session. The log shows no errors. The MDE is wide because blocks differ in
content; the sign and the rough size are what this confirms.

### What this leaves

- **Interpolation now costs ~1.3 ms at the Market** (2.7 − 1.37, by subtraction): the rest of
  the recording (~0.8 ms) and the walk (~0.55 ms). Keying ops by position instead of by `(op, index)`
  maps would take the rest of the recording, but changes the matching rule and is no longer exact
  by construction. Not worth it before the unmeasured blocks are sized.
- **The malloc/free block is smaller now.** The recording made ~3550 of its allocations per tick
  (~2400 per frame period). Size malloc/free after this change, not before.
- **Method:** trace the game natively on the host when the question is the game's data shape.
  The host build runs the fixtures, and its counters matched the device's exactly.

## 8.31 Sizing the rest: two thirds of the heap traffic was looking things up by name; two exact fixes, −3 to −7% each

Measured 2026-09-24 night to 09-25 early morning, the device free for the session. NEXT.md 00000's
remaining blocks — malloc/free, game-side lookups by name, the rest of the interpreter — each got a
counter first, then a skip or an exact change, A/B'd at the Market and Hyrule Field.

### No uprobes: an LD_PRELOAD instead

The prompt's recipe, `perf probe -x libc.so.6 malloc`, cannot work on this device: the `5.6.19-m4`
kernel has `CONFIG_UPROBE_EVENTS` off (kprobes are on). `perf probe -q` fails silently. A
`5.6.19-m4-uprobe` kernel (hardware repo, `pc/pyra-crossbuild.sh kernel-m4-uprobe`: m4 plus that
option, nothing else) is installed in the test slot `l1`, not booted and not the default.

Counting did not need it. `scripts/pyra/allocshim.c` interposes malloc/calloc/realloc/memalign/
strdup and every `operator new`/`delete`, counts per thread, and bins each allocation by its return
address — the link register, so no unwinding. The benchmark prints `BENCH alloc` when it is loaded
and writes per-segment site tables (`SOH_BENCH_SITES`); `docs/pyra/tools/sites.py` names them.
**Validation:** `LeanInterp` 0 vs 1 reads 5566 vs 3199 `new` per frame, a difference of 2367 against
§8.30's ~2400 recording allocations per frame period.

### What allocates

Market, `LeanInterp=1`, per frame: **3202 `operator new`, 45 malloc.** By caller:

| | allocations | what |
|---|---|---|
| `ConsoleVariable::Get(const char*)` | 711 | a `std::string` key built from the name for every CVar read |
| resource lookups by path | ~1430 | ~13 per lookup: `LoadResourceAsync`'s promise/future, `ResourceIdentifier`, `FileAltExists`, the MQ path copy, `"alt/" + path` |
| `Interpreter::GetBaseTexturePath` | 441 | the resource path copied on every texture load, to look up in an empty map |
| frame interpolation's replacement map | ~225 | |

Hyrule Field: 1776 per frame, the same callers.

New counters: `BENCH lookup` (lookups by name, outermost entry only, and CVar reads), `BENCH ops`
(display-list commands per opcode) and, with `SOH_BENCH_OPTIME=1`, `BENCH optime` (see TESTING.md
trap 16 for its cost). Per frame at the Market: **~112 resource lookups** (103 of them
`gSPDisplayList` → `ResourceMgr_LoadGfxByName`), **~713 CVar reads** (`CosmeticsUpdateTick` 264,
`IsSceneMasterQuest` 110 — one per resource lookup, `func_800315AC` 68, HUD drawing ~100), and
**10,927 display-list commands**. `GuiWindow::SyncVisibilityConsoleVariable` also writes a CVar every
frame, unchanged.

### Lookups by name: a 1.1 ms block, now an exact cache

The skip, `gSettings.ProbeMemoLookup=1` (benchmark builds): a per-thread memo on the path pointer in
`ResourceMgr_GetResourceByNameHandlingMQ`. Frozen Market, 5 pairs at 1250 MHz (the sixth capped):
**−1052 µs render-thread CPU (−6.7%, t −31, MDE 94)**, ~9.5 µs per lookup; mode 2's name check found 0
reused pointers.

The shippable form, `gSettings.LeanGameLookup=1` (`soh/soh/ResourceManagerHelpers.cpp`): the same
cache keyed by address but verified against the whole path, used only while the resource manager's
and archive manager's generations, the extension table's (bumped in `OTRExtScanner`), the alt-asset
toggle, the MQ flag and the resource's dirty bit are unchanged. Failed lookups are not cached. A
display list with an alt/ version always takes the original route, so its original is still unloaded
each call.

- **Equivalence, in motion** (`SOH_BENCH_STATIC=0`, Market, 600 frames): mode 2 looks up without the
  cache on every hit and compares — **0 mismatches**; mode 3, the positive control, compares against
  the previous path — **68,670**.
- **Picture:** in-process, `LeanCVar` off, off vs on 0 pixels; both arms within launch noise of the
  unmodified build (26.6k pixels, the trap-3 floor).

| `LeanGameLookup` 0 → 1 | Δ render-thread CPU | t | MDE | Δ period | Δ window |
|---|---|---|---|---|---|
| Market, 4 pairs at 1250 MHz | **−1160 µs (−7.3%)** | −21.5 | 171 | −1133 µs, t −10.2 | −20 µs, t −0.3 |
| Hyrule Field, 4 pairs at 1250 MHz | **−428 µs (−4.4%)** | −11.4 | 119 | −384 µs, t −3.6 | 0 |

It beats its own probe (−1160 vs −1052) because it also skips `UnloadOriginalWhenAltExists`, which
the probe left in. It holds state across frames, so it also got a real-play A/B. These A/Bs ran with `LeanCVar=1` stuck in the config (below); both arms had it, so the deltas
stand.

**Real play.** Watch mode, `LeanGameLookup` 0,1 alternating per 300-frame block, capped and pinned
to 1250 MHz, the owner playing ~17 minutes across nine scenes (the Market and its entrance, Hyrule
Field, the Zora's River/Domain/Fountain area and others), with the pause menu and the ImGui menu
opened. 102 blocks, all on the pin; 46 pairs, 5 dropped for spanning a scene change
(`tools/watchpairs.py`):

| real play, 46 pairs | A (render-thread CPU per frame) | Δ | t | MDE |
|---|---|---|---|---|
| `LeanGameLookup` 0 → 1 | 9439 µs | **−528 µs (−5.6%)** | −3.54 | 298 |

Nothing abnormal on screen, by the owner's account; the log has the same lines, error and warning
kinds as the previous session's, only the known flush bug scaling with its length. **`LeanGameLookup`
now defaults to 1.** With both defaults, the frozen Market makes **970 allocations per frame, down
from 3202**: 679 CVar key strings, ~225 for the interpolation map.

### The texture-load path: exact, −3 to −5%

`LoadBlock` and `LoadTile` copy the texture's resource path into a new string to look it up in
`mMaskedTextures`, which is empty unless a mod registers masked textures. `gSettings.LeanTexLoad=1`
skips both while the map is empty. A find in an empty map always misses, so the result is the same.

| `LeanTexLoad` 0 → 1, 1000 MHz (pyra-thermal capped the runs; trap 14) | Δ render-thread CPU | t | MDE | Δ window |
|---|---|---|---|---|
| Market, 3 pairs | **−596 µs (−3.1%)** | −23.8 | 108 | −901 µs, t −10.8 |
| Hyrule Field, 4 pairs | **−561 µs (−4.8%)** | −27.4 | 65 | −605 µs, t −6.9 |

About 1.35 µs per texture load at 1000 MHz for a path copy and a `shared_ptr` copy — more than I
would have guessed; the measurement is what it is. Pixel-identical in-process, and within launch
noise of the unmodified build. Stateless, so **`LeanTexLoad` now defaults to 1**; a single segment
with the CVar absent showed `GetBaseTexturePath` gone from the allocation table.

### CVar reads: not exact, not shipped

> **Corrected in §8.32:** it is exact. The picture change below was the benchmark's camera override
> failing to hold, which any code change could flip; `LeanCVar=1` is now the default.

`gSettings.LeanCVar=1` finds CVars through a transparent hash on the `const char*` and returns a raw
pointer, removing the key string and the `shared_ptr` copy — all 713 allocations. Pinned to 1000 MHz,
6 pairs: **Market −252 µs (−1.3%, t −5.5, MDE 117)**; Hyrule Field −526 µs, but not stable (pairs 0–1
−146 and −268 with the window flat, pairs 2–5 −559 to −798 with the window moving as much), and the
counts put no CVar reads in the window. Unexplained.

**It changes the picture.** In-process at the Market, `LeanCVar` 0 vs 1 differs in 505k pixels (mean
0.48/255, speckles along edges up to 111/255); the same-arm control is identical. A build with the
pre-change `ConsoleVariable` matches mode 0 to launch noise and mode 1 by 512k pixels. The bisect:
mode 3 (the original keyed find checked against the transparent one on every read, the original's
answer returned) logged 0 disagreements and still changes the picture; mode 4 (mode 3 with `Get()`
also on the original find) still changes it. So the lookup is not the cause; what differs is that the
getters no longer go through `Get()`, which returns the same object. **Not found.** `LeanCVar` stays
0; modes 3 and 4 are left in for the next attempt.

**A harness bug this found:** the game saves CVars mid-run on its own, and `gSettings.LeanCVar=1`
ended up in the device's config — every later run booted with it. The harness now clears an A/B CVar
that was absent before the run, restores one that was present, and saves (it also used to read an
absent CVar as 0, which would have replaced a default of 1).

### The rest of the interpreter

`SOH_BENCH_OPTIME=1`, one Market segment, less the ~640 ns floor the no-op sync commands read: the
geometry handlers dominate as in §8.26 (`G_TRI2` ~5.1 ms, `G_VTX_OTR_HASH` ~1.8 ms, `G_QUAD` ~0.75 ms,
all clock-inflated); then `G_SETTIMG_OTR_HASH` ~0.64 ms, `G_LOADBLOCK` ~0.47 ms (the path copies above),
`G_SETTIMG` ~0.34 ms, `G_TEXRECT_WIDE` ~0.31 ms (16 µs each, 20 per frame), `G_MTX` ~0.27 ms,
`G_SETTILE` ~0.23 ms. No non-geometry handler is a millisecond; dispatch itself could not be separated
from the clock's cost. Not skip-probed.

### The timed window, again

`ProbeMemoLookup` also moved the Market's window +133 µs (t 2.8), like §8.30's +143 µs. Here the swap
accounts for most of it (+107 µs), consistent with the GPU having less slack once the game side is
faster. §8.30's did not: its swap moved +29 µs and the rest +113 µs. Not one effect.

**§8.30's regression holds.** `LeanInterp` 0 → 1 re-measured at the Market, 8 pairs, all 16 segments
pinned at 1000 MHz: render thread **−1656 µs (−8.2%, t −118, MDE 33)**, period −1670 µs (t −19.2),
and the window **+295 µs (+1.4%, t 6.4, MDE 109)** — every pair positive, growing from +124 to +442 µs
across the run. Swap +119 µs, the window's own work +175 µs. The net stays a clear win; the L2 guess
(size each arena buffer to the recording, ~230 KB, instead of 256 KB-and-double, or drop the unused
recorder's storage) is now worth testing.

### Traps, added to TESTING.md

- Segments 0–2 can still be settling after the soak (render thread 18.0 → 15.9 ms in one run): use
  `SOH_BENCH_WARMUP=2400`.
- Starting a run at 65 °C did not stop pyra-thermal capping most of six runs to 1000 MHz. Pinning
  both arms at 1000 MHz (`PYRA_PIN_FREQ=1000000`) gives whole runs; `pairs.py` takes the clock.
- `bench.sh` defaulted `PYRA_DIR` to `~/soh`, a stale binary with no archives; it now defaults to
  `~/soh-bench2`.

### What this leaves

- **The CVar reads** (~250 µs at the Market): why skipping `Get()` changes the picture.
- **The remaining heap traffic** with both on: 970 per frame at the Market, 679 of them CVar key
  strings and ~225 the replacement map.

## 8.32 LeanCVar never changed the picture: the camera override did not hold

Measured 2026-09-24 late to 09-25, the device free for the session. §8.31 found `gSettings.LeanCVar=1`
changed the frozen Market's picture (505k pixels) and left it off. The cause was not in
`ConsoleVariable` at all. **The benchmark's camera override did not reliably hold**: the game's own
camera wrote over it every tick until it happened to stop, and whether it stopped depended on stack
contents that any code change moves. With the override fixed, `LeanCVar=1` is exact at both fixtures,
−1.5% Market and −1.7% Hyrule Field render-thread CPU, and now the default.

### A deterministic visual check

§8.22–8.31's in-process check grabbed the screen with `xwd` at wall-clock times. `SOH_BENCH_DUMP=<dir>`
replaces it: the renderer writes the game's render target (the MSAA-resolved copy when
multisampled) at frames n and n+1 of every segment, so both arms are compared at the same frame
whatever their speed. `tools/dumpcmp.py` compares them. During a dumped frame every draw's vertex
data goes to `<frame>.ppm.vbo` and every `G_MTX` to `<frame>.ppm.mtx` (`tools/vbocmp.py`,
`tools/mtxcmp.py`).

- **Host** (desktop GL, native benchmark build): `LeanCVar` 0 vs 1 differed only by the frozen
  Market's own animation cycle (15–21 pixels, the same between consecutive frames of one arm).
- **Device**: 121,925 of 230,400 pixels differ, mean 0.55/255, max 191; the two arm-0 segments are
  identical, and a second launch reproduces it. So §8.31's result was real on the device, and the
  host is not a stand-in for it (TESTING.md trap 20).

### The bisect

| arm (one run, one segment each) | vs mode 0 |
|---|---|
| 5: non-zero, every getter on the original path | same |
| 6: only `GetInteger` lean | **differs** (122k) |
| 7, 8, 9: only Float / String / Color lean | same |
| 1 | **differs** |

`MALLOC_PERTURB_=165` changed nothing (122k again), so not uninitialised heap. Then the name bisect
failed in an informative way: sending every name down the lean `GetInteger` path through a hash-bucket
selector (the same work as mode 6 plus a hash, a mutex and a map) **no longer changed the picture** —
and neither did mode 1 in that binary, nor a mode that filled 4 KB of stack below `GetInteger` with
0x00 or 0xFF. Across launches the pictures fell into two classes: **P**, arm 0 of the binaries built
before the bisect code, and **Q**, every other arm of every binary. Changing `GetInteger`'s machine
code, on any path, turned P into Q.

### What differs: the camera

The vertex trace of P against Q: 240 of 261 draws differ, only in clip-space x, z and w, never y or
any colour or texture coordinate; the arm-0 control, 0 of 261. The matrix trace: 53 of 366 differ,
the first the viewing matrix, a pure yaw about the eye at (0, 64, 0). From the fixture's EYE and AT
the look direction is (0.862263, −0.506463):

| | from the matrix |
|---|---|
| Q | 0.8622589, −0.5064545 — the fixture, to 16.16 fixed point |
| P (the shipped code's picture) | 0.8621674, −0.5066071 — 0.006° off |

A log of the eye and look-at of every viewing matrix built (`SOH_BENCH_VIEW_LOG`) settled it. In P
the look-at stays at the game's own camera, (−134.5329, 64, 79.0530), for all 1,220 views of the run;
in Q it switches to the override, (−134.5, 64, 79.0), after 447.

**Why.** The freeze sets `frameAdvCtx.enabled`, and `Play_Update` then jumps to a label *after which*
`Camera_Update` still runs every tick. `Camera_Update` ends by writing its eye and look-at into
`play->view` unless the camera's status makes it return early. The harness wrote the override after
each frame, so it survived only while the camera was not writing, and the camera stopped at some tick
or never. The camera modes are full of decomp-faithful uninitialised locals, and `CVarGetInteger` is
called all through the camera code, so what those locals read changes with `GetInteger`'s stack use.
Not chased to the variable: it is the original game's behaviour, and the harness should not depend
on it.

**Fix:** the benchmark applies the override from `OnPlayDrawBegin`, which `Play_Draw` calls after the
update and just before `View_Apply`. Verified: the override lands on every frozen frame (247 views of
the game's camera during settle, then 1,746 of the override), arms 0 and 6 are identical, and the
picture is Q to launch noise (6.4k pixels in the usual region; 122k against P).

What this means for earlier sections: the timings stand — a 0.006° yaw moves nothing measurable,
and within one run the camera was whichever it was for both arms. Visual comparisons *across builds*
before this fix are not trustworthy (TESTING.md trap 19). §8.31's Hyrule Field `LeanCVar` number
(−526 µs, "not stable", the window moving as much as the thread in pairs 2–5) fits the fixture
flipping camera mid-run; not verified.

### LeanCVar=1, measured again

**Picture**, device, dumps: Market 0 pixels against arm 0 at the same interpolation phase; Hyrule
Field 0 pixels outside the sky (the clouds scroll: the arm-0 control differs there as much).

| `LeanCVar` 0 → 1, 6 pairs at 1000 MHz, all segments on the pin | Δ render-thread CPU | t | MDE | Δ period | Δ window |
|---|---|---|---|---|---|
| Market | **−257 µs (−1.5%)** | −9.1 | 73 | −66 µs, t −0.3 | −28 µs, t −0.5 |
| Hyrule Field | **−186 µs (−1.7%)** | −6.4 | 75 | −249 µs, t −4.6 | +106 µs, t 1.5 (MDE 177) |

The Market matches §8.31 (−252 µs). **Hyrule Field contradicts it**: −186 µs, not −526 µs, and
every pair negative (−58 to −263). It holds no state across frames, so no real-play A/B.
**`LeanCVar` now defaults to 1.** The raw pointer does not keep a CVar alive while it is read, as the
`shared_ptr` copy did; that matters only if another thread erases a CVar mid-read, and the original
already races on the map itself then.

**Default check:** one Market segment with the CVar absent from the config, `allocshim.so` loaded:
653 CVar reads per frame counted (the lean path's count; mode 0 reads 676), and **291 heap
allocations per frame, from 970** — 3202 before §8.31. What is left is mostly the interpolation map.

### What this leaves

- The window regression from §8.30 (+295 µs, t 6.4) — the L2 guess, still to test.
- The rest of the heap traffic: ~225 per frame for the interpolation replacement map.
- The camera code's uninitialised reads are real in play too: any build can move them. Harmless to
  the harness now; for the port, a curiosity rather than a task.

## 8.33 Logic against render: logic is 15–20% of the main thread, so a two-core pipeline buys ~10%

**Question.** Could game logic for tick N+1 run on core 1 while tick N is interpreted and submitted
on core 0? Only the audio thread uses core 1 today. The ceiling of such a pipeline is
(L + R) / max(L, R), and the frozen fixture cannot measure it because it skips `Play_Update`.

**Instrument.** `SOH_BENCH_SPLIT=<ticks>` (new, off by default) reads thread CPU time and wall time
at four marks per game tick and logs one `Benchmark split` line per window. **L** runs from the top
of the tick through `Graph_Update`, which includes `Play_Draw` building the display list: that is
game code and would move with logic. **R** is `Graph_ProcessGfxCommands` up to the audio wait:
interpolation, the interpreter over every interpolated frame, GL, ImGui and the swap. It needs only
`SOH_BENCH=1`, so it also runs under `SOH_BENCH_WATCH` during real play.

**Method.** `SOH_BENCH_STATIC=0` (logic running), `FRAMES=900`, pinned at 1250 MHz,
`InterpolationFPS=30` (1.5 frames per tick), 640×360, with the player held on the fixture mark and
NPCs live. Each figure is the mean of 9–10 windows of 100 ticks. The first two windows (load and
settle) are dropped.

| Fixture | L ms/tick | R ms/tick | R ms/frame | L share | ceiling (30 fps interp) | ceiling at InterpolationFPS=20 | main thread on-CPU | other threads ms/tick |
|---|---|---|---|---|---|---|---|---|
| Market, capped | 3.42 | 19.37 | 12.91 | 15.0% | 1.18× | 1.27× | 45% | 2.66 |
| Market, uncapped | 3.47 | 19.84 | 13.22 | 14.9% | 1.17× | 1.26× | 67% (43.3 fps) | 2.55 |
| Hyrule Field, capped | 2.54 | 11.59 | 7.73 | 18.0% | 1.22× | 1.33× | 28% | 2.81 |
| Kokiri Forest, capped | 3.93 | 16.04 | 10.69 | 19.7% | 1.25× | 1.37× | 40% | 2.37 |

L is flat across windows (p95 within ~0.5 ms of the median). R per tick is bimodal: ticks alternate
between one and two frames, which is why its median sits above its mean.

**Reading it.**

- **The ceiling overstates the gain.** It counts CPU only. Uncapped in the Market, a tick takes
  34.7 ms of wall time but the main thread is on-CPU for 23.3 ms of it. Most of the remainder is
  swap and GPU wait (`swap_us` 5.4 ms per frame), and a pipeline leaves that on the render side.
  Taking L out of the serial path gives 34.7 → 31.2 ms, **about +11% uncapped**, and only if the GPU
  absorbs the extra frames.
- **At the shipped 30 fps cap there is no frame-rate gain to have.** The main thread is on-CPU
  for 28–45% of the time already. Splitting moves 2.5–4 ms per tick to core 1, and total CPU work does not
  fall (the snapshot and hand-off add to it), so thermals are unchanged to first order. The busiest
  core's load falls from about 45% to about 38%, which is rarely enough for ondemand to drop a step.
- **The CPU time is in R**, 7.7–13.2 ms per rendered frame. Any two-core split worth its
  complexity has to divide R itself, not separate L from R. The natural cut is the interpreter
  (display-list decode, software T&L, the interpolation replay) against GL submission and the
  swap. That depends on how much of R is GL-side, which this instrument does not split.

### Real play: logic does not rise

`PYRA_WATCH_PIN=1 bench.sh watch start SOH_BENCH_UNCAP=0 SOH_BENCH_SPLIT=100`, pinned at 1250 MHz, with
18 minutes of hand play on the debug save: 221 windows across thirteen scenes, including warps,
Dodongo's Cavern, Spirit Temple and Jabu-Jabu. 201 windows ran at 1.5 frames per tick. The other 19 ran at
1.0 (30 Hz ticks with R_UPDATE_RATE 2, presumably menus and cutscenes; share 18.5%, same as the rest).

| | L ms/tick | R ms/frame | L share |
|---|---|---|---|
| all play, 1.5 frames per tick | **2.64** mean; heaviest window 3.95, heaviest window p95 4.63 | 5–13 | **18.8%** (p10 15.4, median 19.4, p90 22.3; range 10.4–26.5) |
| Kokiri Forest (0x55), the heaviest scene | 3.69 | 7.99 | 23.5% |
| Spirit Temple (0x06), the lightest share | 2.18 | 8.99 | 13.9% |

**Playing did not raise logic above the fixtures. It came out lower**: 2.6 ms mean against the
Market's 3.4, because the Market's crowd costs more than one player moving and fighting. The
fixture numbers above are therefore representative. At a median share of 19% the CPU-only
ceiling is 1.24× (1.37× in the heaviest window), before the swap and GPU wait take their part.
The main thread was on-CPU for only 28% of the time during play. Five-second windows smooth out
single-tick spikes, but the per-window p95 never exceeded 4.63 ms, so no spike was large either.

**Not measured.** Clocks other than 1250 MHz; the cost of the hand-off itself.

## 8.34 §8.30's window regression: gone in wall time; 44 µs of CPU is left, and the L2 guess fits it

Measured 2026-09-25 afternoon, the device free for the session, Market fixture, pinned at 1000 MHz
(`PYRA_PIN_FREQ=1000000`), soaked to 70 °C with `SOH_BENCH_WARMUP=2400`, the die let down to 64 °C
between runs. Every segment of both runs was on the pin; no pair was dropped.

### What the timed window's time was

`tools/pairs.py` now also pairs the window's own CPU time (`BENCH oncpu … window_us`) and `swap_us`.
The window's wall time is the render thread computing *plus* the thread blocked, and the blocked part
moves with the GPU's slack, not with the code under test. Re-reading §8.31's run
(`run.interp-market-1000.log`) that way:

| §8.31's run, `LeanInterp` 0 → 1, 8 pairs | Δ | t | MDE |
|---|---|---|---|
| window, wall | +295 µs | 6.4 | 109 |
| window, render-thread CPU | **+86 µs** | 5.2 | 39 |
| swap | +119 µs | 6.6 | 43 |

So two thirds of §8.31's regression was the thread *waiting* inside the window. Arm 1's frames started
1.7 ms sooner, and §8.31 had seen the same kind of swap increase for `ProbeMemoLookup`.

### Re-measured with the camera fix in

The same A/B on today's build (all §8.31-8.33 defaults, the camera override from §8.32):

| `LeanInterp` 0 → 1, Market, 8 pairs at 1000 MHz | A | Δ | t | MDE |
|---|---|---|---|---|
| render-thread CPU | 18,610 µs | **−1533 µs (−8.2%)** | −94.1 | 39 |
| period | 26,492 µs | −1676 µs (−6.3%) | −16.9 | 235 |
| window, wall | 21,258 µs | **+74 µs (+0.3%)** | 1.6 | 107 |
| window, render-thread CPU | 14,333 µs | **+44 µs (+0.3%)** | 4.0 | 26 |
| swap | 5,421 µs | −50 µs | −3.2 | 37 |

**The wall-time regression does not reproduce**: +74 µs, under its MDE, and the swap moved the other
way. It did not grow through the run either (pairs +270, −123, +116, −59, +16, +160, +156, +55). I
can't say which of the intervening changes did it, since a different camera, three new defaults and
~1.5 ms less render-thread work all changed at once. What survives is **+44 µs of CPU inside the window**,
real (t 4.0) and small: 0.3% of the window and 3% of what the arena saves elsewhere.

### The L2 guess, tested without the arena

The recording is built and walked outside the window, so an arena can only reach the window through
shared state, and the cache is the obvious candidate. `gSettings.ProbeTouchKB=N` (benchmark builds,
`frame_interpolation.cpp`) runs once per tick outside the window. It writes one line in every 64 bytes
of one of two N KB buffers (alternating, like the arena's two recordings) and reads both. Run on the
*original* recorder (`LeanInterp=0` set in the config for the run), 230 against 0:

| `ProbeTouchKB` 0 → 230, `LeanInterp=0`, Market, 8 pairs at 1000 MHz | Δ | t | MDE |
|---|---|---|---|
| render-thread CPU (the probe's own cost) | +501 µs | 37.2 | 32 |
| **window, render-thread CPU** | **+91 µs (+0.6%)** | 5.9 | 37 |
| window, wall | +157 µs | 2.7 | 136 |
| swap | +4 µs | 0.3 | 36 |

About 690 KB of line traffic per tick outside the window costs the interpreter 91 µs inside it. So
work outside the window *can* slow the window through the cache, at this scale, and the arena's
+44 µs is about half that. *Reasoning, not measured:* a monotonic buffer never reuses the storage a
`vector` leaves behind when it grows, so the arena touches more distinct lines per tick than the heap
tree, which recycles freed chunks within the same recording. The probe's own cost, ~750 µs per tick
for ~10,800 line accesses (~70 ns each), says those lines are missing L2.

### What was not tested, and why

The prompt's two candidate fixes cannot change the footprint:

- **Sizing each buffer to the recording (~230 KB) instead of 256 KB.** `monotonic_buffer_resource`
  touches only the bytes it hands out, so the untouched tail of a 256 KB buffer is never in the cache.
  Same recording, same lines.
- **Dropping the unused recorder's storage.** With `LeanInterp=1` the original recorder holds two
  empty root `Path`s; with 0, the arena's buffers are allocated but untouched. Neither is in the cache.

Shrinking the footprint means recycling the abandoned vector storage, e.g. a pool resource over the
arena or reserving each vector's size from the previous recording. The ceiling is the 44 µs, near
this rig's MDE, and either change adds work to the path that saves 1.5 ms. **Not built.** The net of
`LeanInterp=1` stands at −1533 µs render-thread CPU and −1676 µs period at the Market.

### What this leaves

- **Read `win_cpu_us`, not the window's wall time, when a change works outside the window.** Wall time
  inside the window includes GPU waiting, which moves whenever the game side speeds up (§8.31 and §8.34
  both saw ~100 µs of it). TESTING.md says so now.
- The remaining heap traffic (~225 allocations per frame for the replacement map) is the next block.

## 8.35 Client arrays at Kokiri in real play: −20%, and exact-looking in the pre-rendered interiors

Measured 2026-09-25 afternoon, the owner playing. §8.25 made `gSettings.LeanGLState=3` (client-side
vertex arrays) the GLES2 default from the Market, Hyrule Field and a real-play session that never
entered Kokiri or a pre-rendered room. This closes that gap.

**Pre-rendered rooms, from the data.** In the device's `oot.o2r` each of the Kokiri interiors
(`kokiri_home_scene`, `kokiri_home3`–`5`, `kokiri_shop_scene`) has a `…room_0Background_…` image
resource. Kokiri Forest (`spot04_scene`) and the Market (`market_day_scene`) have none, so **no frozen
fixture has ever drawn a pre-rendered background**. (`link_home_scene` has one too; not visited.)

**Run.** Watch mode at entrance 0x00EE, `LeanGLState` 0,3 alternating per 300-frame block, capped
(`SOH_BENCH_UNCAP=0`), pinned at 1250 MHz (`PYRA_WATCH_PIN=1`), the die at 65 → 71 °C. About 7
minutes of play: Kokiri Forest (scene 85) and five pre-rendered interiors, the Know-It-All Brothers'
(38), the Twins' (39), Mido's (40), Saria's (41) and the shop (45), with the pause menu and the ImGui
menu opened. 43 blocks, all on the pin; 7 pairs dropped for spanning a scene change, 14 used.

| real play, render-thread CPU per frame | pairs | A (`LeanGLState` 0) | Δ | t | MDE |
|---|---|---|---|---|---|
| all | 14 | 12,353 µs | **−2495 µs (−20.2%)** | −7.7 | 698 |
| pre-rendered interiors | 9 | 11,747 µs | **−2225 µs (−18.9%)** | −9.0 | 570 |
| Kokiri Forest | 5 | 13,444 µs | −2980 µs (−22.2%) | −3.7 | 2220 |

- The interiors are the clean comparison: the camera is fixed, and the draw counts of the two arms
  agree (132 against 136 per frame). Every one of the nine pairs is negative (−631 to −3054 µs).
- The forest pairs are noisier because the owner was moving: arm 3's blocks drew 14% fewer draws
  there, so part of that −22% is content. Its MDE is too wide to separate the two.
- The saving is larger than §8.25's −13.6% across eleven other scenes. Kokiri's scenes are lighter
  overall, so a fixed per-draw saving is a bigger share of them. *Reasoning, not measured.*
- **Lookups:** in every interior `lookup_miss` and `res_loads_window` are non-zero only in the first
  block after entering the room, which is scene load, and then 0. `lookup_flush` is 0 throughout. The
  pre-rendered background adds no per-frame resource loads. §8.22's worry that the `Gfxs2dexBg*` path
  lookups are uncached does not show up as a cost here.
- **Picture:** the owner saw no change in either arm, in the forest, the interiors, the pause menu or
  the ImGui menu. The log's error and warning lines are the same kinds and counts as the frozen runs of
  §8.34 (the known flush bug, 36 `RGBA Texture that isn't 16 or 32 bit` lines, two PVR
  `ComputeFrameBufferCompleteness` lines at startup).

**`LeanGLState=3` stays the default**, now covering Kokiri, pre-rendered rooms and the ImGui menu.

**A harness trap, again:** the game saved arm 0 into the config during watch mode, so the device booted
with `LeanGLState=0` afterwards until it was put back (TESTING.md trap 17). It was put back.

## 8.36 The replacement map was half a copy; the render side is one third GL, nearly all of it `glDrawArrays`

Measured 2026-09-25 evening, the device free for the session, frozen fixtures pinned at 1000 MHz
(`PYRA_PIN_FREQ=1000000`), soaked to 70 °C with `SOH_BENCH_WARMUP=2400`, the die let down to 65 °C
before every run. Every segment of every run below was on the pin; no pair was dropped. Two tasks:
the interpolation replacement map's heap traffic (§8.31-8.32), and splitting §8.33's render side
into interpreter against GL.

### The map: what allocates

`FrameInterpolation_Interpolate` builds a `std::unordered_map<Mtx*, MtxF>` per interpolated frame
(~165 entries at the Market; the `BENCH interp` line's `map=110` is averaged over all frames, one in
three of which does not interpolate) and ends with `return ctx.mtx_replacements;`. A member of a local
is not moved implicitly, so **the whole map was copied on the way out**: ~165 nodes built, ~165 more
for the copy, the rehashes as it grew, then the original freed. `allocshim` at the Market, per frame:

| | `operator new` per frame |
|---|---|
| defaults (§8.32) | 291.7 |
| `gSettings.ProbeSkipInterp=3` (no map at all, below) | **67.0** |
| `gSettings.LeanInterpMap=1` (reserve, move out) | 178.0 |
| `gSettings.LeanInterpMap=2` (also reuse the previous tick's maps and nodes) | **67.0** |

So the map was all 225 of the allocations §8.32 attributed to it, and the copy was half of them.

### The ceiling: a skip

`gSettings.ProbeSkipInterp=3` (benchmark builds) walks the recordings exactly as usual but writes every
interpolated matrix into one scratch `MtxF` (a global, so the stores stay) and hands back an empty
map. The map is never built, copied or searched; `mtx_repl` reads 0. Like modes 1 and 2 it is right
only in a frozen fixture.

| `ProbeSkipInterp` 0 → 3, Market, 6 pairs | Δ | t | MDE |
|---|---|---|---|
| render-thread CPU (A 17,251 µs) | **−370 µs (−2.1%)** | −18.1 | 52 |
| period | −607 µs (−2.4%) | −6.1 | 255 |
| window, render-thread CPU | −89 µs (−0.6%) | −4.8 | 48 |
| swap | −15 µs | −0.8 | 50 |

A quarter of the ceiling is inside the timed window: the interpreter's 233 replacement hits per
frame, and the map's cache footprint (§8.34 showed traffic outside the window reaching it).

### The change

`gSettings.LeanInterpMap` (`soh/soh/frame_interpolation.cpp`):

- **1** — reserve the map at the previous one's size and `return std::move(...)` it. The same entries
  by construction; only the bucket count can differ, and the interpreter only calls `find`.
- **2** — also reuse the maps and nodes of the previous tick: `Graph_ProcessGfxCommands` hands the
  tick's maps back after drawing (`FrameInterpolation_RecycleMaps`), their nodes are extracted, and
  `new_replacement` re-keys one (`node_type::key()`) instead of allocating. A recycled node holds a
  stale matrix; both writers (`interpolate_mtxf`, `SkinMatrix_MtxFMtxFMult`) write all 16 floats.
- **3** (benchmark builds) — mode 2, plus the same recording walked again into a fresh map and
  compared entry by entry (`cmp`/`mismatch` on the `BENCH interp` line). `gSettings.ProbeInterpCorrupt=2`
  perturbs one entry of the recycled map for the compare, the positive control.

**Equivalence, in motion** (`SOH_BENCH_STATIC=0`, Market, 600 frames each): mode 3 compared **73,432
entries, 0 mismatches**; with the control, **400**, one per interpolated frame.

**Picture:** in-process dumps (`SOH_BENCH_DUMP`, segments 0 1 1 0), mode 1 against mode 0 at the same
frame: **0 pixels** in all three comparisons. Frame n against n+1 differs by the Market's own 21-pixel
cycle in every arm.

| 6 pairs each | Δ render-thread CPU | t | MDE | Δ window CPU |
|---|---|---|---|---|
| Market, mode 1 (A 16,956 µs) | **−145 µs (−0.9%)** | −8.6 | 43 | −14 µs, t −1.1 (MDE 35) |
| Market, mode 2 (A 17,022 µs) | −161 µs (−0.9%) | −8.1 | 51 | +1 µs, t 0.1 (MDE 49) |
| Hyrule Field, mode 1 (A 10,598 µs) | **−37 µs (−0.3%)** | −2.9 | 33 | +14 µs, t 0.7 (MDE 49) |
| Hyrule Field, mode 2 (A 10,626 µs) | −62 µs (−0.6%) | −2.7 | 59 | +15 µs, t 0.5 (MDE 84) |

The period moved within its MDE in every run (225-499 µs); the thread's CPU is the measurement here.

- **The move takes almost all of what allocation-free storage takes.** Mode 2 removes ~111 more
  allocations and frees per frame than mode 1 and reads 16 µs better at the Market, inside the MDE.
  *Reasoning, not measured:* what the copy cost was mostly the re-hashing and 80-byte copies of ~165
  entries, not the allocator. That also says the rest of the ceiling (~210 µs) is the map's inserts
  and the interpreter's lookups, which no storage change touches; a flat table keyed by `Mtx*` would
  change libultraship's interface to reach part of it.
- **Mode 1 is the default** (`d185540`): exact by construction, pixel-identical, and it holds no state
  across ticks except a capacity hint, so no real-play A/B. Mode 2 is exact in motion and stays
  available; it is not the default because it keeps pools across ticks for a gain the rig cannot
  resolve. With the CVar absent from the config, one Market segment read **178 allocations per frame**
  (from 291).

### The render side, split

Two new instruments (libultraship `7168444`):

- **`eof_cpu_us`** on the `BENCH oncpu` line: the render thread's CPU time from the backend's
  `EndFrame` (depth discard, `glFlush`) through the swap — the driver's per-frame kick, as work rather
  than as waiting. `pairs.py` pairs it.
- **`gSettings.ProbeSkipDraw`** (benchmark builds, read per frame in the GLES2 backend): **1** skips the
  `glDrawArrays` in `DrawTriangles`; **2** returns from `DrawTriangles` before any GL work (state,
  uniforms, attribute pointers, the draw). Returning before the state comparisons leaves the backend's
  shadow state and the driver's agreeing, so the other arm still draws correctly. The interpreter does
  all of its own work in both. Wrong picture by design.

| Market, 6 pairs, A 16,954-16,965 µs | Δ render-thread CPU | t | MDE | Δ window CPU | Δ swap |
|---|---|---|---|---|---|
| `ProbeSkipDraw` 0 → 1 (no `glDrawArrays`) | **−4125 µs (−24.3%)** | −340 | 31 | −4174 µs (−29.3%), t −414 | −891 µs |
| `ProbeSkipDraw` 0 → 2 (no per-draw GL) | **−4389 µs (−25.9%)** | −204 | 55 | −4446 µs (−31.2%), t −237 | −941 µs |
| `eof_cpu_us`, both runs and all four map runs | 271-285 µs per frame | | | | |

Draws were 262 per frame in both arms (`BENCH work`), and the `glfn` line confirms mode 1 removed
exactly the `glDrawArrays`, and mode 2 the uniforms, enables and depth state with them (the attribute
pointers left in mode 2 are set outside `DrawTriangles`).

So of the Market's ~14.3 ms of window CPU per frame:

| | µs per frame | share |
|---|---|---|
| `glDrawArrays` in the driver | ~4130 | 29% |
| the rest of the per-draw GL side (state, uniforms, attribute pointers) | ~260 | 2% |
| end of frame: discard, flush, swap (CPU) | ~285 | 2% |
| GL calls left with mode 2 (~690 per frame: 225 texture binds, 202 attribute pointers, 68 texture units, 51 programs, ...) | not measured; §8.24 puts state calls near free | — |
| **the interpreter and everything else in the window** | **~9.6 ms** | **~67%** |

- **One draw costs ~15.8 µs of CPU at 1000 MHz**, ~16,000 cycles, and 262 of them are a quarter of the
  main thread. The swap barely moves (−0.9 ms of 5.3 ms with no geometry at all), so this is CPU time in
  the driver, not the GPU. *Reasoning, not measured:* a Market draw carries ~6 triangles (~1.5 KB of
  client-array vertices), and copying that is ~1 µs, so the cost is per call — validation and command
  emission — rather than per vertex. The next probe should establish that directly.
- **This refines §8.24, it does not contradict it**: GL call *count* is still not the cost — removing
  43% of the calls saved 0.5% — but the draw calls themselves are the largest single block measured in
  this project. Fewer draws is now the lever, at ~16 µs each; NEXT.md item H (flushes on texture loads
  that do not change the texture) is the first place to count them.
- **For a two-core design**: the GL side is ~4.7 ms of the Market's ~17 ms of main-thread CPU per frame.
  A GL thread fed by the interpreter would move about that much off the main thread (~27%), before the
  hand-off's own cost (a copy of each draw's vertices and state). §8.33's logic split moves 2.5-4 ms per
  tick. These are the two numbers a design would start from; neither is designed here.

### What this leaves

- **The per-draw cost: per call or per state change?** A probe that issues each draw twice (the second
  with no state change in between) prices the call alone. If the second call is nearly as dear, batching
  and removing draws is the lever; if it is cheap, the cost is state validation, and sorting draws by
  state is.
- **How many of the 262 draws are avoidable.** Count flushes by cause in the interpreter (texture
  change, combiner, other-mode, matrix, ...) and item H's no-op texture reloads.
- The map's remaining ~210 µs ceiling needs an interface change for a small part of it. Not worth it
  before the draw work.

## 8.37 A draw is ~4 µs of call, ~2 µs of vertices and ~9 µs of validation; 33 Market draws were item H's

Measured 2026-09-25 night, the device free for the session, frozen fixtures pinned at 1000 MHz, soaked
to 70 °C with `SOH_BENCH_WARMUP=2400`, the die let down to 65 °C before every run. Every segment of every
run was on the pin; no pair was dropped. The two tasks §8.36 left: price one draw call, and count the
avoidable ones.

### Two instruments

libultraship `9520c3dd`:

- **`gSettings.ProbeSkipDraw` 3 and 4** (benchmark builds): 3 issues every `glDrawArrays` in
  `DrawTriangles` twice, the second with no state change between; 4 repeats only the first triangle.
  Wrong picture by design (blended draws blend twice).
- **`BENCH batch <seg>`**: every draw counted once, under the *set* of state changes that separated it
  from the previous batch. Each flush site in `GfxSpTri1` marks a bit before it flushes (depth, decal,
  viewport, scissor, sampler, program, blend, full buffer); the texture site marks `tex_new`, or
  `tex_same` when the reload resolved to the cache entry already bound in that unit with no mask or
  blend texture (NEXT.md item H); a flush that draws with no mark is `other` (framebuffer commands,
  end of frame). The set is counted when the next batch takes its first triangle. The line gives
  `cause=any/alone` per bit, then the commonest sets. The first build marked the texture site after its
  flush, so `other` read 241 of 261; fixed before any number below was taken.

### The price of one draw

| 6 pairs each, Δ against mode 0 | Δ window CPU | t | MDE | per draw | Δ swap |
|---|---|---|---|---|---|
| Market, mode 3 (same draw again; A 14,281 µs) | **+1685 µs (+11.8%)** | 97.5 | 44 | **6.5 µs** | +751 µs |
| Market, mode 4 (one triangle again; A 14,298 µs) | **+1120 µs (+7.8%)** | 59.8 | 48 | **4.3 µs** | +151 µs |
| Hyrule Field, mode 3 (A 9,051 µs) | **+771 µs (+8.5%)** | 56.5 | 35 | **4.6 µs** | +787 µs |

With §8.36's 15.8 µs for a whole Market draw (skipping `glDrawArrays` outright), one draw at 1000 MHz is:

| | µs | share |
|---|---|---|
| the call, with state the driver has just validated, one triangle | ~4.3 | 27% |
| the batch's vertices (client arrays, ~6 triangles on average at the Market) | ~2.2 | 14% |
| the first draw after a state change: validation | **~9.3** | **59%** |

- **It is mostly state validation, and partly per call.** Neither "fewer draws" nor "sort by state" is
  the whole lever: a draw that changes nothing still costs ~4.3 µs, and a state change adds ~9.
- The repeat's per-vertex part (~2.2 µs for ~19 vertices, ~115 ns each) is in the driver, on the CPU:
  mode 4 moved the swap only +151 µs against mode 3's +751, so the extra GPU work of mode 3 shows in the
  swap, not in window CPU. *Reasoning, not measured:* ~115 ns per vertex is more than copying 40-odd
  bytes would take; the driver may be converting or re-laying out the client arrays.
- Hyrule Field's repeat is cheaper (4.6 µs) than the Market's (6.5); its draws are smaller.

### What splits the batches

Frozen fixtures, per frame. `any` counts a cause wherever it appears in a draw's set; `alone` where it
was the only one.

| cause | Market (261 draws) any / alone | Hyrule Field (167) any / alone |
|---|---|---|
| `tex_new` (a different texture) | **225 / 142** | **151 / 90** |
| `tex_same` (item H) | 67 / **33** | 21 / **13** |
| `program` | 51 / 1 | 63 / 2 |
| `blend` | 20 / 0 | 26 / 0 |
| `depth` / `decal` | 19 / 12 | 5 / 6 |
| `sampler` | 4 / 0 | 2 / 0 |
| viewport, scissor, other | 1 each (frame start and end) | 1 each |
| full buffer | 0 | 0 |

The commonest sets at the Market: `tex_new` 142, `tex_same` 33, `tex_new+tex_same` 33 (one unit
changed, the other reloaded the same texture), `tex_new+program` 22, `depth+decal+tex_new+program+blend`
11. **86% of the Market's draws change a texture**; nothing else splits more than a fifth of them. No
batch ever fills the buffer. Matrices never split a batch (vertices are transformed on the CPU).

So **33 of the Market's 261 draws (13%) and ~13 of Hyrule Field's 167 (7.5%) exist only because of item
H**. A merged draw makes no state change, so what it saves is the repeat price, ~4.3 µs, not 15.8:
≈140 µs at the Market, ≈55 µs at Hyrule Field.

### The change: `gSettings.LeanTexReuse`

`fd067827`. Before the texture site flushes, `TextureAlreadyBound` builds the cache key exactly as
`ImportTexture` would and looks it up; if it resolves to the entry already in `mRenderingState.mTextures[i]`
**and** the backend confirms that texture id is bound in unit `i` (`GfxRenderingAPI::IsTextureBound`, new;
false in every backend but OpenGL), the batch stays open and the entry moves to the back of the LRU as a
cache hit would move it. Masked and blended textures take the old path. The backend check matters:
`gfx_set_timg_fb` binds a framebuffer texture to unit 0 without touching `mTextures[0]`, and the
backend forgets its binding shadow at every frame start, so ImGui's binds read as unknown.

| 6 pairs each | Δ window CPU | t | MDE | Δ render-thread CPU | t | MDE | draws |
|---|---|---|---|---|---|---|---|
| Market (A 14,302 µs) | **−158 µs (−1.1%)** | −18.2 | 22 | −166 µs (−1.0%) | −23.2 | 18 | 261 → 228 |
| Hyrule Field (A 8,981 µs) | −24 µs (−0.3%) | −1.3 | 49 | −16 µs (−0.2%) | −1.9 | 21 | 167 → 155 |

- The Market saving matches the prediction (33 × 4.3 ≈ 140 µs). Hyrule Field's predicted ~55 µs is at
  the run's MDE, and it did not resolve. The period moved within its MDE in both (437 / 173 µs).
- `glBindTexture` stayed at 225 per frame at the Market in both arms: the change removes draws, not binds.
- Hyrule Field's `eof_cpu_us` rose 12 µs (t 6.5, MDE 5). Small and unexplained; the Market's did not move.
- **Picture:** in-process dumps (segments 0 1 1 0), frames 301 and 302: **0 pixels** between the arms at
  the Market, and 0 at Hyrule Field with the sky masked (rows 216 and up). Frame 301 against 302 differs
  by the Market's own 20-pixel cycle in every arm.
- **Real play** (it trusts the backend's binding shadow across draws, so it is stateful): watch mode,
  `LeanTexReuse` 0,1, the owner playing across seven scenes with the pause menu, framebuffer effects and
  the ImGui menu. 38 blocks, 16 pairs (3 dropped across scene changes, 0 off the pin): +85 µs (t 0.17,
  MDE 1054), i.e. nothing a 1% effect could show. The owner saw nothing wrong; the log held only the
  known noise (TESTING.md trap 11) and six PVR framebuffer-completeness messages at framebuffer creation.
- **Default 1** since then. With the CVar absent from the config, one Market segment read 228 draws,
  `tex_same` 0 and 14,067 µs of window CPU at 1000 MHz.

### What this leaves

- **Texture changes are the draw cost.** 225 of 261 Market draws change a texture, and a draw after a
  state change pays ~9 µs of validation on top of ~6.5. The probes above do not say which state costs
  the validation: whether a texture bind alone triggers it is the next probe (a mode that binds another
  texture and back between the two draws).
- **Merging texture changes means reordering draws**, which is only safe for opaque, depth-tested,
  non-decal draws and changes the order the GPU sees. Count first how many consecutive opaque draws
  could be grouped by texture if sorted within a run of such draws; then decide.
- **For the two-core design:** the draw cost cannot be simply removed. Item H took 13% of the Market's
  draws and 1% of its window CPU; the rest are real state changes. §8.36's ~4.7 ms of GL work per Market
  frame stands, less ~0.16 ms.

## 8.38 A texture bind costs ~5.5 µs at the next draw, even when it binds the same texture back

Measured 2026-09-25 night, the device free, frozen Market pinned at 1000 MHz, soaked to 70 °C with
`SOH_BENCH_WARMUP=2400`, the die let down to 65 °C before each run. All 24 segments were on the pin.
`LeanTexReuse=1` is the default here, so the Market draws 228-229 per frame.

### The probe

`gSettings.ProbeSkipDraw` 5 and 6 (libultraship `60114393`), A/B'd against mode 3 (every draw twice), so
the delta is only what a texture bind adds to the repeat. Both bind on unit 0; the other texture is the
one unit 0 held at the previous draw (a real scene texture), or a complete 1×1 texture when that is the
same one. Unit 0 and the backend's shadow end as they started.

- **5**: bind the other texture and the original back, then repeat. The repeat draws exactly as before.
- **6**: repeat with the other texture bound, then restore. A real texture change, nothing else.

| 6 pairs each, Δ against mode 3 | Δ window CPU | t | MDE | per draw |
|---|---|---|---|---|
| mode 5, bind away and back (A 15,931 µs) | **+1208 µs (+7.6%)** | 56.0 | 55 | **5.3 µs** |
| mode 6, another texture (A 15,691 µs) | **+1313 µs (+8.4%)** | 63.9 | 53 | **5.8 µs** |

`glfn` shows the extra calls and nothing else: +2 `glBindTexture` and +1 `glActiveTexture` per draw
(≈0.1 µs each, §8.24). The swap *fell* ~200 µs in both, the GPU side is not what the numbers measure.

- **The driver validates on any bind, not on a change of texture.** Binding a texture away and straight
  back costs 5.3 µs at the next draw; a draw that really samples another texture costs 5.8. The backend's
  shadow already skips binds of the texture already bound; nothing smaller than "fewer binds" helps.

### §8.37's decomposition, corrected

§8.37 split §8.36's 15.8 µs per draw as if every draw were alike. That average was over 262 draws, 33 of
which (item H's) changed no state and cost ~6.5 µs. The other 229 therefore averaged (4125 − 33 × 6.5) /
229 ≈ **17.1 µs**, and with this probe:

| a Market draw that changes state, 1000 MHz | µs |
|---|---|
| the call | ~4.3 |
| its vertices (client arrays) | ~2.2 |
| the texture bind, validated at the draw | **~5.5** |
| the rest of the state change (texture-size uniforms, attribute pointers, program, ...) | ~5 (by difference) |

*Reasoning, not measured:* the texture-size uniforms follow texture changes (85 `glUniform2fv` per frame),
so a uniform upload is the likeliest part of the remaining ~5 µs; a repeat after re-uploading an
unchanged uniform would price it the same way.

### What this leaves

- Every texture change costs ~5.5 µs of driver time however it is issued, and the Market makes 225 of
  them: ~1.2 ms per frame. Only fewer binds reduce it: grouping draws by texture (reordering, NEXT.md),
  or fewer distinct textures per frame (atlases; not a small change).
- Price the uniform part the same way before deciding anything about uniforms.

## 8.39 A uniform upload costs ~1.2 µs at the next draw; uniforms are not the missing ~5 µs

Same session and conditions as §8.38 (frozen Market, 1000 MHz, soaked, 65 °C between runs, all 24
segments on the pin, `LeanTexReuse=1`, 228 draws).

`gSettings.ProbeSkipDraw` 7 and 8 (libultraship `b931969e`), against mode 3 as in §8.38, on draws whose
program samples a texture (226 of 228): **7** re-uploads the texture-size uniform with the value it holds
before the repeat; **8** draws the repeat with the first width one larger, then restores it.

| 6 pairs each, Δ against mode 3 | Δ window CPU | t | MDE | extra `glUniform2fv` | per draw |
|---|---|---|---|---|---|
| mode 7, same value (A 15,908 µs) | **+280 µs (+1.8%)** | 11.5 | 63 | 226 | **~1.2 µs** |
| mode 8, different value (A 15,806 µs) | **+296 µs (+1.9%)** | 9.9 | 77 | 452 | **~1.3 µs** |

- **A uniform upload costs ~1.2 µs at the next draw, whatever the value** — the driver re-validates on the
  call, as with binds (§8.38), but at a fifth of a bind's price. Mode 8's second upload per draw (the
  restore) lands before the next real draw and did not add a second 1.2 µs there, which fits "dirty once,
  validated once per draw".
- The game makes 85 such uploads per frame at the Market (`LeanGLState` ≥ 2 already skips unchanged
  ones): ~0.1 ms, ~0.5 µs averaged over the 229 state-changing draws. **§8.38's remaining ~5 µs is ~4.5 µs
  unexplained.**
- What is left per frame to account for it: 202 `glVertexAttribPointer` (shadowed; re-specified when a
  program's layout or stride changes), 51 `glUseProgram`, and depth/blend/polygon-offset changes (35
  `glEnable`, 21 `glDepthMask`, ...). The attribute pointers are the only one near one per draw.

### What this leaves

- **Price an attribute-pointer re-specification** the same way (a mode that re-issues the program's
  unchanged `glVertexAttribPointer`s between mode 3's draws). If it is the ~4.5 µs, a single vertex
  layout for every program (padding the unused attributes) would remove it without reordering anything.

## 8.40 Pointers and programs price the rest of a draw; 85 of the Market's 228 draws could merge by texture

Measured 2026-09-25 afternoon, the device free for the session, frozen Market pinned at 1000 MHz, soaked
to 70 °C with `SOH_BENCH_WARMUP=2400`, the die let down to 62-65 °C before each run. All 36 segments were
on the pin; no pair was dropped. `LeanTexReuse=1` is the default here, so the Market draws 228 per frame.
The two tasks §8.39 left: price an attribute-pointer re-specification, and count what grouping by
texture could remove before building any reordering.

### The probes

`gSettings.ProbeSkipDraw` 9, 10 and 11 (libultraship `dd32139f`, mode 10 as below from `3d5a2408`), each
A/B'd against mode 3 (every draw twice) as in §8.38, so the delta is only what the extra calls add to the
repeat:

- **9**: re-issue every attribute pointer of the current program with the values it already holds.
- **10**: point each of them elsewhere (stride + 4) and straight back, as mode 5 does for a bind. Mode 10
  first re-issued only the first pointer; it was changed before it ran, once mode 9 showed an unchanged
  pointer costs nothing, because the game's own re-specifications are real changes.
- **11**: `glUseProgram` of the last other program drawn with, and back.

| 6 pairs each, Δ against mode 3 | Δ window CPU | t | MDE | extra calls per frame | per draw |
|---|---|---|---|---|---|
| mode 9, pointers re-issued unchanged (A 16,092 µs) | +40 µs (+0.3%) | 0.5 | 220 | 1304 `glVertexAttribPointer` | < 1 µs |
| mode 10, pointers away and back (A 16,047 µs) | **+561 µs (+3.5%)** | 20.0 | 72 | 2620 `glVertexAttribPointer` | **~2.4 µs** |
| mode 11, program away and back (A 16,067 µs) | **+1335 µs (+8.3%)** | 45.3 | 76 | 458 `glUseProgram` | **~5.8 µs** |

Mode 9's run was noisy: one pair read −385 µs, the other five +87 to +155 µs. Either way it is well
under a microsecond per draw. Mode 11 moved the swap −162 µs (t −11.1) and `eof_cpu_us` −12 µs; the
window CPU is what it measures.

- **The driver skips an unchanged attribute pointer, but not a changed one.** Pointing every attribute
  of the program (~5.7 per draw) away and back costs ~2.4 µs at the next draw.
- **A program switch costs ~5.8 µs at the next draw**, the same as a texture bind (§8.38), even when the
  draw ends up on the program it started with.

### What a state-changing draw costs, now

The game makes 51 program switches and 202 pointer re-specifications per Market frame, and the pointer
changes come with the program switches (~4 each). Averaged over the 229 state-changing draws at 1000 MHz:

| | µs |
|---|---|
| the call (§8.37) | 4.3 |
| its vertices (§8.37) | 2.2 |
| texture bind, 225 per frame × 5.5 (§8.38) | ~5.4 |
| uniform uploads, 85 × 1.2 (§8.39) | ~0.5 |
| program switch, 51 × 5.8 | ~1.3 |
| attribute pointers, 51 changes × ~2.4 | ~0.5 |
| **accounted** | **~14.2 of 17.1** |

*Reasoning, not measured:* the ~3 µs left is depth, blend and polygon-offset changes (35 `glEnable`, 21
`glDepthMask` per frame) and whatever the prices fail to add linearly. §8.39 already saw one validation
per draw, not one per call. None of it is a lever on its own. **One vertex layout for every program, the
change §8.39 hoped for, would save ~0.1 ms per frame: not worth it.**

### What grouping by texture could remove

`BENCH group <seg>` (same commit): a **run** is a maximal sequence of consecutive draws that are opaque
(no blend), depth-tested with depth writes, not decal and not "invisible", with no viewport, scissor or
framebuffer change between. The backend bumps an epoch at every framebuffer switch, clear, copy, resolve
and frame start (`gBenchFbEpoch`), which ends a run. Within such a run only equal depths make the order
visible. Each draw's texture set is the cache entries of the textures its program samples, with their mask
and blend textures. The counts are per frame, and identical in every segment and on the host's native
build (TESTING.md §6):

| | Market | Hyrule Field (host) |
|---|---|---|
| draws per frame | 228 | ~155 |
| runs | 11 | 13 |
| draws in runs | **159** | 48 |
| … whose texture set an earlier draw of the run had (`tex_dup`) | **86** | 15 |
| … with the same program as well (`key_dup`) | **85** | 15 |
| texture-set changes inside runs, as issued → sorted by texture set | **148 → 62** | 35 → 20 |

- **At the Market, 85 draws per frame (37%) could join an earlier draw of their run**: same textures,
  same program, same depth and blend state. The per-draw uniforms come only from the textures and the
  program (`SetPerDrawUniforms`), so a merged draw draws the same thing. Sampler parameters live on the
  texture object and are set in place (4 per frame); a real implementation has to end a run on one.
- Hyrule Field is mostly blended or decal: only 48 of its draws are in runs, and 15 could merge.
- *Estimate, not measured:* a merged draw keeps its vertices (~2.2 µs) and drops the rest. If the draws
  that merge cost the 17.1 µs average, that is ~15 µs × 85 ≈ 1.3 ms of the Market's ~14.1 ms window CPU
  (~9%). Most of them are texture-only changes (175 of 228 draws are split by `tex_new` alone), priced
  at ~4.3 + 5.5 + ~1 µs of uniforms, so ~11 µs × 85 ≈ 0.9 ms (~6%) is the conservative figure. Hyrule
  Field: ~0.15-0.2 ms (~2%).

### Is reordering worth its risk?

**Yes, at the Market: 6-9% of window CPU is more than any lever left except moving GL to another core,
and it composes with that one.** The risk is specific and testable:

- **Depth ties.** With `GL_LESS` the earlier of two draws wins where their depths are equal. Moving a
  draw earlier changes that only where two opaque, non-decal surfaces coincide exactly. The in-process
  dumps would show it in the frozen fixtures, and a real-play session is still needed.
- **State that is not in the key.** Sampler parameters (set on the texture object), texture uploads into
  a live id, and anything the backend does outside `DrawTriangles` must end a run. If it lives in the
  backend, which already shadows every piece of state, the barriers are in one class.
- **Its own cost.** It copies each run's vertices once more into per-key buckets (~159 draws × ~6
  triangles), which is small next to 85 × 11-15 µs.

The right shape is a CVar-gated deferral in the GLES2 backend: `DrawTriangles` on a groupable state
appends the vertices to a bucket keyed by (texture ids per unit, program); anything non-groupable, and
every state or framebuffer call that could change what an earlier draw sees, replays the buckets in
first-appearance order and restores the shadow. A/B it frozen at both fixtures, dump both, then real
play.

### What this leaves

- Build the grouping (`gSettings.LeanGroup`, default 0) and A/B it. The estimate is 0.9-1.3 ms at the
  Market; if it lands well under 0.9 ms, the draw prices do not add up the way this section assumes.
- Then the two-core design: GL at ~4.7 ms per Market frame (§8.36) is ~3.6 ms after grouping.

## 8.41 Grouping by texture: −840 µs (−5.8%) of Market window CPU, ~10 µs per draw removed

Measured 2026-09-25 evening, the device free for the session, both fixtures pinned at 1000 MHz, soaked to
70 °C with `SOH_BENCH_WARMUP=2400`, the die let down to ~65 °C before each run. All 24 timed segments
were on the pin; no pair was dropped.

### The change: `gSettings.LeanGroup` (default 0)

libultraship `4c2fbd4c`, GLES2 backend, only with `LeanGLState=3`. A draw whose state is groupable (depth
test on with `GL_LESS`, depth writes, no polygon offset, no blend) is not drawn: its vertices are copied
into a bucket keyed by program, the texture ids it reads (sampled, or read for the size and filtering
uniforms, in the pairs `SetPerDrawUniforms` reads them) and the frame uniforms the program holds. Anything
that could change what a deferred draw sees, or that needs it drawn, replays the buckets first in
first-appearance order, one `glDrawArrays(first, count)` per bucket:
- a draw that is not groupable;
- a sampler change, texture upload or texture delete touching a texture a bucket reads;
- viewport, scissor, framebuffer bind, clear, copy, resolve, read-back, depth read, depth discard;
- shader-cache clear, framebuffer creation or resize, frame start and end;
- the end of the interpreter's frame, through a new `FlushDeferredDraws` hook, before ImGui draws.

Three design points decided whether it pays or is correct:

- **One arena per run.** Every bucket's vertices live in one array, in chunks that start on a whole vertex
  of the bucket program's stride, so a bucket is drawn with a `first` from the arena's start and the
  attribute pointers do not move between buckets (§8.40: ~2.4 µs per changed pointer). A bucket that
  outgrows its chunk grows in place if its chunk is the arena's last, else takes a new chunk twice as
  large (one more draw). With 2048-float first chunks 10 of 76 Market buckets split; with 8192 none do.
- **Lazy state.** In this mode binds, program switches and blend are applied at the draw, against a
  shadow of what the context holds. Depth and polygon-offset state are computed by the ungrouped path's
  own rules, quirks included (the depth function follows the decal flag only when test or mask change),
  so every draw gets the state it would have had. The frame-count and noise-scale uniforms are the ones
  the program would hold from its last `LoadShader`, not the current frame's.
- **Mode switches between frames** bring the context to what the path being entered assumes (binds,
  program, blend), so the harness can alternate arms.

### What it removes

| per frame | Market, 0 → 1 | Hyrule Field, 0 → 1 |
|---|---|---|
| `glDrawArrays` | 228 → **145** | 155 → **140** |
| draws deferred / buckets / replays | 159 / 76 / 12 | 48 / 33 / 13 |
| `glBindTexture` | 225 → 143 | 167 → 155 (host) |
| `glUniform2fv` | 85 → 49 | 71 → 64 (host) |
| `glUseProgram` | 51 → 48 | 64 → 62 (host) |

The prediction was ~143 at the Market (§8.40's `key_dup` 85). It came in at 145, from two more buckets than
`BENCH group` counts, because the key here also holds the textures the uniforms read, and one more replay
than there are runs. Host and device counts agree. Program switches barely fall. First-appearance
order keeps each run's program sequence, so the ~1.3 µs per draw of program switches (§8.40) was never
removable this way.

### Frozen A/B, 6 pairs each, 1000 MHz

| Δ, LeanGroup 1 − 0 | Market | Hyrule Field |
|---|---|---|
| **window CPU** (`win_cpu_us`) | **−840 µs (−5.8%), t −41.1, MDE 53** (A 14,391) | **−143 µs (−1.6%), t −8.9, MDE 41** (A 9,041) |
| render-thread CPU | −805 µs (−4.7%), t −34.5, MDE 60 | −140 µs (−1.3%), t −11.7, MDE 31 |
| frame period | −1123 µs (−4.5%), t −12.2, MDE 237 | −231 µs (−1.3%), t −3.5, MDE 168 |
| swap | −89 µs, t −5.0, MDE 46 | −6 µs, t −0.1, MDE 131 |

- **Each removed draw saves ~10 µs at the Market (840 / 83) and ~9.5 µs at Hyrule Field (143 / 15).**
  §8.40 put the conservative figure at ~11 µs (call, texture bind, uniforms), less the extra copy of every
  deferred vertex, which this build adds. The Market came in just under §8.40's 0.9 ms low end, not well
  under it: **the §8.37-8.40 prices add, to within ~10%.** The 1.3 ms high end assumed a merged draw sheds
  the whole 15 µs, program switches included; first-appearance order keeps those (above).

### Pixels

In-process dumps (`SOH_BENCH_DUMP`, frames 301-302 and 304-305, two pairs each), on the device:

- **Market: identical.** Each arm-1 frame equals an arm-0 frame exactly; which one depends on the
  segment's interpolation phase, as always (TESTING.md).
- **Hyrule Field: 26 pixels, ±1 level, on alternate frames only, arm 1 only**, below the sky mask. 25
  are on the horizon band (rows 210-212 of 360, just under the sky, where distant fogged terrain meets
  it) and one on Link's boot. Arm 0 showed the other variant in 8 of 8 frames; arm 1 showed this one in
  4 of 8, and those frames matched each other exactly. *Reasoning, not measured:* two near-coplanar
  surfaces of almost the same colour tie exactly in depth only on some interpolated frames. Grouping
  moved the later one earlier, so under `GL_LESS` it wins the tie. This is the one difference
  reordering was expected to cause (§8.40).
- The host drew both fixtures identically below the sky.

### Real play

Watch mode at 1000 MHz, the owner playing: Kokiri Forest, houses and shop (pre-rendered), the Lost
Woods, Hyrule Field, the pause menu and its sub-screens, ImGui menus. **Nothing visible: no flicker, no
wrong or missing textures, pause menu and ImGui correct.** No speed result: 24 pairs, 6 dropped at scene
changes, window CPU +102 µs (t 0.42, MDE 509), render-thread CPU +210 µs (t 0.74, MDE 592). Blocks
differ by ±600 µs by content; the frozen effect (1.3-5.8%) is under what 24 pairs resolve.
`watchpairs.py` now takes the pin clock as a third argument; it had 1250 MHz hard-coded and dropped
every pair of this run.

### What this leaves

- ~~Whether to make `LeanGroup=1` the GLES2 default~~ **Made the GLES2 default at the owner's call**
  (libultraship, after `4c2fbd4c`), the first default that is not exact. Checked with the key absent from
  the device's config: one soaked Market segment at 1000 MHz drew 145 `glDrawArrays`, deferred 159, and
  read 13.51 ms of window CPU (the A/B's arm 1: ~13.55).
- Program switches are the part of a draw grouping did not take (~48 per Market frame, ~5.8 µs each at
  the next draw). Sorting a run's buckets by program would take some, at the price of more reordering.
  Not measured.
- The two-core design: GL was ~4.7 ms per Market frame (§8.36), ~3.9 ms with grouping.
