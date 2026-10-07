#!/bin/sh
# board-cut-test.sh — measure always-on board supplies, no build.
#
# A: desktop stopped, backlight off
# B: right USB-C port 5 V off (GPIO94 usb1-drvvbus + GPIO72 SW7226 load switch low)
# C: panel off as well (GPIO180 reset low, GPIO60/GPIO251 TPS65132 ±5.4 V bias low)
# The screen (and the right USB port) stay dead afterwards: REBOOT when done.
# Nothing may be plugged into the right USB-C port for this test.
#
# Run on battery, detached:
#   sudo sh -c 'nohup sh /home/atzero/board-cut-test.sh > /home/atzero/board-cut.txt 2>&1 &'
# Result: cat ~/board-cut.txt   (takes ~4 min)

PS=/sys/class/power_supply/mt6351-battery
SECS=${SECS:-60}
DOUT=0x10005100          # GPIO DOUT bank n at DOUT + 0x10*n; +4 = SET, +8 = CLR

rd() { busybox devmem "$1" 32; }
wr() { busybox devmem "$1" 32 "$2"; }
avg() { s=0; n=0; while [ $n -lt "$1" ]; do v=$(cat $PS/current_now); s=$((s + v)); n=$((n + 1)); sleep 1; done; echo $((s / n / 1000)); }
pin() {  # pin <gpio>  -> prints 0/1 output level
    b=$(( $1 / 32 )); i=$(( $1 % 32 ))
    echo $(( ($(rd $((DOUT + 0x10 * b))) >> i) & 1 ))
}
low() {  # low <gpio>
    b=$(( $1 / 32 )); i=$(( $1 % 32 ))
    wr $((DOUT + 0x10 * b + 8)) $((1 << i))
}
show() { echo "  GPIO94=$(pin 94) GPIO72=$(pin 72) GPIO180=$(pin 180) GPIO60=$(pin 60) GPIO251=$(pin 251)"; }

[ -r $PS/current_now ] || { echo "no gauge at $PS"; exit 1; }
[ "$(cat $PS/status)" = "Discharging" ] || echo "WARNING: status=$(cat $PS/status) (unplug USB for a valid number)"
echo "before:"; show

echo "stopping greetd, backlight off"
systemctl stop greetd
for b in /sys/class/backlight/*; do echo 4 > "$b/bl_power" 2>/dev/null; echo 0 > "$b/brightness" 2>/dev/null; done
sleep 20

A=$(avg "$SECS"); echo "A desktop stopped, backlight off : $A mA"

low 72; low 94
echo "right USB port 5 V off:"; show
sleep 10
B=$(avg "$SECS"); echo "B + right-port VBUS off         : $B mA"

low 180; sleep 0.1; low 60; low 251
echo "panel off:"; show
sleep 10
C=$(avg "$SECS"); echo "C + panel reset/bias off        : $C mA"

echo "right-port 5 V ≈ $((A - B)) mA, panel ≈ $((B - C)) mA (more negative = more drain)"
echo "done — reboot now (sudo systemctl reboot)"
