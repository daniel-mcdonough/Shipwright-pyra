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
#   ./scripts/pyra/bench.sh watch start|read|stop  author a fixture with the controls
#
# Any SOH_BENCH* variable can be passed through on the command line, e.g.
#   ./scripts/pyra/bench.sh run SOH_BENCH_FRAMES=1200 SOH_BENCH_STATIC=0
#
# The device must be idle. Measuring while anything else is running measures that
# instead - see docs/pyra/07-phase5-performance.md for how much the machine moves.
#
# It must also be at a repeatable temperature. Frame rate falls ~30% between a cold
# part and equilibrium, and the warm-up frame count alone does not get there: report 8
# §8.14 watched a run with 16000 warm-up frames still climb 46 -> 77 C. Pass
# SOH_BENCH_SOAK_C to hold warm-up until the die reaches a temperature, so every run
# starts measuring at the same one:
#
#   ./scripts/pyra/bench.sh ab <cvar> 0 3 SOH_BENCH_SOAK_C=76 SOH_BENCH_WARMUP_MAX=20000
#
# The soak is free on a device that is already hot, so a back-to-back sequence of runs
# pays it once. Every result line now carries temp_c=start->end; two runs whose
# temperatures do not overlap are not comparable in absolute terms however tight their
# individual error bars are.
set -euo pipefail

HOST="${PYRA_HOST:?set PYRA_HOST to user@device}"
# The benchmark install (TESTING.md §0). The old default, ~/soh, holds a stale binary with no
# archives beside it: a run that forgot PYRA_DIR launched that and stopped at a missing-o2r dialog.
REMOTE_DIR="${PYRA_DIR:-/home/$(echo "$HOST" | cut -d@ -f1)/soh-bench2}"
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
#
# The pin uses the userspace governor, not min == max. pyra-thermal.service rewrites
# scaling_max_freq every 2 s - to 1.5 GHz whenever the die is below its 77 C rung - so a
# min == max pin silently became a 1.25-1.5 GHz range under ondemand the moment the daemon
# next ran (report 8 §8.20: two segments at 1500 MHz under a "1.25 GHz pin"). With
# userspace, scaling_setspeed fixes the request: a daemon ceiling above it changes nothing,
# and a thermal cap below it still lowers the clock, which the segment's cpu_mhz fields
# then show. The daemon keeps running; it is the safety backstop and owns the charger.
# The governor in force before the run is saved on the device and put back by unpin_cpu.
GOV_SAVE=/tmp/soh-bench-governor
pin_cpu() {
  [[ -n "$PIN_FREQ" ]] || return 0
  ssh -n "$HOST" "
    [ -e $GOV_SAVE ] || cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor > $GOV_SAVE
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
      [ -e \"\$p/scaling_max_freq\" ] || continue
      sudo sh -c \"cat \$p/cpuinfo_min_freq > \$p/scaling_min_freq\"
      sudo sh -c \"cat \$p/cpuinfo_max_freq > \$p/scaling_max_freq\"
      sudo sh -c \"echo userspace > \$p/scaling_governor\"
      sudo sh -c \"echo $PIN_FREQ > \$p/scaling_setspeed\"
    done
    echo \"pinned: governor \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor) setspeed \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_setspeed) kHz, cur \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq) kHz (was \$(cat $GOV_SAVE))\"" >&2
}

# Always runs, including on a failed or timed-out run: leaving a machine pinned would be a
# surprising thing to walk away from.
unpin_cpu() {
  [[ -n "$PIN_FREQ" ]] || return 0
  ssh -n "$HOST" "
    gov=\$(cat $GOV_SAVE 2>/dev/null || echo ondemand)
    [ \"\$gov\" = userspace ] && gov=ondemand
    for p in /sys/devices/system/cpu/cpu*/cpufreq; do
      [ -e \"\$p/scaling_max_freq\" ] || continue
      sudo sh -c \"echo \$gov > \$p/scaling_governor\"
      sudo sh -c \"cat \$p/cpuinfo_min_freq > \$p/scaling_min_freq\"
      sudo sh -c \"cat \$p/cpuinfo_max_freq > \$p/scaling_max_freq\"
    done
    rm -f $GOV_SAVE
    # Leaving ondemand discards its tunables; the kernel brings them back at stock, which
    # undid pyra-governor-tune's sampling_down_factor=10 after every pinned run until
    # 2026-09-29. Re-apply it (the hardware repo's unit) when it is installed.
    [ \"\$gov\" = ondemand ] && systemctl cat pyra-governor-tune.service >/dev/null 2>&1 &&
      sudo systemctl restart pyra-governor-tune.service
    echo \"restored \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)-\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq) kHz, governor \$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor), sampling_down_factor \$(cat /sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor 2>/dev/null || echo -)\"" >&2
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

# --- Fixture authoring -------------------------------------------------------
# SOH_BENCH_WATCH=1 runs the game normally - no freeze, no camera override, no
# measurement - and logs position/yaw/eye/at every 60 frames, so a fixture can be
# walked to with the controls in hand. It never ends on its own, which is why it is
# not a `run`: start it, play, read the viewpoint, stop it.
#
#   ./scripts/pyra/bench.sh watch start SOH_BENCH_ENTRANCE=0x00CD
#   ...walk to the spot, let the camera settle...
#   ./scripts/pyra/bench.sh watch read
#   ./scripts/pyra/bench.sh watch stop
#
# No CPU pin here: authoring is not a measurement, and the game is nicer to steer
# at the clock the governor picks. Scene numbers in the log are the scene_table.h
# ids - 0x55 Kokiri Forest, 0x51 Hyrule Field.
#
# Real-play A/B (report 8 8.22): add SOH_BENCH_AB_CVAR/SOH_BENCH_AB_VALUES and the game
# alternates the arms in ABBA blocks of 300 frames while someone plays, logging one
# "watch-block" line per block. Keep the frame cap (SOH_BENCH_UNCAP=0 - uncapped play runs
# the game too fast) and pin the clock (PYRA_WATCH_PIN=1): under ondemand the lighter arm
# gets a lower clock and more CPU time per frame, which biases against it.
#
#   PYRA_WATCH_PIN=1 ./scripts/pyra/bench.sh watch start SOH_BENCH_UNCAP=0 \
#       SOH_BENCH_AB_CVAR=gSettings.LeanResLookup SOH_BENCH_AB_VALUES=0,3
#   ./scripts/pyra/bench.sh watch blocks
#   ./scripts/pyra/bench.sh watch stop
watch_start() {
  watch_stop
  if [[ "${PYRA_WATCH_PIN:-0}" == 1 ]]; then pin_cpu; fi
  local env_args=("SOH_BENCH=1" "SOH_BENCH_WATCH=1" "$@")
  echo "==> ${env_args[*]}" >&2
  # The redirect and the & are grouped around the launch alone. Putting them after a
  # `cd ... && rm ... &&` list backgrounds the whole list, so the earlier commands keep
  # the ssh channel's stdout open and the ssh never returns.
  ssh -n "$HOST" "cd '$REMOTE_DIR' && rm -f watch.log && \
    { SDL_VIDEO_X11_FORCE_EGL=1 SDL_VIDEO_GL_DRIVER='$DDK_LIB' DISPLAY='$DISPLAY_ID' \
      ${env_args[*]} nohup ./soh.elf > watch.log 2>&1 < /dev/null & }"
  echo "started. Walk to the spot, then: $0 watch read" >&2
}

watch_read() {
  ssh -n "$HOST" "grep 'Benchmark viewpoint' '$REMOTE_DIR/watch.log' 2>/dev/null | tail -5" || true
}

watch_blocks() {
  ssh -n "$HOST" "grep -o 'Benchmark watch-block .*' '$REMOTE_DIR/watch.log' 2>/dev/null" || true
}

# The game catches SIGTERM and shuts down slowly, so a plain pkill returns long before
# the process is gone and the next launch meets a live fullscreen window.
watch_stop() {
  ssh -n "$HOST" "pkill -x '$COMM' 2>/dev/null; sleep 3; pkill -9 -x '$COMM' 2>/dev/null; sleep 1; exit 0" >/dev/null 2>&1 || true
  # Only if a watch pinned it: the saved-governor file is the marker.
  if ssh -n "$HOST" "[ -e $GOV_SAVE ]"; then unpin_cpu; fi
}

case "${1:-run}" in
  deploy) deploy ;;
  watch)
    shift
    sub="${1:-start}"; [[ $# -gt 0 ]] && shift
    case "$sub" in
      start) watch_start "$@" ;;
      read)  watch_read ;;
      blocks) watch_blocks ;;
      stop)  watch_stop ;;
      *) echo "usage: $0 watch {start [VAR=VAL ...]|read|blocks|stop}" >&2; exit 1 ;;
    esac
    ;;
  run)    shift; run "$@" ;;
  ab)
    shift
    [[ $# -ge 3 ]] || { echo "usage: $0 ab <cvar> <valueA> <valueB> [VAR=VAL ...]" >&2; exit 1; }
    cvar="$1"; a="$2"; b="$3"; shift 3
    run "SOH_BENCH_AB_CVAR=$cvar" "SOH_BENCH_AB_VALUES=$a,$b" "$@"
    ;;
  *) echo "usage: $0 {deploy|run|ab|watch}" >&2; exit 1 ;;
esac
