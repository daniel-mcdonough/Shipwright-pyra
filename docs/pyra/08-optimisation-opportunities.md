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

Needs a draw-call-per-frame count before it can be sized — that number is not yet measured.

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
| 1 | Filtering into shader key, not a uniform branch (§8.2) | 1 | large → **revised down**, §8.9 | low | yes | **next to screen** |
| 2 | Kill dependent texture reads via `GL_CLAMP_TO_EDGE` (§8.3) | 1 | largest / least certain | medium | yes | screened by the same test |
| 3 | Native texture formats instead of RGBA8888 (§8.4) | 1 | moderate–large | low | yes | confidence lowered, §8.10 |
| 4 | `GL_EXT_discard_framebuffer` (§8.5) | 2 | **measured: none (§8.10)** | very low | device-specific | done, no gain |
| 5 | Conditional `mod()` in 1-cycle variants (§8.5) | 2 | small | very low | yes | below the floor, deferred |
| 6 | `int` → `vec2` size uniforms (§8.5) | 2 | small | none | yes | below the floor, deferred |
| 7 | VBO ring buffer (§8.6) | 3 | unsized | medium | yes | unsized, needs a draw-call count |
| 8 | Program binary cache (§8.6) | 3 | hitching only | low | device-specific | not attempted |
| 9 | NEON for the three scalar audio routines (§8.6) | 3 | heat, not FPS | low | yes | still live — see §8.11 |

The one measurement to take next is `gSettings.TextureFilter` 0 vs 1 on the report 9 harness.
It needs no code, and its outcome decides items 1, 2 and 3 together — see §8.9.

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
- **§8.6 (VBO) cannot be sized at all** without a draw-call-per-frame count.

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
