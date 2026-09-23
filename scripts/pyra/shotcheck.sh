#!/usr/bin/env bash
# Compare what two arms of a CVar actually put on the screen.
#
# The benchmark harness answers "is this frame cheaper". It cannot answer "is this
# frame still correct", and for a shader change that is the question that matters
# first: a wrong range analysis shows as a colour saturating where it should have
# wrapped, which a frame time cannot see.
#
#   ./scripts/pyra/shotcheck.sh gSettings.ShaderOpts 0 3
#
# Runs the fixture three times - arm A, arm B, then arm A again - grabbing a few
# frames off the device with xwd each time, and asks whether B differs from A by
# more than A differs from itself. That third run is the control and is the whole
# point: the scene is frozen but not reproducible across launches, so a bare A-vs-B
# pixel count is not evidence of anything.
#
# Not a benchmark: no CPU pin, no soak, no timing. It leaves the part warm, which is
# the right order to run these in.
set -euo pipefail

HOST="${PYRA_HOST:?set PYRA_HOST to user@device}"
REMOTE_DIR="${PYRA_DIR:-/home/$(echo "$HOST" | cut -d@ -f1)/soh}"
DDK_LIB="${PYRA_DDK_LIB:-/opt/omap5-sgx-ddk-um-linux/lib/libGLESv2.so.1}"
DISPLAY_ID="${PYRA_DISPLAY:-:0}"
SHOTS="${PYRA_SHOTS:-3}"
# Long enough for a cold scene load plus settle and warm-up. Reaching it means the
# run never started measuring, which is a failure worth naming rather than timing out.
READY_TIMEOUT="${PYRA_READY_TIMEOUT:-180}"
COMM="soh.elf"
# No ssh in the capture path may hang: a silent stall here is indistinguishable from a
# slow scene load, and the first run of this script spent thirteen minutes proving it.
SSH="timeout 60 ssh -o ConnectTimeout=10"

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${PYRA_SHOT_DIR:-$REPO/build-pyra/shots}"

[[ $# -ge 3 ]] || { echo "usage: $0 <cvar> <valueA> <valueB> [VAR=VAL ...]" >&2; exit 1; }
CVAR="$1"; A="$2"; B="$3"; shift 3
EXTRA=("$@")

mkdir -p "$OUT"
rm -f "$OUT"/shot-*.xwd "$OUT"/shot-*.png "$OUT"/diff-*.png

# SIGTERM is not enough: the game catches it and shuts down slowly enough that the next
# launch starts alongside it. Instances then accumulate, and because each one is fullscreen
# an `xwd -root` grab photographs whichever is on top rather than the one just launched -
# which silently compares an arm against itself, or against an arm from three runs ago.
# So: ask, wait, insist, and verify. Anything left is fatal rather than tolerated.
cleanup() {
  $SSH -n "$HOST" "pkill -x '$COMM' 2>/dev/null; sleep 3; pkill -9 -x '$COMM' 2>/dev/null; \
                   sleep 2; exit 0" >/dev/null 2>&1 || true
}

require_clean() {
  local left
  left="$($SSH -n "$HOST" "pgrep -c -x '$COMM' 2>/dev/null || true")"
  if [[ -n "${left//[[:space:]]/}" && "${left//[[:space:]]/}" != "0" ]]; then
    echo "$left instance(s) of $COMM still running after cleanup - refusing to grab frames, "\
         "because a leftover fullscreen window is what xwd would photograph." >&2
    exit 1
  fi
}

trap cleanup EXIT INT TERM

# Frames are named by slot (A, B) and not by the arm's value, so that `shotcheck.sh <cvar>
# 0 0` is a real null control rather than one launch silently overwriting the other's frames
# and then being compared with itself.
capture_arm() {
  local slot="$1" arm="$2"
  echo "==> slot $slot: $CVAR=$arm" >&2
  cleanup
  require_clean

  # Both A/B values are the same arm deliberately: this borrows the harness for its
  # fixture and its freeze, not for its alternation. A huge frame count keeps the run
  # inside segment 0 for as long as it takes to grab the frames.
  local env_args=(
    "SOH_BENCH=1" "SOH_BENCH_EXIT=0" "SOH_BENCH_STATIC=1"
    "SOH_BENCH_AB_CVAR=$CVAR" "SOH_BENCH_AB_VALUES=$arm,$arm"
    "SOH_BENCH_FRAMES=100000" "SOH_BENCH_REPS=1" "SOH_BENCH_SEG_WARMUP=60"
    "${EXTRA[@]}"
  )

  # The braces matter. Written as `cd ... && rm ... && cmd > log &` the `&` backgrounds the
  # whole list, so cd and rm still hold the ssh channel's stdout open, the channel never
  # closes and ssh hangs until something kills it. Grouping puts the redirect and the `&` on
  # the launch alone, so the remote shell has nothing left to hold open and returns.
  $SSH -n "$HOST" "cd '$REMOTE_DIR' && rm -f shot.log && \
    { SDL_VIDEO_X11_FORCE_EGL=1 SDL_VIDEO_GL_DRIVER='$DDK_LIB' DISPLAY='$DISPLAY_ID' \
      ${env_args[*]} nohup ./soh.elf > shot.log 2>&1 < /dev/null & }"

  # Waiting on the log line rather than on a fixed sleep: a cold scene load is minutes
  # slower than a warm one, and a fixed sleep would photograph the title card.
  local waited=0
  until $SSH -n "$HOST" "grep -q 'Benchmark measuring' '$REMOTE_DIR/shot.log' 2>/dev/null"; do
    sleep 3; waited=$((waited + 3))
    if (( waited >= READY_TIMEOUT )); then
      echo "arm $arm never reached the measuring phase in ${READY_TIMEOUT}s. Last of shot.log:" >&2
      $SSH -n "$HOST" "tail -20 '$REMOTE_DIR/shot.log'" >&2 || true
      exit 1
    fi
  done
  echo "    measuring after ${waited}s; grabbing $SHOTS frames" >&2

  for i in $(seq 1 "$SHOTS"); do
    echo "    grab $i/$SHOTS" >&2
    $SSH -n "$HOST" "DISPLAY='$DISPLAY_ID' xwd -root -silent > '$REMOTE_DIR/shot-$slot-$i.xwd'"
    sleep 1
  done

  cleanup
  echo "    fetching" >&2
  timeout 120 scp -q "$HOST:$REMOTE_DIR/shot-$slot-*.xwd" "$OUT/"
  $SSH -n "$HOST" "rm -f '$REMOTE_DIR'/shot-$slot-*.xwd"
  for f in "$OUT"/shot-"$slot"-*.xwd; do
    magick "$f" "${f%.xwd}.png"
  done
}

capture_arm A "$A"
capture_arm B "$B"
# A third slot running arm A again, so the null is measured in the same session rather
# than assumed. It is not optional: two launches of the SAME arm differ by ~10000 pixels
# at this fixture, because particles and fairies come to rest in different places each
# time, so "the arms differ by N pixels" means nothing on its own. One extra launch is two
# minutes; going without it cost most of an evening and produced a confident wrong answer.
capture_arm N "$A"

python3 "$REPO/scripts/pyra/shotdiff.py" "$OUT" "$A" "$B"
