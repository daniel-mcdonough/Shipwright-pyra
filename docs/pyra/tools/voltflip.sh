#!/bin/sh
# Runs ON THE DEVICE during a pinned watch session (§8.51): moves the 750 MHz point between 870 and 950 mV every
# 30 s while soh.elf runs, so the two voltages can be compared by block in one process (after clockflip.sh).
# Start it after `PYRA_WATCH_PIN=1 PYRA_PIN_FREQ=750000 bench.sh watch start`:
#   setsid nohup ~/voltflip.sh >/dev/null 2>&1 </dev/null &
# It stops on its own when the game exits or the governor leaves userspace, and always leaves 870 mV set (the
# shipped value, /etc/default/pyra-undervolt). Kill it (TERM) before `watch stop`.
# `pyra-undervolt set` steps the CPU to 875 MHz for ~0.2 s (the module cannot change the point in use), and the
# watch block samples the clock too sparsely to see it, so each switch is logged with its start and end time:
# `slowpairs.py watch.log volt 870000 950000 voltflip.log` drops every block that overlaps one.
log=~/soh-bench2/voltflip.log
set_uv() {
  t0=$(date +%T.%3N)
  sudo pyra-undervolt set 750000 "$1" >/dev/null 2>&1
  rail=$(sudo pyra-undervolt status | grep -o 'rail) [0-9]*' | grep -o '[0-9]*$')
  echo "$t0 $(date +%T.%3N) $1 rail=$rail" >> $log
}
trap 'set_uv 870000; exit 0' TERM INT
echo "# start $(date -Is)" > $log
set_uv 870000
uv=950000
while pgrep -x soh.elf >/dev/null; do
  sleep 30 & wait $!
  pgrep -x soh.elf >/dev/null || break
  [ "$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)" = userspace ] || break
  set_uv $uv
  [ $uv = 870000 ] && uv=950000 || uv=870000
done
set_uv 870000
