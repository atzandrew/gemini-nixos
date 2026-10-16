#!/bin/sh
# Vcore control readback: 0600 CTRL, 0602 SEL, 0608 VOSEL, 060a ON, 060c SLEEP, 060e DA_NI_VOSEL, 0610 SYNC, 0612 CON9
busybox cat /sys/kernel/debug/regmap/1000d000.pwrap/registers | grep -E "^(0600|0602|0608|060a|060c|060e|0610|0612):" | tr "\n" " "; echo
