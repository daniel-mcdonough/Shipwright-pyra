# CMake toolchain file for the DragonBox Pyra.
#
#   SoC   TI OMAP5432, 2 x Cortex-A15 @ 1.5 GHz, NEON / VFPv4
#   OS    Debian 13 (trixie) armhf
#   GPU   PowerVR SGX544MP2, OpenGL ES 2.0 only (see docs/pyra/)
#
# Follows the same pattern as the Switch and Wii U builds (docs/BUILDING.md):
# an external toolchain file plus a separate build tree, with game assets
# generated once in a native tree and copied in.
#
# Usage (inside the container from scripts/pyra/Dockerfile):
#   cmake -S . -B build-pyra -GNinja \
#         -DCMAKE_TOOLCHAIN_FILE=CMake/toolchains/pyra-armhf.cmake \
#         -DPYRA=ON -DCMAKE_BUILD_TYPE=Release

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(PYRA_TRIPLE arm-linux-gnueabihf CACHE STRING "Cross toolchain triple")

set(CMAKE_C_COMPILER   ${PYRA_TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${PYRA_TRIPLE}-g++)

# Debian's armhf default is -march=armv7-a+fp, which does NOT define __ARM_NEON.
# That matters: __ARM_NEON is what selects aMixImplNEON over the scalar audio
# mixer (soh/soh/mixer.c:689, dispatch at :358-365). Without it the NEON path
# silently compiles out.
# armv7ve rather than armv7-a: Cortex-A15 implements ARMv7VE, which mandates the
# integer divide instructions. Plain armv7-a leaves division to libgcc, and
# __aeabi_uidiv shows up at ~1.6% of a profile of the running game. GCC has no
# "+idiv" feature for armv7-a, so the architecture itself has to be raised.
set(PYRA_ARCH_FLAGS "-march=armv7ve+neon-vfpv4 -mtune=cortex-a15 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${PYRA_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${PYRA_ARCH_FLAGS}")

# lld rather than ld.bfd: relinking the 48 MB soh.elf drops from ~6.5 s to ~0.6 s,
# and that link is most of an incremental build. mold is as fast but leaves out
# the hard-float flag in the ELF header and the .ARM.attributes section; lld keeps
# both. CMAKE_LINKER_TYPE (CMake 3.29+) also reaches existing build trees, which
# CMAKE_*_LINKER_FLAGS_INIT does not. Set PYRA_LINKER=BFD to compare.
set(PYRA_LINKER LLD CACHE STRING "CMAKE_LINKER_TYPE for the Pyra build (LLD, MOLD or BFD)")
string(TOUPPER "${PYRA_LINKER}" CMAKE_LINKER_TYPE)

# Debian multiarch: armhf libraries live beside the host's in the same rootfs,
# so CMAKE_LIBRARY_ARCHITECTURE steers find_library rather than a sysroot split.
set(CMAKE_LIBRARY_ARCHITECTURE ${PYRA_TRIPLE})
set(CMAKE_FIND_ROOT_PATH /usr/${PYRA_TRIPLE} /usr/lib/${PYRA_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# pkg-config must not answer with host .pc files.
set(ENV{PKG_CONFIG_LIBDIR} "/usr/lib/${PYRA_TRIPLE}/pkgconfig:/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
