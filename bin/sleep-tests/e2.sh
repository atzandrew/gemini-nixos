#!/bin/sh
s() { for i in $(busybox seq $1); do echo "$2 $(busybox devmem 0x10006134 32)"; busybox usleep 100000; done; }
sleep 3
s 100 A
/usr/sbin/iw dev wlan0 set power_save on; echo "set on rc=$? $(/usr/sbin/iw dev wlan0 get power_save)"
s 400 B
/usr/sbin/iw dev wlan0 set power_save off; echo "set off rc=$? $(/usr/sbin/iw dev wlan0 get power_save)"
s 100 C
echo done
