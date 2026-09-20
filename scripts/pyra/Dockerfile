# Cross-compilation environment for the DragonBox Pyra (armhf / Cortex-A15).
#
# Debian 13 (trixie) matches the Pyra's own userland, so the armhf multiarch
# packages installed here are the same versions the device runs. That avoids
# needing a sysroot rsynced off the device for anything except the proprietary
# PowerVR SGX driver, which is not in Debian.
FROM debian:trixie

ENV DEBIAN_FRONTEND=noninteractive

RUN dpkg --add-architecture armhf && apt-get update && apt-get install -y --no-install-recommends \
      crossbuild-essential-armhf \
      cmake ninja-build pkg-config git curl ca-certificates python3 python3-pip \
      file xxd \
 && rm -rf /var/lib/apt/lists/*

# Target-architecture dependencies. These mirror .github/workflows/apt-deps.txt,
# minus libglew-dev (Windows/macOS only - the Linux CMake path never links GLEW).
RUN apt-get update && apt-get install -y --no-install-recommends \
      libsdl2-dev:armhf \
      libpng-dev:armhf \
      libzip-dev:armhf \
      zlib1g-dev:armhf \
      libbz2-dev:armhf \
      libtinyxml2-dev:armhf \
      libspdlog-dev:armhf \
      libogg-dev:armhf \
      libopus-dev:armhf \
      libopusfile-dev:armhf \
      libvorbis-dev:armhf \
      libx11-dev:armhf \
      libgl-dev:armhf \
      libopengl-dev:armhf \
      libglx-dev:armhf \
      libgles-dev:armhf \
      libegl-dev:armhf \
      libusb-1.0-0-dev:armhf \
      nlohmann-json3-dev \
 && rm -rf /var/lib/apt/lists/*

# CMake/Utils.cmake:235-248 shells out to lsb_release on the *build host* for
# CPack package naming, and hard-errors if it is missing.
RUN apt-get update && apt-get install -y --no-install-recommends lsb-release \
 && rm -rf /var/lib/apt/lists/*

# pkg-config must look in the armhf multiarch path, not the host's.
ENV PKG_CONFIG_LIBDIR=/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig
ENV PKG_CONFIG_PATH=

WORKDIR /src
