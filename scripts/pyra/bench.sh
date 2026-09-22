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
# Pin the CPU to one frequency for the duration of a run. The ondemand governor walked
# the clock 1.4 -> 1.0 GHz across the run in docs/pyra/08-optimisation-opportunities.md
# §8.14, tripling the pair standard deviation: ABBA cancels linear drift, not a step
# change part-way through. 1.25 GHz is deliberately not the maximum - it is where every
# quiet run happened to sit, and capping the 1.4-1.5 GHz bursts makes a pinned run cooler
# than an unpinned one rather than hotter. Set PYRA_PIN_FREQ= (empty) to leave the
# governor alone.
PIN_FREQ="${PYRA_PIN_FREQ-1250000}"
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

# Thermal protection is not affected: it is enforced by the cpu_thermal zone through the
# thermal-cpufreq cooling device, which clamps frequency whatever the governor is set to.
# The passive trip is 100 C and this device runs benchmarks around 80 C.
pin_cpu() {
  [[ -n "$PIN_FREQ" ]] || return 0
  # The governor is left alone deliberately. Setting scaling_min_freq == scaling_max_freq
  # forces any governor to a single frequency, so there is no governor state to save and
  # restore - and sysfs will not hold a saved copy anyway.
  ssh -n "$HOST" "
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
      [ -e \"\$p/scaling_max_freq\" ] || continue
      sudo sh -c \"cat \$p/cpuinfo_min_freq > \$p/scaling_min_freq\"
      sudo sh -c \"echo $PIN_FREQ > \$p/scaling_max_freq\"
      sudo sh -c \"echo $PIN_FREQ > \$p/scaling_min_freq\"
    done
    echo \"pinned to \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)-\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) kHz (governor \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) left as-is)\"" >&2
}

# Always runs, including on a failed or timed-out run: leaving a machine pinned would be a
# surprising thing to walk away from.
unpin_cpu() {
  [[ -n "$PIN_FREQ" ]] || return 0
  ssh -n "$HOST" "
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
      [ -e \"\$p/scaling_max_freq\" ] || continue
      sudo sh -c \"cat \$p/cpuinfo_min_freq > \$p/scaling_min_freq\"
      sudo sh -c \"cat \$p/cpuinfo_max_freq > \$p/scaling_max_freq\"
    done
    echo \"restored \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)-\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) kHz\"" >&2
}

run() {
  local env_args=("SOH_BENCH=1" "SOH_BENCH_OUT=$REMOTE_DIR/bench.txt" "$@")
  trap unpin_cpu EXIT INT TERM
  pin_cpu
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
