# Report 3 — Dependency & Build System

## 3.1 The good news about cross-compiling

**There is no x86-hostile code in the CMake system.** The only architecture-specific flag in the
whole tree is `soh/CMakeLists.txt:560-561`:

```cmake
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CPU_OPTION -msse2 -mfpmath=sse)
endif()
```

which correctly evaluates empty on armhf. No `CMAKE_SIZEOF_VOID_P` checks, no `-m64`, no
32-vs-64 branching. The work is **additive** — new code paths, not removing hostile ones.

**The host-tool problem is already solved by the project.** `ZAPD` (`ZAPDTR/ZAPD/CMakeLists.txt:265`)
is the only in-tree binary that gets executed during a build, and only by three targets in the
root `CMakeLists.txt` that are **not part of `ALL`**:

- `ExtractAssets` (`CMakeLists.txt:199-213`)
- `ExtractAssetHeaders` (`:216-222`)
- `GenerateSohOtr` (`:225-238`)

A plain `cmake --build .` in a cross tree will never try to execute an armhf `ZAPD` on the x86_64
host. The project's own CI already splits this: `.github/workflows/generate-builds.yml` job
`generate-soh-otr` (lines 10-75) builds `soh.o2r` once natively and hands it to the other jobs as
an artifact; `docs/BUILDING.md:301-341` documents the same two-tree pattern for Switch and Wii U.

`ZAPD` is still *cross-built* and installed (`CMakeLists.txt:186` → `./assets/extractor`) because
end users need to extract their own ROM on the device. That is intentional — it just must not be
*run* during the cross build.

## 3.2 Versions and standards

| | |
|---|---|
| `cmake_minimum_required` | **3.26.0** (root `CMakeLists.txt:1`, `soh/CMakeLists.txt:1`); libultraship asks 3.24, OTRExporter 3.16 |
| C++ standard | C++20 |
| C standard | C23 (root), but `libultraship` pins its own target to C11 (`libultraship/src/CMakeLists.txt:7`, "needed for libgfxd `_Alignas`") |
| Target gcc | Debian 13 armhf ships **gcc 14.2.0** (verified on device) — comfortably covers C23/C++20 |

**Note on your build host:** you have **CMake 4.4.3**. CMake 4.x removed compatibility with
`cmake_minimum_required(VERSION <3.5)`. The repo's own minimums are fine, but FetchContent'd
dependencies may declare lower minimums and fail. If that bites, set
`CMAKE_POLICY_VERSION_MINIMUM=3.5` for the affected subprojects rather than editing them.

## 3.3 Dependency inventory

| Dependency | How obtained | Pin | Cross-compile note |
|---|---|---|---|
| **SDL2** | `find_package(SDL2 REQUIRED)` — `libultraship/cmake/dependencies/linux.cmake:2`, `soh/CMakeLists.txt:297` | system | **No FetchContent fallback on Linux.** Device has runtime `libsdl2 2.32.4`; `libsdl2-dev` is in apt but **not installed** |
| SDL2_net | optional, gated by `BUILD_REMOTE_CONTROL` (off by default) | system | skip |
| OpenGL/GLES | `find_package(OpenGL QUIET)` (`common.cmake:3`); GLES linkage at `libultraship/src/CMakeLists.txt:97-107` and `linux.cmake:5-10` | system | see §3.4 |
| GLEW | Windows/macOS only | — | **not used on Linux** — `libglew-dev` in `.github/workflows/apt-deps.txt` is a dead entry for Linux |
| ImGui | FetchContent + local patch | `v1.91.9b-docking` | network at configure time |
| StormLib | FetchContent + patch (`INCLUDE_MPQ_SUPPORT` is ON at `CMakeLists.txt:163`) | `v9.25` | declared twice (also `OTRExporter/OTRExporter/CMakeLists.txt:158-161`, unpatched — first declaration wins). x86 asm in libtomcrypt/libtommath is `__i386__`/`__x86_64__`-gated with portable fallbacks — **ARM-clean** |
| stb_image.h | **raw `file(DOWNLOAD)`** at configure time, unconditional (`common.cmake:55`) | pinned commit | no FetchContent caching semantics; hard failure with no network |
| libgfxd | FetchContent (`GFX_DEBUG_DISASSEMBLER` ON at `CMakeLists.txt:156`) | pinned commit | also vendored separately in `ZAPDTR/lib/libgfxd` — two copies |
| thread-pool | FetchContent | `v4.1.0` | header-only |
| prism | FetchContent | pinned commit | text templating; spot-check once populated |
| dr_libs | FetchContent (`soh/CMakeLists.txt:290-295`) | pinned commit | header-only |
| nlohmann_json, tinyxml2, spdlog, libzip | `find_package(... REQUIRED)` on Linux | system | **no Linux fallback** (Android has one) — all exist as Debian armhf packages |
| libpng | `find_package(PNG REQUIRED)` (`ZAPDTR/ZAPD/CMakeLists.txt:325`) | system | needed even in the cross tree, since ZAPD is cross-built |
| ogg / vorbis / opus / opusfile | custom Find modules in `CMake/Find*.cmake` | system | |
| gamecontrollerdb.txt | `execute_process(curl ...)` at configure time (`soh/CMakeLists.txt:726-727`) | HEAD of master | network at configure time |
| linuxdeploy | `file(DOWNLOAD)` at **cpack** time, **hardcoded `linuxdeploy-x86_64.AppImage`** (`CMake/Packaging.cmake:63`) | `1-alpha-20240109-1` | runs on the build host; fine for a cross build, wrong for native-on-device packaging |

**Configure-time network dependencies** (ImGui, StormLib ×2, stb_image.h, thread-pool, prism,
dr_libs, gamecontrollerdb) are all target-arch libraries — none are host tools — but they do
require network at configure time. Pre-seed with `FETCHCONTENT_SOURCE_DIR_<NAME>` if you want an
offline/reproducible cross build.

**Device status:** every required `-dev` package (`libsdl2-dev`, `libpng-dev`, `libzip-dev`,
`nlohmann-json3-dev`, `libtinyxml2-dev`, `libspdlog-dev`, `libogg-dev`, `libopus-dev`,
`libopusfile-dev`, `libvorbis-dev`, `cmake`, `ninja-build`) is **available in apt but not yet
installed**. Native on-device builds are therefore possible after one `apt install` — worth
knowing as a fallback when a cross-sysroot issue is eating time.

## 3.4 GLES detection — already exists, and it is the Pyra hook

`libultraship/src/CMakeLists.txt:10`:
```cmake
option(USE_OPENGLES "Enable GLES3" OFF)
```
`:97-107` links `GLESv3` on Android, `OpenGL::GLES2` on OpenBSD, and plain `GLESv2` via bare
`find_library` everywhere else. `:179` propagates `USE_OPENGLES` as a compile definition.
`linux.cmake:5-10` links `${OPENGL_GLESv2_LIBRARY}` into ImGui and defines
`IMGUI_IMPL_OPENGL_ES3`.

Two problems with that Linux path for this device:

1. `${OPENGL_GLESv2_LIBRARY}` is only populated if `find_package(OpenGL)` (called `QUIET` at
   `common.cmake:3` with no `COMPONENTS`) locates GLES — it needs `libgles2-mesa-dev`/EGL dev
   data present. Worth requesting components explicitly.
2. **Whatever it finds will be libglvnd, not the DDK** — see §3.5, which is the more important
   problem.

Casing is inconsistent between `linux.cmake` (`OPENGL_GLESv2_LIBRARY`) and `openbsd.cmake`
(`OPENGL_gles2_LIBRARY`); normalise if you touch these.

## 3.5 The deployment trap — glvnd will shadow the DDK

| | |
|---|---|
| DDK | `/opt/omap5-sgx-ddk-um-linux/lib/libEGL.so.1`, `libGLESv2.so.1` — SONAMEs `libEGL.so.1` / **`libGLESv2.so.1`** |
| System | `/usr/lib/arm-linux-gnueabihf/libGLESv2.so.**2**` (libglvnd 1.7.0), `libEGL.so.1` (glvnd) |
| glvnd EGL vendors | **only** `/usr/share/glvnd/egl_vendor.d/50_mesa.json` |
| `ld.so.conf.d` | lists `/opt/omap5-sgx-ddk-um-linux/lib/` **after** `/usr/lib/arm-linux-gnueabihf` |

>  **Superseded by report 5 §5.1, which measured this on the device.** The prediction below
>  that SDL would silently fall back to Mesa llvmpipe was **wrong** — there is no working GLX on
>  this system, so the desktop path hard-fails instead. The remedy was right; the mechanism was
>  not, and the minimum environment turned out to be **two** variables, not four.

SDL2 dlopens `libGLESv2.so.2` by default. The SONAME mismatch (`.so.1` vs `.so.2`) means the DDK
can never win by link order.

**Fix is launcher configuration, not code.** Measured minimum:

```sh
export SDL_VIDEO_X11_FORCE_EGL=1
export SDL_VIDEO_GL_DRIVER=/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1
```

`SDL_VIDEO_EGL_DRIVER` is **not** required — the system glvnd `libEGL.so.1` routes to the DDK on
its own. See report 5 §5.1 for the full test matrix, including the dangerous middle case where a
context is created but every GL entry point resolves to NULL.

## 3.6 Existing non-desktop platform paths (the template)

| Platform | Mechanism |
|---|---|
| Nintendo Switch | `CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch"` branches (root `CMakeLists.txt:114`; `soh/CMakeLists.txt:604,621,672,738,747`; `ZAPDTR/ZAPD/CMakeLists.txt:417-425`), external `/opt/devkitpro/cmake/Switch.cmake` toolchain, `nx_create_nro` |
| Wii U | `CMAKE_SYSTEM_NAME STREQUAL "CafeOS"` branches (`soh/CMakeLists.txt:375-389,539-558,682-692,762-774`), external `WiiU.cmake`, `wut_create_rpx` |
| Android | `libultraship/cmake/dependencies/android.cmake` + `CMAKE_SYSTEM_NAME STREQUAL "Android"`, external NDK toolchain |
| macOS arm64 | no CMake branch; `-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"` from CI |

There is **no `arm`, `aarch64`, `armv7` or `NEON` string in any CMakeLists.txt or .cmake file in
the tree**. The Switch/Wii U shape — *external toolchain file + a `CMAKE_SYSTEM_*` branch +
assets generated in a separate native tree* — is the right template.

`soh/CMake/Linux32bit-toolchain.cmake` is the only in-tree toolchain file. It is **dead,
unreferenced, and i386** (`-m32`, `/lib/i386-linux-gnu`), with no `CMAKE_SYSTEM_NAME` /
`CMAKE_SYSTEM_PROCESSOR` and `CMAKE_FIND_ROOT_PATH` commented out. Useful only as a skeleton.

## 3.7 Packaging

- Linux generator is `External` (root `CMakeLists.txt:291`) → `CMake/Packaging.cmake`'s
  `CPACK_EXTERNAL_PACKAGE_SCRIPT` → downloads and runs `linuxdeploy-x86_64.AppImage`.
- `CMake/Packaging.cmake:34-35` derives the package architecture from
  `execute_process(COMMAND dpkg --print-architecture)`, which always reports the **build host's**
  architecture. A cross build on x86_64 would mislabel the package `amd64`. Needs an override.
- DEB/RPM generators exist in `CMake/Packaging-2.cmake:9-12` but no workflow exercises them.

For the Pyra, an AppImage is the wrong shape anyway: the app must link against a device-specific
proprietary driver at `/opt/omap5-sgx-ddk-um-linux/lib`, which a bundled AppImage would fight.
A plain tarball or a `.deb` with a launcher script that sets the SDL EGL variables from §3.5 is
the correct packaging. See plan Phase 6.

## 3.8 Minimum changes required to cross compile

1. A real toolchain file: `CMAKE_SYSTEM_NAME Linux`, `CMAKE_SYSTEM_PROCESSOR armv7l`,
   `arm-linux-gnueabihf-{gcc,g++}`, `CMAKE_FIND_ROOT_PATH` → armhf sysroot,
   `FIND_ROOT_PATH_MODE_PROGRAM NEVER / LIBRARY ONLY / INCLUDE BOTH`,
   `PKG_CONFIG_LIBDIR`/`PKG_CONFIG_SYSROOT_DIR`.
2. An armhf sysroot with the `-dev` packages from §3.3 (`dpkg --add-architecture armhf`, or
   `debootstrap`, or rsync one from the device — the device already has all the runtime libs and
   the DDK headers under `/opt/omap5-sgx-ddk-um-linux/include`).
3. Generate `soh.o2r` / `oot.o2r` in a **separate native x86_64 tree** and copy into
   `<pyra-build>/soh/`. Never run `ExtractAssets`/`GenerateSohOtr` in the cross tree.
4. Nothing else. The CMake system does not need to be un-broken; it needs to be extended.

Note: you have Docker on the build host and no `arm-linux-gnueabihf-gcc` installed. A
`debian:trixie` container with `crossbuild-essential-armhf` and an armhf sysroot is the
lowest-friction route and gives you a reproducible recipe to hand upstream.
