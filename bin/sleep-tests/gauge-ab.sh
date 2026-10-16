#!/bin/sh
P=/sys/class/power_supply/mt6351-battery
m() { c0=$(cat $P/charge_counter); t0=$(date +%s.%N); "$@" >/dev/null 2>&1; c1=$(cat $P/charge_counter); t1=$(date +%s.%N); echo "$1 $c0 $c1 $t0 $t1" | awk '{printf "%-7s %7.1f mA over %.1f s (%d uAh)\n", $1, ($2-$3)*3.6/($5-$4), $5-$4, $2-$3}'; }
sleep 20; cat $P/status; cat /sys/devices/system/cpu/online
m sleep 55
m insmod /home/andrew/mt6797-spm-load.ko stage=6 io_en=0x81 acinactm=3 sleep_ms=2000 cycles=25
rmmod mt6797-spm-load
m sleep 55
dmesg | grep spm-load | grep -E "returned|cycle 24"
