# Report 1 — Architecture Compatibility (ARMv7-A / armhf / Cortex-A15)

**Verdict: no blockers.** This codebase has already been through Android (arm/arm64),
Nintendo Switch (aarch64) and Wii U (32-bit PowerPC) ports, and the usual 64-bit-pointer
landmines were cleaned out in the process. 32-bit ARM Linux is a new *combination*, not new
*territory*.

## 1.1 x86 / SSE / AVX assumptions — all correctly gated

| Site | What it is | Status |
|---|---|---|
| `soh/soh/Extractor/FastCrc32C.c:7-40,81-140` | `cpuid` + `<nmmintrin.h>` + `_mm_crc32_u*` | **OK** — gated on `__x86_64__ \|\| __i386__`; on armhf falls through to `NO_CRC_INTRIN` → portable table impl (`:112-120`) |
| `soh/soh/Extractor/FastCrc32C.c:31-34` | `__asm__("crc32cx ...")` | **OK** — gated on `__aarch64__ && __ARM_FEATURE_CRC32`; skipped on armv7 |
| `soh/soh/mixer.c:359-361,609-687` | SSE2 / AVX2 audio mixer | **OK** — gated on `__SSE2__ \|\| _M_AMD64` |
| `soh/soh/mixer.c:689` | **`aMixImplNEON`, `#include <arm_neon.h>`** | ~~OK and a win~~ → **was a BLOCKER, now fixed.** See correction below |
| `soh/CMakeLists.txt:560-561` | `-msse2 -mfpmath=sse` | **OK** — inside `if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")`; evaluates empty on armhf |

No `xmmintrin.h` / `immintrin.h` / `emmintrin.h` / `_mm_` outside those two files.
No `-mavx`, `-march=x86-64`, `-m64` in any active CMake path.
No `.S` / `.asm` files anywhere in the tree. No naked functions.

**Action:** to get the NEON mixer you must actually define `__ARM_NEON`, i.e. compile with
`-mfpu=neon-vfpv4` (or `-march=armv7-a+neon-vfpv4`). Debian's default is `-march=armv7-a+fp`,
which does **not** enable NEON. This is the single most valuable ARM compile flag in the port.

### Correction (Phase 1) — the NEON mixer did not build on ARMv7 at all

The assessment above was wrong, and the cross build proved it. The block is guarded on
`__ARM_NEON`, which 32-bit ARMv7 defines once NEON is enabled — but the body uses
**`vmull_high_n_s16` and `vmull_high_s16`, which are AArch64-only (A64) intrinsics**
(`soh/soh/mixer.c:723-727`). On aarch64 (Switch, Apple Silicon) `__ARM_NEON` is always defined so
it compiles; on armhf it is a hard compile error:

```
soh/soh/mixer.c:723:39: error: implicit declaration of function 'vmull_high_n_s16'
soh/soh/mixer.c:723:39: error: incompatible types when initializing type 'int32x4_t' using type 'int'
```

So the guard is too broad upstream: this path has **never** been buildable on 32-bit ARM, and it
fails loudly rather than degrading to `aMixImplRef`.

Fixed by expressing the high-half forms with their exact AArch32 equivalents — a high-half
widening multiply *is* a widening multiply of the upper four lanes:

```c
#define SOH_VMULL_HIGH_N_S16(a, n) vmull_n_s16(vget_high_s16(a), (n))
#define SOH_VMULL_HIGH_S16(a, b)   vmull_s16(vget_high_s16(a), vget_high_s16(b))
```

Named distinctly rather than shadowing the intrinsics, so nothing collides if a toolchain later
provides them for AArch32. Confirmed working: `mixer.c.o` contains **192 NEON instructions**, and
`aMixImplNEON` is inlined into `aMixImpl`.

**This is an upstream bug, not a Pyra one** — it affects every 32-bit ARM Linux target
(Raspberry Pi 32-bit, other armhf handhelds). Worth submitting to Shipwright separately.

## 1.2 Pointer width — already `uintptr_t` clean

- `soh/include/variables.h:209` / `soh/src/code/main.c:23` — `uintptr_t gSegments[NUM_SEGMENTS];`
- `soh/include/macros.h:33-37` — `PHYSICAL_TO_VIRTUAL` / `VIRTUAL_TO_PHYSICAL` are
  add/subtract `0x80000000` round trips, which are modular-arithmetic-safe at any pointer width.
- `libultraship/include/libultraship/libultra/gbi.h:1678-1721` and
  `libultraship/include/fast/lus_gbi.h:1152-1154` — display-list words are `uintptr_t w0, w1`
  with `static_assert(sizeof(Gwords) == 2*sizeof(void*))`.
- `libultraship/src/fast/interpreter.cpp` (≈`:2678-3613`) and
  `src/fast/resource/factory/DisplayListFactory.cpp:45-59` — host pointers are stored in
  `words.w1` as `uintptr_t` and cast back consistently.
- `libultraship/include/libultraship/libultra/types.h:28-33` — the legacy
  `typedef u32 uintptr_t;` block is explicitly disabled under `#if 0` in favour of `<stdint.h>`.
  Clear evidence this was fixed deliberately.

Places that *do* truncate a pointer into a `u32` are all **lossless on ILP32** (they are latent
64-bit bugs, not ARM bugs):
`soh/include/z64.h:2128` (`UCodeDisas::segments[]`, filled at `soh/src/code/ucode_disas.c:144`),
`soh/src/overlays/actors/ovl_En_Horse/z_en_horse.c:2390`, and
`soh/include/z64.h:2168-2176` (dead — the assigning code in `soh/src/code/z_jpeg.c:45-50` is
inside `#if 0`).

## 1.3 Unaligned access — WARN, works in practice on Linux/ARMv7

ARMv7-A Linux runs with `SCTLR.A = 0`, so unaligned `LDR`/`LDRH` are handled in hardware and
the kernel alignment trap is off by default. These are technically UB but will not fault:

| Site | Pattern |
|---|---|
| `soh/src/code/z_room.c:235` | `BE32SWAP(*(u32*)data)` on an OTR-loaded byte buffer |
| `soh/src/code/audio_seqplayer.c:1351` | `BE16SWAP(*(u16*)(seqData + computed_offset))` — offset can be odd |
| `soh/src/code/z_lib.c:329-349` | `*(u16*/u32*/f32*)(actor_ptr + ichain->offset)` — offsets always land on aligned struct members by construction |
| `ZAPDTR/lib/libgfxd/gfxd.c:72,79,88` | unaligned word reads during display-list disassembly |

Counter-example showing the codebase is alignment-aware where it matters:
`soh/src/code/z_jpeg.c:106-115` (`Jpeg_GetUnalignedU16`) explicitly tests `((u32)ptr & 1) == 0`.

**Action:** none required for Phase 1. **Do not** build with `-mno-unaligned-access`, and do not
enable the kernel alignment trap. If SIGBUS or audio corruption ever appears on device, these
four sites are the list to convert to `memcpy`.

No `__attribute__((packed))` or `#pragma pack` anywhere in the tree — so there are no structs
forcing genuinely misaligned members into NEON/VFP loads.

## 1.4 Endianness — OK

`libultraship/include/ship/utils/binarytools/endianness.h` and `BitConverter.h:8-26` derive
`IS_BIGENDIAN` from `__BYTE_ORDER__`/`__ORDER_BIG_ENDIAN__` and use `__builtin_bswap*`.
`BitConverter`'s multi-byte reads (`:29-160`) are byte-wise with `std::memcpy` for float/double
reconstruction — alignment- and aliasing-safe. Vendored StormLib's `jenkins/lookup3.c:38-66`
gets byte order from glibc `<endian.h>`, correct on ARM.

## 1.5 `long` width / format strings — OK on this target

`u64`/`s64` are `long long`, not `long` (`libultraship/include/libultraship/libultra/types.h:14-15`),
so nothing assumes `long` is 64-bit. On Linux ILP32, `sizeof(long) == sizeof(size_t) == 4`, so
the `%lu`-with-`size_t` sites (`soh/soh/Enhancements/debugger/dlViewer.cpp:142,155,329`) and the
`(long)i >= 0` underflow check (`libultraship/src/fast/interpreter.cpp:98`) remain width-correct.
Style nit only (`%zu` would be better); not a functional issue.

## 1.6 Threading / memory model — OK

`std::atomic` use is limited to progress counters
(`soh/soh/OTRGlobals.cpp:414`, `soh/soh/Extractor/Extract.{h,cpp}`) using default seq_cst, which
libstdc++ lowers to correct `ldrex`/`strex`/`dmb` on ARMv7. No volatile-as-mutex, no hand-rolled
barriers, no spinlocks assuming x86 TSO. Threading otherwise runs on the portable header-only
`bshoshany/thread-pool`.

## 1.7 Recommended compiler flags

```
-march=armv7-a+neon-vfpv4   # or: -march=armv7-a -mfpu=neon-vfpv4
-mtune=cortex-a15
-mfloat-abi=hard
-mthumb                     # Debian default; keeps I-cache pressure down on a 2-core A15
-O2                         # NOT -O3: bigger code, worse I-cache behaviour, no measured win here
-ffast-math                 # matches what the Switch build already does (CMakeLists.txt:114)
```

Notes:
- `-mfpu=neon-vfpv4` is what unlocks `aMixImplNEON`. Verify with
  `echo | arm-linux-gnueabihf-gcc -dM -E -mfpu=neon-vfpv4 - | grep ARM_NEON`.
- `-mtune=cortex-a15` only (not `-mcpu`), so the binary stays runnable on other armv7 hardware.
- Cortex-A15 has hardware integer divide (`idiva`/`idivt` are in the device's feature list) —
  `-march=armv7-a` already implies `-mno-...`; nothing extra needed, but do not fall back to
  `armv5te`-style flags, which would emit `__aeabi_idiv` calls.
- Avoid LTO for now. It roughly triples peak link memory, and this device has **4 GB and no swap**.

## 1.8 WARN list (non-blocking, worth tracking)

1. `soh/CMake/Linux32bit-toolchain.cmake` — dead, unreferenced, hardcodes `-m32` and
   `/lib/i386-linux-gnu`. It is *i386*, not ARM. Someone will eventually try to reuse it for the
   Pyra and lose an afternoon. Either delete it or retitle it.
2. `soh/CMakeLists.txt` C options include `-Wno-incompatible-pointer-types -Wno-int-conversion
   -Wno-implicit-int` project-wide, so pointer/int width mistakes are silenced on every
   architecture. Not ARM-specific, but it means the compiler will not warn you if a new
   truncation is introduced during the port.
3. The four unaligned-read sites in §1.3.
4. `prism-processor` is FetchContent'd rather than vendored, so it was not greppable from a clean
   tree — spot-check it for arch assumptions once populated (it is a text templating library, so
   the risk is near zero).
