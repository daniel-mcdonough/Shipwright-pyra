#!/bin/sh
# Device side. Attach to the middle segments of a 10-segment A/B-against-itself run.
# Segment ~ 180 grace + 600 frames at ~28 ms = ~22 s. Segments 0-2 clean, perf stat over
# ~3-4, perf record over ~6-7, 8-9 clean.
OUT=/home/pyra/soh-bench2/prof
rm -rf "$OUT"; mkdir -p "$OUT"
until grep -q 'Benchmark measuring' /home/pyra/soh-bench2/run.log 2>/dev/null; do sleep 1; done
date +%T.%N > "$OUT/t_measuring"
PID=$(pgrep -x soh.elf)
# render thread = the busiest thread over 3 s
for t in /proc/$PID/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat) $(cat $t/comm)"; done > "$OUT/tasks0"
sleep 3
for t in /proc/$PID/task/*; do echo "$(basename $t) $(awk '{print $14+$15}' $t/stat) $(cat $t/comm)"; done > "$OUT/tasks1"
TID=$(join "$OUT/tasks0" "$OUT/tasks1" | awk '{print $4-$2, $1}' | sort -rn | head -1 | cut -d' ' -f2)
echo "$TID" > "$OUT/tid"
sleep 63   # ~ start of segment 3
date +%T.%N > "$OUT/t_stat_start"
sudo perf stat --per-thread -e task-clock,context-switches,cycles,instructions -p $PID -o "$OUT/stat.txt" -- sleep 20
date +%T.%N > "$OUT/t_stat_end"
sleep 25   # ~ start of segment 6
date +%T.%N > "$OUT/t_rec_start"
sudo perf record -F 499 -t $TID -o "$OUT/rec.data" -- sleep 25 2> "$OUT/rec.err"
date +%T.%N > "$OUT/t_rec_end"
# report only after the game exits
while pgrep -x soh.elf > /dev/null; do sleep 2; done
sudo perf report -i "$OUT/rec.data" --no-children --sort dso --stdio > "$OUT/dso.txt" 2>&1
sudo perf report -i "$OUT/rec.data" --no-children --sort dso,sym --stdio > "$OUT/sym.txt" 2>&1
sudo chown -R pyra "$OUT"
echo done > "$OUT/done"
