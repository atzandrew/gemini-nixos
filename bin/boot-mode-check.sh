#!/bin/bash
# boot-mode-check.sh — what did LK boot us in, and is the battery charging?
#
# Run on the Gemini once after a NORMAL boot and once after a CHARGER boot
# (switched off, then plugged into the wall: LK's off-mode-charging path),
# then diff the two reports. Read-only.
#
#   scp bin/boot-mode-check.sh atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 sudo bash /tmp/boot-mode-check.sh
#   scp atzero@192.168.0.139:'~/boot-mode-*.txt' logs/
#
# What it records:
#   - LK's /chosen node. CONFIG_CMDLINE_FORCE makes the KERNEL ignore
#     /chosen/bootargs, but the property is still in the DT LK patched, so
#     userspace (and the initrd) can read LK's real cmdline here, incl.
#     androidboot.mode=charger in a POC boot (LK atags.c). Other /chosen
#     props (atag,boot etc., if LK sets them) are hex-dumped.
#   - The gauge's "s since PMIC power-on" line and the MT6351 RTC regs.
#   - Every power_supply's uevent, then 10 samples of the battery current
#     (gauge current_now: + = into the cell) to see whether it charges.
set -u
OUT_DIR=${SUDO_USER:+/home/$SUDO_USER}
OUT_DIR=${OUT_DIR:-$HOME}
OUT="$OUT_DIR/boot-mode-$(date +%Y%m%d-%H%M%S).txt"
exec > >(tee "$OUT") 2>&1

echo "=== boot-mode-check $(date -Is)  uptime $(cut -d' ' -f1 /proc/uptime) s"
echo "kernel: $(uname -a)"
echo "boot_id: $(cat /proc/sys/kernel/random/boot_id)"
echo

CH=/proc/device-tree/chosen
echo "=== /chosen properties"
if [ -d "$CH" ]; then
    for p in "$CH"/*; do
        n=$(basename "$p")
        [ -f "$p" ] || { echo "  $n/ (node)"; continue; }
        sz=$(stat -c %s "$p")
        case "$n" in
        bootargs|stdout-path|linux,stdout-path|name)
            printf '  %s (%s B): %s\n' "$n" "$sz" "$(tr '\0' ' ' < "$p")" ;;
        *)
            printf '  %s (%s B): ' "$n" "$sz"
            od -A n -t x4 -v -N 256 "$p" | tr -s ' \n' ' '
            echo ;;
        esac
    done
else
    echo "  (no /proc/device-tree/chosen)"
fi
echo
echo "=== LK cmdline, one arg per line (androidboot.* and mode-ish args)"
tr '\0 ' '\n\n' < "$CH/bootargs" 2>/dev/null | grep -i -E 'androidboot|mode|boot_reason|bootreason|charger|lcm=' || echo "  (none)"
echo
echo "=== kernel's own cmdline (forced)"
cat /proc/cmdline
echo

echo "=== gauge / power driver boot lines"
dmesg | grep -i -E 'mt6351_gauge|mt6351-gauge|PMIC power-on|mt6797.power|bq25890|rtc-mt6351|watchdog' | head -40
echo
RR=/sys/bus/platform/devices/10007000.power/rtc_regs
echo "=== MT6351 RTC regs ($RR)"
cat "$RR" 2>/dev/null || echo "  (not readable)"
echo

echo "=== power supplies"
for s in /sys/class/power_supply/*; do
    echo "--- $(basename "$s")"
    sed 's/^POWER_SUPPLY_/  /' "$s/uevent" 2>/dev/null
done
echo

BAT=/sys/class/power_supply/mt6351-battery
CHG=$(ls -d /sys/class/power_supply/bq25890-charger* 2>/dev/null | head -1)
echo "=== 10 samples, 2 s apart (gauge current: + = charging the cell)"
printf '%-6s %-10s %-9s %-7s %-6s %-12s %-10s\n' t_s I_cell_mA V_mV cap_% status chg_online iin_lim_mA
for i in $(seq 1 10); do
    I=$(cat "$BAT/current_now" 2>/dev/null); V=$(cat "$BAT/voltage_now" 2>/dev/null)
    C=$(cat "$BAT/capacity" 2>/dev/null); S=$(cat "$BAT/status" 2>/dev/null)
    O=$(cat "$CHG/online" 2>/dev/null); L=$(cat "$CHG/input_current_limit" 2>/dev/null)
    printf '%-6s %-10s %-9s %-7s %-6s %-12s %-10s\n' "$(cut -d. -f1 /proc/uptime)" \
        "$(( ${I:-0} / 1000 ))" "$(( ${V:-0} / 1000 ))" "${C:-?}" "${S:-?}" "${O:-?}" "$(( ${L:-0} / 1000 ))"
    sleep 2
done
echo
echo "=== this boot's first journal lines (wall clock may be fake until the RTC/NTP set it)"
journalctl -b -o short-monotonic --no-pager 2>/dev/null | head -3
echo
echo "report: $OUT"
