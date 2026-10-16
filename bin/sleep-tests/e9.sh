#!/bin/sh
# E9: register snapshot boot-state vs after one CONN off/on (no SPM)
D=/home/andrew/connsys; P=/sys/module/mtk_wcn/parameters
sh $D/snap.sh $D/snapA1.txt; sleep 5; sh $D/snap.sh $D/snapA2.txt
bluetoothctl power off; sleep 2
echo 1 > $P/conn_pwr_off
nmcli radio wifi off
echo "7 0 0" > /proc/driver/wmt_dbg; sleep 1
echo "7 3 0" > /proc/driver/wmt_dbg; sleep 5
echo "pwr_con off: $(busybox devmem 0x1000632c 32)"
echo "7 3 1" > /proc/driver/wmt_dbg; echo "on rc=$?"
echo 0 > $P/conn_pwr_off
nmcli radio wifi on
bluetoothctl power on; sleep 15
sh $D/snap.sh $D/snapB1.txt; sleep 5; sh $D/snap.sh $D/snapB2.txt
echo "done $(date +%T)"
