# Report 6 — Phase 1: ARMv7 Cross Build

## 6.1 What was added

| File | Purpose |
|---|---|
| `scripts/pyra/Dockerfile` | Debian 13 (trixie) cross container — `crossbuild-essential-armhf` plus armhf multiarch dependencies |
| `scripts/pyra/build.sh` | Wrapper: `image` / `configure` / `build` / `shell` |
| `CMake/toolchains/pyra-armhf.cmake` | `arm-linux-gnueabihf` toolchain, Cortex-A15 + NEON tuning |
| `CMakeLists.txt` | `option(PYRA ...)` preset + `PYRA_DDK_ROOT` cache path |
| `soh/CMakeLists.txt` | ARMv7 branch beside the existing `x86_64` one, enabling NEON |

Nothing in `libultraship` was touched. Per the agreed split (plan §4.0), the device-specific
bits live in Shipwright and the renderer work stays generic for Phase 3.

## 6.2 Why Debian trixie in a container rather than a rsynced sysroot

The Pyra runs Debian 13, so the armhf multiarch packages in a `debian:trixie` container are the
**same versions the device runs**. Verified:

| | Container | Device |
|---|---|---|
| gcc | 14.2.0 | 14.2.0 |
| SDL2 | 2.32.4 | 2.32.4 |

That removes the need to rsync a general sysroot. The **one** exception is the proprietary
PowerVR SGX driver, which is not in Debian — `~/.cache/pyra-ddk` (4.1 MB, rsynced from
`/opt/omap5-sgx-ddk-um-linux`) is bind-mounted read-only. It is deliberately **not** committed:
it is redistributable only under TI/Imagination's terms.

```sh
rsync -a pyra@<host>:/opt/omap5-sgx-ddk-um-linux/ ~/.cache/pyra-ddk/
./scripts/pyra/build.sh image
./scripts/pyra/build.sh configure
./scripts/pyra/build.sh build
```

## 6.3 NEON is the flag that matters

Debian's armhf baseline is `-march=armv7-a+fp`, which does **not** define `__ARM_NEON`. That
macro is what selects `aMixImplNEON` over the scalar audio mixer
(`soh/soh/mixer.c:689`, dispatch at `:358-365`). Without it the NEON path silently compiles out
and nothing warns you.

Set in two places on purpose:
- the toolchain file, for cross builds;
- `soh/CMakeLists.txt`, so a **native on-device** build does not quietly lose it.

The second is written generically (`CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7"`), not gated on
`PYRA`, so it benefits any 32-bit ARM Linux target and is upstreamable as-is.

Verified in the container:
```
$ echo | arm-linux-gnueabihf-gcc -dM -E -march=armv7-a+neon-vfpv4 -mfloat-abi=hard - | grep __ARM_NEON
#define __ARM_NEON__ 1
#define __ARM_NEON 1
```

## 6.4 The GLES linkage trap — measured, and why `PYRA_DDK_ROOT` exists

This is the most important thing Phase 1 turned up, and it invalidates the obvious approach.

libultraship calls GL entry points **directly** (`GL_GLEXT_PROTOTYPES` + a direct
`target_link_libraries(... GLESv2)` at `libultraship/src/CMakeLists.txt:97-107`), while SDL
separately `dlopen`s a GLES library to create the context. **Those two must resolve to the same
library.** On the Pyra they do not by default:

| Linked against | `DT_NEEDED` | Result on device |
|---|---|---|
| glvnd `libGLESv2.so` — what `find_library(GLESv2)` finds | `libGLESv2.so.2` | **shader compilation fails, empty info log** |
| DDK `libGLESv2.so` | `libGLESv2.so.1` | works — 533 fps at 640×360 |

So a stock `-DUSE_OPENGLES2=ON` build would link glvnd, compile cleanly, and then fail at
runtime in a way that looks like a shader bug rather than a linkage bug. `PYRA_DDK_ROOT` exists
so the Pyra preset can point the GLES lookup at the DDK instead. It is wired up in Phase 3, when
`USE_OPENGLES2` lands.

## 6.5 Result

`soh.elf` builds and links for armhf, and runs on the device.

```
$ file build-pyra/soh/soh.elf
ELF 32-bit LSB pie executable, ARM, EABI5, dynamically linked,
interpreter /lib/ld-linux-armhf.so.3, for GNU/Linux 3.2.0

Tag_CPU_name: "7-A"          Tag_CPU_arch: v7
Tag_FP_arch: VFPv4           Tag_Advanced_SIMD_arch: NEONv1 with Fused-MAC
Tag_ABI_VFP_args: VFP registers
```

On the Pyra, **all 16 `DT_NEEDED` libraries resolve with zero missing** — the container's
Debian trixie versions match the device's exactly, which is the whole point of §6.2.

Running it:

```
[warning] [ArchiveManager.cpp:228] The archive at path ./soh.o2r does not exist
Failed to initialize OpenGL loader!
```

That is the intended Phase 1 stopping point: ARMv7 code executes, spdlog and the resource layer
initialise, and the only thing in the way is the renderer — which is Phase 2/3. There is no
desktop GL on this device (report 5 §5.1), so the desktop loader failing is correct behaviour.

### One real bug found and fixed

The cross build caught an upstream defect that no x86 or aarch64 build can hit: the NEON audio
mixer uses **AArch64-only intrinsics behind an `__ARM_NEON` guard**, so it has never compiled on
32-bit ARM. Fixed in `soh/soh/mixer.c`; full write-up and the fix in report 1 §1.1
("Correction (Phase 1)"). This is worth sending upstream on its own — it affects every armhf
target, not just the Pyra.

## 6.6 Confirmations of earlier audit findings

- **`lsb_release` is a build-host dependency.** `CMake/Utils.cmake:235-248` hard-errors without
  it (report 3 §3.7 flagged this). Added to the container.
- **ZAPD cross-builds but is never executed** during a normal build, exactly as report 3 §3.7
  predicted — no host/target tool split was needed.
- **FetchContent needs network at configure time** (ImGui, StormLib, stb, prism, thread-pool,
  dr_libs, plus a `curl` for `gamecontrollerdb.txt`). Fine interactively; an offline CI would
  need `FETCHCONTENT_SOURCE_DIR_*` pre-seeding.

## 6.7 Known gaps at the end of Phase 1

- The build uses the **desktop GL path**, not GLES. That is intentional: `USE_OPENGLES` in this
  tree means GLES**3**, which this GPU does not have, and the renderer is replaced wholesale in
  Phase 3. Phase 1's job is to prove the toolchain and the ~3,400 non-renderer sources.
- No assets are generated here. `soh.o2r` / `oot.o2r` must come from a native tree and be copied
  into `build-pyra/soh/`.
- Packaging (`dpkg --print-architecture` mislabelling, AppImage vs `.deb`) is untouched — Phase 6.
