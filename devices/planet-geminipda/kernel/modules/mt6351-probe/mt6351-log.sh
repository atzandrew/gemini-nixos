#!/bin/sh
# mt6351-log.sh [interval_s] [outfile] — sample the MT6351 fuel gauge
# (latched CURRENT_OUT / CAR / NTER) next to the BQ25896 charger supply
# into a CSV. Run on the Gemini as root with mt6351-probe.ko loaded.
#   sudo sh mt6351-log.sh 5 ~/mt6351.csv        (Ctrl-C to stop)
#
# Uses latch mode (writes only FGADC_CON0[15:8], the vendor read
# handshake). R_CURR is NOT used: it stays frozen (2026-10-03 on glass).
# Columns: fg_mA + = charging (vendor sign); bq_ichg_uA is the BQ's own
# charge-current ADC (driver reports it negative while charging, 50 mA
# steps, 0 when not charging).
#
# Safety floor (ATTENDED tests only — systemctl poweroff does not truly
# power the unit off yet, see docs/standing-goals.md G1, so never rely
# on battery-guard's poweroff to end a discharge test):
#   FLOOR_MV (default 3650): on battery, two consecutive loaded VBAT
#   samples below it -> loud `wall` alert every sample: PLUG IN NOW.
#   Loaded VBAT sags ~0.3 V below the cell's resting voltage at ~1 A on
#   this unit, so 3650 mV loaded is far from empty.
#   Alerts: the terminal running this script, `wall` (all terminals/ssh),
#   and a critical desktop notification for $SUDO_USER.
#   TEST_NOTIFY=1 sends one test popup at start.
I=${1:-5}
OUT=${2:-/tmp/mt6351.csv}
S=/sys/kernel/debug/mt6351_probe/status
P=/sys/module/mt6351_probe/parameters/latch
PA=/sys/module/mt6351_probe/parameters/adc
B=$(ls -d /sys/class/power_supply/bq25890-charger-* 2>/dev/null | head -1)
[ -r "$S" ] || { echo "no $S — insmod mt6351-probe.ko first (and run as root)"; exit 1; }
echo 1 > "$P"
[ -w "$PA" ] && echo 1 > "$PA"   # fresh MT6351 VBAT (probe with adc param)
FLOOR_MV=${FLOOR_MV:-3650}
low=0
# Desktop popup for the user who ran sudo (Plasma/any freedesktop
# notification daemon). Tries notify-send, kdialog, then gdbus.
DUSER=${SUDO_USER:-}
DUID=$( [ -n "$DUSER" ] && id -u "$DUSER" 2>/dev/null )
notify() {
  [ -n "$DUID" ] || return 0
  bus="unix:path=/run/user/$DUID/bus"
  if command -v notify-send >/dev/null 2>&1; then
    runuser -u "$DUSER" -- env DBUS_SESSION_BUS_ADDRESS="$bus" notify-send -u critical -a mt6351-log "Battery test: PLUG IN NOW" "$1" 2>/dev/null && return
  fi
  if command -v kdialog >/dev/null 2>&1; then
    runuser -u "$DUSER" -- env DBUS_SESSION_BUS_ADDRESS="$bus" kdialog --title "Battery test" --passivepopup "PLUG IN NOW: $1" 30 2>/dev/null && return
  fi
  if command -v gdbus >/dev/null 2>&1; then
    runuser -u "$DUSER" -- env DBUS_SESSION_BUS_ADDRESS="$bus" gdbus call --session \
      --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications \
      --method org.freedesktop.Notifications.Notify mt6351-log 0 battery-caution \
      "Battery test: PLUG IN NOW" "$1" '[]' '{"urgency": <byte 2>}' 0 >/dev/null 2>&1
  fi
}
[ "${TEST_NOTIFY:-0}" = 1 ] && notify "test notification from mt6351-log.sh (floor ${FLOOR_MV} mV)"
echo "ts,uptime_s,nter,car_mAh,fg_mA,mt_vbat_mV,bq_online,bq_status,bq_charge_type,bq_vbat_mV,bq_ichg_mA" | tee "$OUT"
while :; do
  st=$(cat "$S")
  up=$(cut -d' ' -f1 /proc/uptime)
  fg=$(echo "$st" | sed -n 's/^CURRENT_OUT .*-> \(-\{0,1\}[0-9]*\) mA.*/\1/p')
  car=$(echo "$st" | sed -n 's/^CAR .*-> \(-\{0,1\}[0-9.]*\) mAh.*/\1/p')
  nt=$(echo "$st" | sed -n 's/^NTER *\([0-9]*\) samples.*/\1/p')
  mv=$(echo "$st" | sed -n 's/^VBAT fresh *\([0-9]*\) mV.*/\1/p')
  on=; bs=; ct=; vb=; ic=
  if [ -n "$B" ]; then
    on=$(cat $B/online 2>/dev/null); bs=$(cat $B/status 2>/dev/null)
    ct=$(cat $B/charge_type 2>/dev/null)
    vb=$(( $(cat $B/voltage_now 2>/dev/null || echo 0) / 1000 ))
    ic=$(( $(cat $B/current_now 2>/dev/null || echo 0) / -1000 ))
  fi
  echo "$(date +%s),$up,$nt,$car,$fg,$mv,$on,$bs,$ct,$vb,$ic" | tee -a "$OUT"
  # floor check: prefer the MT6351 VBAT (works on battery at any level);
  # fall back to the BQ (drops out to 2304 below ~3.56 V loaded)
  fv=$vb; [ -n "$mv" ] && fv=$mv
  if [ "$on" = 0 ] && [ -n "$fv" ] && [ "$fv" -gt 2500 ] && [ "$fv" -lt "$FLOOR_MV" ]; then
    low=$((low + 1))
    if [ "$low" -ge 2 ]; then
      msg="mt6351-log: VBAT ${fv} mV < floor ${FLOOR_MV} mV on battery (CAR ${car} mAh) - PLUG IN THE CHARGER NOW"
      echo "!!! $msg"; wall "$msg" 2>/dev/null; notify "$msg"
    fi
  else
    low=0
  fi
  sleep "$I"
done
