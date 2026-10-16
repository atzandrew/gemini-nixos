#!/bin/sh
# E8: CONN power-off/on x3 with the rx-idle fix (boot-rxidle-20261015)
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32) $(busybox devmem 0x1000632c 32)"; busybox usleep 100000; done; }
up() { cut -d" " -f1 /proc/uptime; }
P=/sys/module/mtk_wcn/parameters
sleep 3
bluetoothctl power off; echo "bt power off rc=$?"; sleep 2
echo 1 > $P/conn_pwr_off; echo "conn_pwr_off=$(cat $P/conn_pwr_off)"
for c in 1 2 3; do
  echo "=== cycle $c $(date +%T)"
  nmcli radio wifi off; echo "radio off rc=$?"
  echo "7 0 0" > /proc/driver/wmt_dbg; echo "bt func off rc=$?"
  sleep 1
  echo "7 3 0" > /proc/driver/wmt_dbg; echo "wifi func off rc=$?"
  s 100 B$c
  t0=$(up); echo "7 3 1" > /proc/driver/wmt_dbg; rc=$?; t1=$(up)
  echo "wifi func on rc=$rc t0=$t0 t1=$t1"
  s 10 C$c
  nmcli radio wifi on; echo "radio on rc=$?"
  for i in $(busybox seq 30); do nmcli -t -f DEVICE,STATE dev | grep -q "^wlan0:connected$" && break; sleep 1; done
  echo "wlan0 after ${i}s: $(nmcli -t -f DEVICE,STATE dev | grep ^wlan0)"
  sleep 5
done
echo 0 > $P/conn_pwr_off; echo "conn_pwr_off=$(cat $P/conn_pwr_off)"
bluetoothctl power on; echo "bt power on rc=$?"
echo "done $(date +%T)"
