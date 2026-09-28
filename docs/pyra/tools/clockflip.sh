#!/bin/sh
# Runs ON THE DEVICE during a pinned watch session (§11.14): moves the userspace pin between 625 and
# 750 MHz every 30 s while soh.elf runs, so two clocks can be compared by block in one process.
# Start it after `PYRA_WATCH_PIN=1 bench.sh watch start`: setsid nohup ~/clockflip.sh >/dev/null 2>&1 </dev/null &
# It stops on its own when the game exits or the governor leaves userspace; kill it before `watch stop`.
# Pair the blocks with `slowpairs.py blocks.txt clock 750 625`. Flip times go to ~/soh-bench2/clockflip.log.
f=625000
while pgrep -x soh.elf >/dev/null; do
  sleep 30
  pgrep -x soh.elf >/dev/null || break
  [ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)" = userspace ] || break
  for p in /sys/devices/system/cpu/cpu[01]/cpufreq; do sudo sh -c "echo $f > $p/scaling_setspeed"; done
  echo "$(date +%T) $f" >> ~/soh-bench2/clockflip.log
  [ $f = 625000 ] && f=750000 || f=625000
done
