#!/usr/bin/env bash
# The two checks on the combiner wrap elision that do not need the Pyra.
#
#   docs/pyra/tools/offline-shader-checks.sh
#
# shadergen  expands default.shader.fs through the real prism processor for four combiner shapes
#            and prints the GLSL, so a template change can be read before it reaches a GPU.
# rangetest  runs CombinerCycleInRangeFor against a table of hand-worked cases. The functions are
#            extracted verbatim from gfx_opengl.cpp at build time, so this tests the shipped text;
#            if they are renamed or moved, this stops compiling rather than quietly testing a copy.
#
# Needs a configured host build for libprism.a: cmake -S . -B build-cmake -GNinja && ninja -C build-cmake prism
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
BUILD="${BUILD_DIR:-$REPO/build-cmake}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

PRISM_SRC="$BUILD/_deps/prism-src/src"
PRISM_LIB="$BUILD/_deps/prism-build"
[[ -f "$PRISM_LIB/libprism.a" ]] || { echo "no $PRISM_LIB/libprism.a - configure and build the host tree first" >&2; exit 1; }

python3 - "$REPO" "$OUT" <<'PY'
import sys
repo, out = sys.argv[1], sys.argv[2]
src = open(f"{repo}/libultraship/src/fast/backends/gfx_opengl.cpp").read()
start = src.index("// --- Combiner range analysis")
end = src.index("std::optional<std::string> opengl_include_fs")
block = src[start:end]
assert block.count("static bool") == 4, f"expected 4 functions, found {block.count('static bool')}"
open(f"{out}/range_extracted.inc", "w").write(block)
PY

g++ -std=c++20 -Wall -O0 "$REPO/docs/pyra/tools/rangetest.cpp" -I"$OUT" -o "$OUT/rangetest"
g++ -std=c++20 -O0 "$REPO/docs/pyra/tools/shadergen.cpp" -I"$PRISM_SRC" -L"$PRISM_LIB" -lprism -o "$OUT/shadergen"

echo "== rangetest"
"$OUT/rangetest"
echo
echo "== shadergen (${1:-summary}; pass 'full' for the GLSL)"
if [[ "${1:-}" == "full" ]]; then
  "$OUT/shadergen" "$REPO/libultraship/src/fast/shaders/opengl/default.shader.fs"
else
  "$OUT/shadergen" "$REPO/libultraship/src/fast/shaders/opengl/default.shader.fs" |
    grep -E '^//=+|WRAP\(texel|uniform (vec2 texture_size|int texture_width)'
fi
