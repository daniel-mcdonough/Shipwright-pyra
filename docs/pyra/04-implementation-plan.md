# Report 4 — Recommended Implementation Plan

## 4.0 One design pushback before the phases

You proposed a single `-DPYRA=ON` that enables ARMv7 tuning + a GLES2 backend + Pyra defaults.
**I'd split that**, because as specified it works against your own "keep changes upstreamable"
constraint:

- Upstream libultraship will not accept a `PYRA` option. A device name in a general-purpose
  library is exactly the kind of thing that gets rejected in review, and it would strand the
  GLES2 work in your fork permanently.
- The three things it bundles are genuinely orthogonal and belong in three different places:
  ARM tuning belongs in a **toolchain file** (it is a property of the compiler, not the project),
  the GLES2 backend belongs in **libultraship as `USE_OPENGLES2`** (which upstream PR #1288
  already names), and the Pyra defaults belong in **soh**.

Recommended shape instead:

| Layer | Mechanism | Upstreamable? |
|---|---|---|
| ARMv7/NEON/Cortex-A15 flags, sysroot | `CMake/toolchains/pyra-armhf.cmake` | yes — mirrors the Switch/Wii U external-toolchain pattern (`docs/BUILDING.md:301-341`) |
| GLES2 renderer | `USE_OPENGLES2` in libultraship | **yes — this is literally upstream PR #1288** |
| Conservative low-end defaults | `option(PYRA ...)` in **Shipwright root only** | fork-local, small, and harmless |
| Developer convenience | `CMakePresets.json` entry `pyra` | yes |

So `-DPYRA=ON` still exists and still does what you want — it just becomes a thin preset that
forwards to orthogonal flags, rather than the thing that implements them:

```cmake
# root CMakeLists.txt
option(PYRA "Preset: DragonBox Pyra (OMAP5432 / SGX544 / GLES2) defaults" OFF)
if (PYRA)
    set(USE_OPENGLES2 ON CACHE BOOL "" FORCE)   # implies USE_OPENGLES upstream
    add_compile_definitions(SOH_LOWEND_DEFAULTS)
    # NOTE: no -march/-mtune here. That is the toolchain file's job, so that
    # -DPYRA=ON also works for a native on-device build with plain gcc.
endif()
```

If you disagree and want the mega-flag, say so and I'll build it that way — it's your fork and
the difference is maybe 30 lines. But I think you'd regret it at upstreaming time.

## 4.1 Phase 0 — Measure before building (NEW, ~half a day)

Not in your original list, but everything downstream depends on it. All of this happens on the
**existing x86_64 build**, which already works.

1. **Varying-budget histogram.** Log `(numInputs, o_alpha, o_fog, o_grayscale, clamp count)` at
   `gfx_opengl.cpp:463-472` each time a shader variant is created. Play title screen → Kokiri
   Forest → Hyrule Field → a dungeon → Hyrule Castle Town. Dump the distribution.
   **Decision point:** if the p100 fits in 8 varying vectors, §2.4 is a non-issue and the port is
   mostly mechanical. If it doesn't, we know exactly how many variants need the uniform-migration
   fix (§2.4 mitigation 3) before anything else is worth doing.
2. **Confirm the SGX driver can actually be reached from SDL2.** On the device:
   `apt install libsdl2-dev`, build a ~40-line SDL2 program that creates an ES2 context and
   prints `GL_RENDERER`, and find the exact environment-variable combination from §3.5 that makes
   it print `PowerVR SGX 544MP` instead of a Mesa string. **If this cannot be made to work, the
   whole port is blocked and we should know on day one, not in Phase 3.**
3. **Baseline the CPU side.** Cross-build (or native-build) *just* `ZAPD` for armhf. It is a
   small, self-contained C++20 target with the same dependency shape as `soh`, so it smoke-tests
   the toolchain, the sysroot and the C++20 support without waiting on the renderer.

Deliverable: a go/no-go on the two real risks, plus a working armhf toolchain.

## 4.2 Phase 1 — Minimal ARMv7 build

- `CMake/toolchains/pyra-armhf.cmake` — modelled on `soh/CMake/Linux32bit-toolchain.cmake:1-15`
  but with real cross fields (§3.8 item 1) and the flags from report 1 §1.7.
- Sysroot: simplest path is to rsync `/usr`, `/lib` and `/opt/omap5-sgx-ddk-um-linux` off the
  device after `apt install`ing the `-dev` packages listed in §3.3. That guarantees the sysroot
  matches the actual runtime, including the DDK headers.
- Docker recipe (`debian:trixie` + `crossbuild-essential-armhf`) so the build is reproducible and
  hand-offable. You have Docker; you do not currently have `arm-linux-gnueabihf-gcc`.
- Generate `soh.o2r` in a separate native tree; copy into `<pyra-build>/soh/`.
- Add the ARM branch beside `soh/CMakeLists.txt:560-561` if any tuning needs to live in-project
  rather than in the toolchain file.
- Delete or clearly retitle `soh/CMake/Linux32bit-toolchain.cmake` (report 1 §1.8 item 1).

**Exit criteria:** `soh.elf` links for armhf and runs far enough to print a version banner.
Do not expect a window yet.

Commits: `toolchain: add arm-linux-gnueabihf cross file`, `build: ARM tuning flags`,
`docs: cross-compiling for armhf`.

## 4.3 Phase 2 — Window + EGL

- Apply the Phase 0.2 environment findings as a launcher script, not as code.
- Add a startup log of `GL_VENDOR` / `GL_RENDERER` / `GL_VERSION` / `GL_SHADING_LANGUAGE_VERSION`
  and a loud warning if `GL_RENDERER` does not contain `PowerVR`. This is cheap insurance against
  silently benchmarking llvmpipe, and is useful enough to be upstreamable on its own.
- `gfx_sdl2.cpp:333-344` — request `SDL_GL_CONTEXT_PROFILE_ES` / 2.0 under `USE_OPENGLES2`
  (this is PR #1288's `gfx_sdl.cpp` change, ported to the SDL2 file).
- Drop the `SDL_GL_STENCIL_SIZE` request if we confirm stencil is genuinely unused
  (no `glStencilFunc`/`glStencilOp` exists in the backend — see report 2 row 11).

**Exit criteria:** a window opens, an ES 2.0 context is current, `GL_RENDERER` says
`PowerVR SGX 544MP`. Rendering may be garbage.

## 4.4 Phase 3 — Renderer compiles against GLES2

**Do not write this from scratch.** Backport upstream PR #1288 (`dbb0435`, 9 files, +280/−33)
onto the SDL2 fork. Only ~5 lines touch SDL, so the backport is mechanical. Explicitly **do not**
take the SDL3 migration in the same step — that is 43 commits of unrelated churn and would make
any GLES2 bug indistinguishable from an SDL3 regression.

Land it as a series of small commits mirroring the PR's own structure so the provenance stays
legible and so the deltas we add on top are separable:

1. `build: add USE_OPENGLES2 option`
2. `fast3d: GLES2 headers and context`
3. `fast3d: GLSL ES 1.00 shader dialect`
4. `fast3d: quad-based framebuffer blit for GLES2`
5. `fast3d: depth/stencil attachment split for GLES2`
6. `imgui: GLES2 backend selection`

**Exit criteria:** links against `libGLESv2`, shaders compile, something recognisable is on screen.

## 4.5 Phase 4 — Replace what PR #1288 does not cover

These are the four device-driven deltas from report 2 §2.7. Each is a separate commit, and each
is a candidate to push back to PR #1288 as a review comment — this is where the Pyra earns its
place as the PR's hardware validation:

1. **`GL_EXT_frag_depth` absent** (report 2, row 18). PR #1288 emits `gl_FragDepthEXT` behind
   `#extension GL_EXT_frag_depth : enable`; this device has no such extension, so every
   `o_prim_depth` variant fails to compile. Add a third case that omits the depth write.
   *This is a concrete bug in the PR that only real hardware could have found.*
2. **`glReadPixels` format** (row 10). `GL_UNSIGNED_SHORT_5_5_5_1` is not a legal readback pair
   here — the device reports `BGRA` / `UNSIGNED_BYTE`. Read `RGBA`/`UNSIGNED_BYTE`, repack on CPU.
3. **`GL_MIRROR_CLAMP_TO_EDGE`** (row 12). Absent. Emulate in-shader, or fall back to
   `GL_MIRRORED_REPEAT` with a documented visual caveat.
4. **Varying budget** (§2.4), scoped by the Phase 0 histogram. Expected order: hand-pack clamps
   and texcoords first; migrate per-draw-constant combiner inputs from attributes to uniforms if
   the tail demands it; degrade `o_grayscale` as a last resort.

Also in this phase: the defensive rewrite of non-constant uniform-array indexing in
`default.shader.fs:75-84` (report 2 §2.5) — cheap, and it removes a known PowerVR compiler
weak spot before it costs a debugging session.

**Exit criteria:** the game renders correctly at a low internal resolution. Frame rate irrelevant.

## 4.6 Phase 5 — Optimise for SGX544

Only start measuring here, and only against a confirmed-hardware `GL_RENDERER`.

Ordered by expected value:

1. **`GL_EXT_discard_framebuffer`** (present). On a tile-based deferred renderer, discarding
   depth/stencil at end-of-frame avoids a full-resolution tile writeback every frame. This is
   the highest-value, lowest-risk SGX-specific change available.
2. **Program binary cache** via `GL_OES_get_program_binary` / `GL_IMG_program_binary` (both
   present). Fast3D compiles shader variants lazily at runtime and the SGX GLSL compiler is slow;
   a persistent on-disk cache removes most first-encounter hitching. Broadly useful beyond the
   Pyra, so upstreamable.
3. **Conservative defaults** (the `SOH_LOWEND_DEFAULTS` from §4.0): internal resolution 640x360
   or 800x450 against the 1280x720 panel, MSAA off (already forced to 1), framebuffer effects
   off, widescreen off, HD textures off, three-point filtering considered for disabling — it is
   4 texture fetches per sample (`default.shader.fs:66-73`) and fill rate is the budget here.
4. **CPU side:** confirm `__ARM_NEON` is actually defined so `aMixImplNEON`
   (`soh/soh/mixer.c:689`) is live. Audio mixing on a 2-core A15 is not free. Also consider
   pinning the audio thread away from the render thread — there are only two cores.
5. **Governor.** The device idles at 1.125 GHz under `ondemand` with a 1.5 GHz ceiling. The
   launcher should at minimum document (or optionally set) `performance` while the game runs.

**Reality check on the target.** 2 × Cortex-A15 @ 1.5 GHz and an SGX544MP2 against a
640x360 N64-workload render is a reasonable ask. 20 FPS minimum looks achievable; 30 FPS is
plausible but will depend mostly on fill rate and draw-call count, not on the CPU. The existing
`MAX_TRI_BUFFER = 256` batching (`interpreter.cpp:107`) is the same lightweight scheme that ran
on Wii U, so the CPU-side submission cost is already in the right ballpark. I'd want Phase 0's
numbers before committing to 30.

## 4.7 Phase 6 — Packaging

- **Not an AppImage.** The binary must link against a device-specific proprietary driver at
  `/opt/omap5-sgx-ddk-um-linux/lib`; bundling fights that. A `.deb` (or plain tarball) that
  depends on `omap5-sgx-ddk-um-linux` is the honest shape.
- A launcher script that sets the SDL EGL environment from §3.5, verifies `GL_RENDERER`, and
  optionally sets the CPU governor.
- Fix `CMake/Packaging.cmake:34` so `dpkg --print-architecture` does not mislabel a cross-built
  package as `amd64` (report 3 §3.7).
- Leave `CMake/Packaging.cmake:63`'s `linuxdeploy-x86_64.AppImage` alone — it only runs on the
  build host.

## 4.8 Upstreaming strategy

Roughly in order of how welcome each will be:

| Change | Destination |
|---|---|
| Phase 4 fixes 1–4 | **review comments on PR #1288** — hardware-validation findings against an open PR are the most valuable thing here |
| `GL_RENDERER` startup log + software-rendering warning | libultraship, standalone |
| Program-binary shader cache | libultraship, standalone |
| `GL_EXT_discard_framebuffer` | libultraship, guarded by extension query |
| armhf toolchain file + cross-compile docs | Shipwright |
| `PYRA` preset + low-end defaults | Shipwright (fork-local is fine) |

Nothing here needs to be a permanent fork. The one thing that would create a fork is bundling
the SDL3 migration into the GLES2 work — hence the recommendation in §4.4 to keep them apart.

## 4.9 What I need from you before Phase 1

1. **The `-DPYRA=ON` shape** — do you accept the split in §4.0, or do you want the single
   mega-flag?
2. **Cross vs. native.** Cross-compile from your x86_64 Arch box in a Debian container, or
   `apt install build-essential cmake ninja-build` on the Pyra and build natively? Native is
   slower per iteration but removes the entire sysroot class of problems, and the device has
   4 GB (though **no swap**, so no LTO and probably `-j2`). Cross is the better long-term
   answer; native might be the faster way to get to a first window.
3. **Where these four reports should live** — `docs/pyra/` in the repo, or stay outside git?
