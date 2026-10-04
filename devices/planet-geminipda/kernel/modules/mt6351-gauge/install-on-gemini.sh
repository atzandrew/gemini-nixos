#!/bin/sh
# install-on-gemini.sh — install the MT6351 gauge module + gauge-aware
# battery-guard on the Gemini (Debian), so both survive reboots.
# Run ON THE GEMINI as root from the directory holding the copied files:
#   sudo sh install-on-gemini.sh [mt6351-gauge.ko] [battery-guard.sh]
# Interim until the gemini-debian image/bundle carries these
# (claude/power.md). Undo: see the end of this file.
set -eu
KO=${1:-./mt6351-gauge.ko}
GUARD=${2:-./battery-guard.sh}
KVER=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$KO" ] || { echo "missing $KO"; exit 1; }
[ -f "$GUARD" ] || { echo "missing $GUARD"; exit 1; }

install -D -m 644 "$KO" "/lib/modules/$KVER/extra/mt6351-gauge.ko"
/sbin/depmod -a "$KVER"
echo mt6351_gauge > /etc/modules-load.d/gemini-gauge.conf
echo 'options mt6351_gauge system=1' > /etc/modprobe.d/gemini-gauge.conf

install -m 755 "$GUARD" /usr/local/sbin/battery-guard.sh

# (re)load the module from its installed location with the boot options
/sbin/rmmod mt6351_probe 2>/dev/null || true
/sbin/rmmod mt6351_gauge 2>/dev/null || true
/sbin/modprobe mt6351_gauge
systemctl restart gemini-battery-guard.service
sleep 2
dmesg | grep mt6351_gauge | tail -2
journalctl -t battery-guard -n 3 --no-pager
echo "OK: mt6351_gauge loads at boot (system=1); battery-guard uses the gauge."

# Undo:
#   rm /etc/modules-load.d/gemini-gauge.conf /etc/modprobe.d/gemini-gauge.conf
#   rm /lib/modules/$(uname -r)/extra/mt6351-gauge.ko && depmod -a
#   (battery-guard.sh falls back to the BQ25896 logic when the gauge is absent)
