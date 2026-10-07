#!/bin/sh
# gpu-cut-test.sh — measure what the idle GPU costs, without a kernel build.
#
# Stops the desktop (greetd), turns the backlight off, unloads panfrost,
# averages battery current for 60 s, then switches the VGPU rail off
# (RT5735 EN bits) and averages again. The difference = GPU rail cost.
# The GPU stays dead afterwards: REBOOT when it's done.
#
# Run on battery, detached:
#   sudo sh -c 'nohup sh /home/atzero/gpu-cut-test.sh > /home/atzero/gpu-cut.txt 2>&1 &'
# Result: cat ~/gpu-cut.txt   (takes ~3 min)

PS=/sys/class/power_supply/mt6351-battery
SECS=${SECS:-60}

avg() {   # average current_now over $1 s, in mA
    s=0; n=0
    while [ $n -lt "$1" ]; do
        v=$(cat $PS/current_now); s=$((s + v)); n=$((n + 1)); sleep 1
    done
    echo $((s / n / 1000))
}

rt5735_bus() {   # bus number of the i2c controller at 0x11010000
    for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
        [ -f "$d" ] || continue
        h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
        [ "$(echo "$h" | cut -c9-16)" = "11010000" ] && {
            echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
    done
    return 1
}

[ -r $PS/current_now ] || { echo "no gauge at $PS"; exit 1; }
[ "$(cat $PS/status)" = "Discharging" ] || echo "WARNING: status=$(cat $PS/status) (unplug USB for a valid number)"
BUS=$(rt5735_bus) || { echo "RT5735 bus not found"; exit 1; }
echo "RT5735 on i2c-$BUS: VSEL0(0x10)=$(i2cget -y "$BUS" 0x1c 0x10) VSEL1(0x11)=$(i2cget -y "$BUS" 0x1c 0x11)"

echo "stopping greetd, backlight off, unloading panfrost"
systemctl stop greetd
for b in /sys/class/backlight/*; do echo 4 > "$b/bl_power" 2>/dev/null; echo 0 > "$b/brightness" 2>/dev/null; done
sleep 15
modprobe -r panfrost 2>/dev/null && echo "panfrost unloaded" || echo "panfrost still loaded (ok)"
sleep 5

A=$(avg "$SECS")
echo "A desktop stopped, GPU rail ON : $A mA"

v0=$(i2cget -y "$BUS" 0x1c 0x10); v1=$(i2cget -y "$BUS" 0x1c 0x11)
i2cset -y "$BUS" 0x1c 0x10 $((v0 & 0x7f))
i2cset -y "$BUS" 0x1c 0x11 $((v1 & 0x7f))
echo "VGPU off: VSEL0=$(i2cget -y "$BUS" 0x1c 0x10) VSEL1=$(i2cget -y "$BUS" 0x1c 0x11)"
sleep 10

B=$(avg "$SECS")
echo "B GPU rail OFF                 : $B mA"
echo "GPU rail cost ≈ $((A - B)) mA (sign: more negative = more drain)"
echo "done — reboot now (sudo systemctl reboot)"
