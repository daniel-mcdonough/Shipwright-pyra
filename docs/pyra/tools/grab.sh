#!/bin/sh
# Usage on the device, armed before the run: sh grab.sh <tag>; then on the host
#   PYRA_PIN_FREQ=1000000 bench.sh ab <cvar> A B $MARKET SOH_BENCH_FRAMES=2400 SOH_BENCH_REPS=2
# Frames land in /tmp/grab-<tag>/; convert with magick and compare with shotcmp.py.
# Device side: grab two frames in segments 0, 1 and 3 of a FRAMES=2400 REPS=2 A/B (TESTING.md §3).
# Segment k starts ~5 s + k * 70 s after "Benchmark measuring" at the Market pinned to 1000 MHz.
D=/tmp/grab-$1; rm -rf $D; mkdir -p $D
until grep -q 'Benchmark measuring' ~/soh-bench2/run.log 2>/dev/null; do sleep 1; done
t0=$(date +%s)
at() { while [ $(( $(date +%s) - t0 )) -lt $1 ]; do sleep 0.5; done; DISPLAY=:0 xwd -root -silent > $D/$2.xwd; }
at 25 A-1; at 28 A-2; at 95 B-1; at 98 B-2; at 235 C-1; at 238 C-2
echo done > $D/done
