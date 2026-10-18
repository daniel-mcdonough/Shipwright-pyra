# Pyra Port — Audit Executive Summary

Audit date: 2026-09-20
Repo: `Shipwright` @ `feature/touchpad-bindings` (91abd3b), submodule `libultraship` @ `911fd17d`
Target verified live over SSH against the actual device (Debian 13 trixie, kernel 5.6.19-freq, armv7l).

> **This is an audit snapshot, not a status page.** It records what was true when the port was
> scoped. The device facts and GLES limits below are still accurate; some of the *recommendations*
> have since been tested and were wrong. [README.md](README.md) lists which claims across all the
> reports are still good and which are superseded — read it before acting on anything here.

## Verdict

**The port is viable and is a moderate effort, not a rewrite.** The CPU side is essentially
already clean. The rendering stack is the whole job, as you predicted — but less of it than
expected, for one reason you should know before reading anything else:

> **Upstream libultraship already has an open GLES 2.0 / GLSL ES 1.00 pull request.**
> [Kenix3/libultraship#1288](https://github.com/Kenix3/libultraship/pull/1288) by KiritoDv,
> opened 2026-09-11, **still open**, 1 commit, 9 files, **+280/-33 lines**.
> It adds `USE_OPENGLES2`, a `#version 100` shader dialect, a quad-based `glBlitFramebuffer`
> replacement, MSAA pinned to 1, and `IMGUI_IMPL_OPENGL_ES2`.
> Its own description states: *"Testing on actual GLES 2.0 hardware remains pending."*

So the strategic framing changes. This is not "write a GLES2 backend for SoH". It is
**"be the hardware validation for an existing upstream GLES2 backend, on the first real
GLES2-only device to run it"** — which is the most upstreamable position you could be in.

## The five findings that actually decide the plan

### 1. BLOCKER — `MAX_VARYING_VECTORS = 8` on SGX544, Fast3D can need ~11
Measured on device. The Fast3D shader generator emits, worst case:
7 × `vInput[1..7]` (vec4) + `vFog` (vec4) + `vGrayscaleColor` (vec4) + 2 × `vTexCoord` (vec2)
+ 4 × clamp floats. Even with GLSL ES 1.00's mandatory varying packing that is **~11 vectors
against a hard limit of 8**. `numInputs` is capped at 7 by `SHADER_INPUT_7`
(`libultraship/include/fast/interpreter.h:45-51`, computed at `src/fast/interpreter.cpp:4701-4709`).
High-input combiner variants **will fail to link** on this GPU.
PR #1288 does not address this — it was validated with `glslangValidator`, which does not
enforce a device's varying budget.
This is the single highest-risk item and it needs a measurement pass before any code is written
(see plan Phase 0).

### 2. BLOCKER — `GL_EXT_frag_depth` is **not present** on SGX544
PR #1288 resolves `frag_depth` to `gl_FragDepthEXT` behind `#extension GL_EXT_frag_depth : enable`.
The device's extension string (captured below) does not include it. Every shader variant with
`o_prim_depth` set will fail to compile on the Pyra. Needs a third path: drop the depth write
and accept the depth-sorting inaccuracy, which is what the N64 prim-depth mode costs.

### 3. BLOCKER (deployment, not code) — the DDK will be shadowed by Mesa software rendering
- DDK: `/opt/omap5-sgx-ddk-um-linux/lib/libEGL.so.1`, `libGLESv2.so.**1**` (SONAME `.so.1`)
- System: `/usr/lib/arm-linux-gnueabihf/libGLESv2.so.**2**` (libglvnd 1.7.0)
- The only glvnd EGL vendor registered is `/usr/share/glvnd/egl_vendor.d/50_mesa.json`.

SDL2 loads `libGLESv2.so.2` by default, which resolves to glvnd → Mesa → **llvmpipe**.
This is exactly the "GLES works via MESA not hardware" complaint on the Pyra forums. The fix is
environment/launcher configuration (`SDL_VIDEO_X11_FORCE_EGL`, `SDL_VIDEO_EGL_DRIVER`,
`SDL_VIDEO_GL_DRIVER` pointed at the DDK paths), not a code change — but if it is missed, every
performance number collected will be meaningless.

### 4. Good news — every other GLES2 concern is covered by an extension the device has
Confirmed present: `GL_OES_texture_npot`, `GL_OES_packed_depth_stencil`, `GL_OES_depth24`,
`GL_OES_rgb8_rgba8`, `GL_OES_required_internalformat`, `GL_OES_vertex_array_object`,
`GL_OES_depth_texture`, `GL_OES_fragment_precision_high`.
That removes the NPOT-wrap risk, the sized-internalformat risk, and the packed depth/stencil risk
that PR #1288 lists as its own open assumptions. PR #1288's `#define GL_RGBA8 GL_RGBA` workaround
is unnecessary here (harmless, keep it for other devices).

### 5. Good news — the CPU/ARM side is basically a non-event
No blockers found. Pointers already go through `uintptr_t` everywhere (`gSegments`,
`Gfx`/`Gwords`), SSE/AVX paths are correctly gated, **a hand-written NEON audio mixer already
exists** (`soh/soh/mixer.c:689`, `aMixImplNEON`), there is no inline asm outside an
aarch64-gated CRC32 block, and endianness goes through compiler builtins. The only x86 CMake
flag (`-msse2 -mfpmath=sse`) is already guarded by `CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64"`.
See report 01 for the WARN-level items.

## Measured device facts (all captured live, not assumed)

| | |
|---|---|
| SoC / CPU | OMAP5432, 2 × Cortex-A15 (`0xc0f`), max 1.5 GHz, `ondemand`, currently 1.125 GHz |
| CPU features | `neon vfpv3 vfpv4 idiva idivt vfpd32 lpae thumbee` |
| RAM | 4 GB, **swap 0** |
| OS / kernel | Debian 13 trixie, 5.6.19-freq, armhf |
| Native toolchain | gcc/g++ 14.2.0, binutils 2.44, default `-march=armv7-a+fp -mfloat-abi=hard -mthumb` |
| `cmake` / `ninja` on device | **not installed** (both available in apt) |
| GPU | `GL_RENDERER = PowerVR SGX 544MP` |
| GL | `OpenGL ES 2.0 build 1.14@3699939`, `GLSL ES 1.00` — **no GLES3 path exists** |
| EGL | 1.4, Imagination Technologies, `EGL_CLIENT_APIS = OpenGL_ES` only |
| Driver | `omap5-sgx-ddk-um-linux 1.14.3699939-15`, kernel module `pvrsrvkm_omap5_sgx544_116` |
| Window system | X11 (`Xorg` + `xserver-xorg-video-armsoc-omap5`), `/etc/powervr.ini` → `libpvrDRI3WSEGL.so` |
| Panel | 1280x720 (native 720x1280, rotated) |
| Also installed | `gl4es` 1.1.7.1 (GL→GLES2 wrapper), Mesa 25.0.7 (software fallback) |

### Measured GLES2 limits

| Limit | Value | Comment |
|---|---|---|
| `MAX_VARYING_VECTORS` | **8** | **the blocker** — see finding 1 |
| `MAX_FRAGMENT_UNIFORM_VECTORS` | 64 | fine (Fast3D uses ~13 + 6 samplers) |
| `MAX_VERTEX_UNIFORM_VECTORS` | 128 | fine |
| `MAX_VERTEX_ATTRIBS` | 16 | **exactly** the worst-case Fast3D attribute count |
| `MAX_TEXTURE_IMAGE_UNITS` | 8 | fine (6 samplers max) |
| `MAX_VERTEX_TEXTURE_IMAGE_UNITS` | 8 | vertex texture fetch available |
| `MAX_TEXTURE_SIZE` / `MAX_RENDERBUFFER_SIZE` | 4096 | fine |
| `IMPLEMENTATION_COLOR_READ_FORMAT` | `GL_BGRA_EXT` (0x80E1) | **not** `RGBA` — affects `ReadFramebufferToCPU` |
| `IMPLEMENTATION_COLOR_READ_TYPE` | `GL_UNSIGNED_BYTE` | `UNSIGNED_SHORT_5_5_5_1` readback is invalid here |

### Full extension string (device, GLES2 context)

```
GL_OES_rgb8_rgba8 GL_OES_depth24 GL_OES_vertex_half_float GL_OES_texture_float
GL_OES_texture_half_float GL_OES_element_index_uint GL_OES_mapbuffer
GL_OES_fragment_precision_high GL_OES_compressed_ETC1_RGB8_texture GL_OES_EGL_image
GL_OES_EGL_image_external GL_OES_required_internalformat GL_OES_depth_texture
GL_OES_get_program_binary GL_OES_packed_depth_stencil GL_OES_standard_derivatives
GL_OES_vertex_array_object GL_OES_egl_sync GL_OES_texture_npot GL_OES_surfaceless_context
GL_EXT_discard_framebuffer GL_EXT_blend_minmax GL_EXT_multi_draw_arrays
GL_EXT_multisampled_render_to_texture GL_EXT_shader_texture_lod
GL_EXT_texture_format_BGRA8888 GL_EXT_occlusion_query_boolean GL_EXT_texture_rg
GL_EXT_draw_buffers GL_EXT_shader_framebuffer_fetch GL_IMG_shader_binary
GL_IMG_texture_compression_pvrtc GL_IMG_texture_compression_pvrtc2 GL_IMG_texture_npot
GL_IMG_texture_format_BGRA8888 GL_IMG_read_format GL_IMG_program_binary
GL_IMG_uniform_buffer_object GL_IMG_multisampled_render_to_texture GL_KHR_debug
```

**Notably absent:** `GL_EXT_frag_depth`, `GL_NV_framebuffer_blit`, `GL_ANGLE_framebuffer_blit`,
`GL_OES_texture_3D`, anything GLES3.

**Notably present and worth exploiting** (see report 04, Phase 5):
- `GL_EXT_discard_framebuffer` — on a tile-based deferred renderer, discarding depth/stencil at
  end of frame avoids a full-resolution tile writeback. **Implemented and measured: no gain.**
  The estimate of ~140 MB/s of avoided writeback was right in magnitude and wrong in relevance —
  this workload is fragment-shader bound, not bandwidth starved. See report 8 §8.10.
- `GL_OES_get_program_binary` / `GL_IMG_program_binary` — Fast3D compiles shader variants lazily
  at runtime and the SGX GLSL compiler is slow. A persistent on-disk program-binary cache would
  remove most first-encounter hitching.
- `GL_EXT_shader_framebuffer_fetch` — could implement N64 blender modes that read framebuffer
  colour without a copy. Speculative, Phase 5 at the earliest.
