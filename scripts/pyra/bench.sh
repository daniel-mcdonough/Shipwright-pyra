#!/usr/bin/env bash
# Run the deterministic benchmark harness on the device and fetch the results.
#
# Requires a build configured with -DSOH_BENCHMARK=ON:
#   ./scripts/pyra/build.sh configure -DSOH_BENCHMARK=ON
#   ./scripts/pyra/build.sh build -j"$(nproc)"
#
#   ./scripts/pyra/bench.sh deploy                 copy the binary to the device
#   ./scripts/pyra/bench.sh run [VAR=VAL ...]      run once, print the results
#   ./scripts/pyra/bench.sh ab <cvar> <a> <b>      interleaved A/B on one CVar
#
# Any SOH_BENCH* variable can be passed through on the command line, e.g.
#   ./scripts/pyra/bench.sh run SOH_BENCH_FRAMES=1200 SOH_BENCH_STATIC=0
#
# The device must be idle. Measuring while anything else is running measures that
# instead - see docs/pyra/07-phase5-performance.md for how much the machine moves.
set -euo pipefail

HOST="${PYRA_HOST:?set PYRA_HOST to user@device}"
REMOTE_DIR="${PYRA_DIR:-/home/$(echo "$HOST" | cut -d@ -f1)/soh}"
# Without these SDL loads libGLESv2.so.2, which resolves through glvnd to Mesa and
# renders in software on llvmpipe. It still runs, it still reports a frame rate, and
# the frame rate is meaningless - see docs/pyra/00-EXECUTIVE-SUMMARY.md finding 3.
# The DDK SONAME really is .so.1 against the system .so.2; that mismatch is the bug.
DDK_LIB="${PYRA_DDK_LIB:-/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1}"
DISPLAY_ID="${PYRA_DISPLAY:-:0}"
# A hung harness must not hang the shell. The harness exits on its own when
# SOH_BENCH_EXIT=1 (the default), so reaching this is already a failure.
RUN_TIMEOUT="${PYRA_TIMEOUT:-900}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="$REPO/build-pyra/soh/soh.elf"
# Truncated to the 15 characters `pkill -x` matches against comm, because matching
# the full path once matched this script's own ssh command line and killed the session.
COMM="soh.elf"

deploy() {
  [[ -f "$BIN" ]] || { echo "not built: $BIN" >&2; exit 1; }
  ssh "$HOST" "mkdir -p '$REMOTE_DIR'"
  scp "$BIN" "$HOST:$REMOTE_DIR/soh.elf"
  # The game will not boot without its assets, and the failure looks like a hang
  # rather than an error, so say so here instead.
  ssh "$HOST" "cd '$REMOTE_DIR' && for f in oot.o2r soh.o2r; do
      [ -e \"\$f\" ] || echo \"missing: \$REMOTE_DIR/\$f (symlink it from a working install)\" >&2
    done"
}

run() {
  local env_args=("SOH_BENCH=1" "SOH_BENCH_OUT=$REMOTE_DIR/bench.txt" "$@")
  echo "==> ${env_args[*]}" >&2
  # `env -i`-style explicitness is deliberate: the run must be reproducible from
  # this line alone, not from whatever happens to be in the device's profile.
  ssh "$HOST" "cd '$REMOTE_DIR' && rm -f bench.txt && \
    SDL_VIDEO_X11_FORCE_EGL=1 SDL_VIDEO_GL_DRIVER='$DDK_LIB' DISPLAY='$DISPLAY_ID' \
    ${env_args[*]} timeout -s KILL $RUN_TIMEOUT ./soh.elf > run.log 2>&1" >/dev/null 2>&1 || true
  ssh "$HOST" "pkill -x '$COMM' 2>/dev/null || true"
  ssh "$HOST" "cat '$REMOTE_DIR/bench.txt' 2>/dev/null" || {
    echo "no results. Last of $REMOTE_DIR/run.log:" >&2
    ssh "$HOST" "tail -30 '$REMOTE_DIR/run.log' 2>/dev/null" >&2 || true
    exit 1; }
}

case "${1:-run}" in
  deploy) deploy ;;
  run)    shift; run "$@" ;;
  ab)
    shift
    [[ $# -ge 3 ]] || { echo "usage: $0 ab <cvar> <valueA> <valueB> [VAR=VAL ...]" >&2; exit 1; }
    cvar="$1"; a="$2"; b="$3"; shift 3
    run "SOH_BENCH_AB_CVAR=$cvar" "SOH_BENCH_AB_VALUES=$a,$b" "$@"
    ;;
  *) echo "usage: $0 {deploy|run|ab}" >&2; exit 1 ;;
esac
