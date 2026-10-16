#!/bin/sh
for i in $(seq 300); do [ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] && break; sleep 1; done
[ "$(cat /sys/class/power_supply/bq25890-charger-0/online)" = 0 ] || { echo "still plugged - abort $(date +%T)"; exit 1; }
sleep 10
runuser -u andrew -- env XDG_RUNTIME_DIR=/run/user/1001 WAYLAND_DISPLAY=wayland-0 kscreen-doctor --dpms off
sleep 3
exec sh /home/andrew/connsys/e14.sh
