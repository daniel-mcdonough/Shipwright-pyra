#!/usr/bin/env bash
# Cross-build Ship of Harkinian for the DragonBox Pyra.
#
# Assets are NOT generated here. ZAPD would be an armhf binary and cannot run on
# the build host, so soh.o2r / oot.o2r must be produced in a native build tree
# first and copied into build-pyra/soh/ - the same split the Switch and Wii U
# builds use (docs/BUILDING.md) and that CI uses for Linux.
#
#   ./scripts/pyra/build.sh image      build the cross container
#   ./scripts/pyra/build.sh configure  configure build-pyra/
#   ./scripts/pyra/build.sh build      compile
#   ./scripts/pyra/build.sh shell      interactive shell in the container
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE=soh-pyra-cross
BUILD_DIR=build-pyra
# Proprietary PowerVR SGX driver, rsynced off the device. Not redistributable,
# so it lives outside the repo and is mounted read-only.
DDK="${PYRA_DDK:-$HOME/.cache/pyra-ddk}"

run() {
  local net=()
  [[ "${1:-}" == "--net" ]] && { shift; net=(); }
  docker run --rm -i \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$REPO:/src" \
    -v "$DDK:/opt/omap5-sgx-ddk-um-linux:ro" \
    -w /src "${net[@]}" "$IMAGE" "$@"
}

case "${1:-build}" in
  image)
    docker build -t "$IMAGE" -f "$REPO/scripts/pyra/Dockerfile" "$REPO/scripts/pyra"
    ;;
  configure)
    [[ -d "$DDK/lib" ]] || { echo "PowerVR DDK not found at $DDK" >&2
                             echo "  rsync -a pyra@<host>:/opt/omap5-sgx-ddk-um-linux/ $DDK/" >&2; exit 1; }
    run cmake -S /src -B "/src/$BUILD_DIR" -GNinja \
        -DCMAKE_TOOLCHAIN_FILE=/src/CMake/toolchains/pyra-armhf.cmake \
        -DPYRA=ON \
        -DPYRA_DDK_ROOT=/opt/omap5-sgx-ddk-um-linux \
        -DCMAKE_BUILD_TYPE=Release \
        "${@:2}"
    ;;
  build)
    run cmake --build "/src/$BUILD_DIR" "${@:2}"
    ;;
  shell)
    docker run --rm -it --user "$(id -u):$(id -g)" -e HOME=/tmp \
      -v "$REPO:/src" -v "$DDK:/opt/omap5-sgx-ddk-um-linux:ro" -w /src "$IMAGE" bash
    ;;
  *) echo "usage: $0 {image|configure|build|shell}" >&2; exit 1 ;;
esac
