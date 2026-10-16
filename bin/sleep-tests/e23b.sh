#!/bin/sh
# E23 step 2: GPU domains off/on once (blanked, Mali suspended), then plain baseline SPM run (GPU on).
O=/home/andrew/connsys/e23-cyc.log; : > $O
runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms off >/dev/null 2>&1; sleep 10
for i in 1 2 3; do
	g=$(sh /home/andrew/connsys/gpu.sh off) || { echo "$g" >> $O; sh /home/andrew/connsys/gpu.sh on >> $O; sleep 5; continue; }
	echo "$(date +%T) $g" >> $O; sleep 5; echo "$(date +%T) $(sh /home/andrew/connsys/gpu.sh on)" >> $O; break
done
sync; sleep 10
sh /home/andrew/connsys/e22-run.sh 25 0x5fc102ff /home/andrew/connsys/e23-cyc.log.run
