#!/bin/bash
# battery-guard.sh — Gemini PDA battery safety daemon
#
# Motivation (experiments on this device):
#   1. Keep the battery at a safe level: WARN when VBAT is low, and do an
#      orderly poweroff before the Li-ion reaches a dangerous depth of
#      discharge (hard brown-out / preloader-only brick state).
#   2. Verify the device is ACTUALLY charging when USB is connected:
#      alert when VBUS is present but the BQ25896 is not charging (the
#      B-19/B-22 failure mode, where the OTG boost puts the charger IC
#      into source mode so an external charger cannot sink).
#
# Data source: /sys/class/power_supply/bq25890-charger-* (mainline
# bq25890_charger.c driving the TI BQ25896 on i2c0 @ 0x6b — see
# docs/hardware.md "Battery & charging"). Property semantics:
#   status        Charging | Discharging | Full | Not charging
#   online        1 when VBUS is present
#   voltage_now   VBAT in uV (chip ADC: 2.304 V + N * 20 mV). Under load
#                 this sags below OCV; thresholds below account for that.
#   current_now   charge-path current in uA. NEGATIVE while charging in
#                 principle — BUT in the mainline driver it stays 0 while
#                 online (the driver only re-triggers the ADC conversion
#                 when !online/hiz). Unreliable while charging; use
#                 charge_type (from the chip's chrg_status register) as
#                 the primary charge signal.
#   temp          degC * 10, rough (TS pin % -> temperature table).
#
# Thresholds (env-overridable — tune after first on-hardware readings):
#   BATTERY_GUARD_POLL_S=10          poll period
#   BATTERY_GUARD_WARN_LOW_MV=3650   "low battery" on battery power
#   BATTERY_GUARD_CRIT_MV=3500       orderly poweroff on battery power
#                                     (vendor hard power-off is 3400 mV —
#                                     3.18 mt_battery_meter.c:1141; 100 mV
#                                     of headroom so the shutdown completes)
#   BATTERY_GUARD_ALERT_COOLDOWN_S=300  min spacing between repeat alerts
#
# Outputs:
#   - journal:   logger -t battery-guard (journalctl -t battery-guard)
#   - state:     /run/battery-guard/state  (key=value for harnesses)
#   - history:   /var/log/battery-history.csv (one row per poll; rotated
#                 at 5 MiB to battery-history.csv.1)
#
# Safety note: a VBAT read below 2.5 V is treated as a READ ERROR, never
# as "critical" — the ADC floor is 2.304 V, so sub-2.5 V means the read
# failed (I2C glitch) and must not trigger a poweroff. Poweroff requires
# two consecutive CRIT samples.

# --- MT6351 fuel gauge mode (2026-10-03) ---------------------------------
# When /sys/class/power_supply/mt6351-battery exists (mt6351-gauge.ko, see
# devices/planet-geminipda/kernel/modules/mt6351-gauge and claude/power.md),
# the guard uses IT instead of the BQ25896 for the battery decisions:
#   - voltage_now comes from the MT6351 AUXADC, which works at any battery
#     level. The BQ25896 VBAT ADC returns code 0 (2.304 V) below ~3.56 V
#     loaded, i.e. the old BQ-only guard went BLIND exactly when the
#     battery was low.
#   - capacity is the coulomb-counted state of charge.
# On battery:
#   WARN  capacity <= GAUGE_WARN_PCT (10) or VBAT < GAUGE_WARN_MV (3450)
#         -> journal + desktop notification
#   CRIT  VBAT + |I| x R < GAUGE_CRIT_MV (3400; IR-compensated, see below;
#         vendor Android also shut down at 3.4 V), or raw VBAT <
#         GAUGE_FLOOR_MV (3100, under any load), or (capacity == 0 and
#         VBAT < GAUGE_CAP0_MV 3500), two consecutive samples
#         -> notify + systemctl poweroff.
#   IR compensation [2026-10-07]: this pack has ~0.28 Ohm, so heavy load
#   sags the terminal voltage far below the cell's own voltage. A full
#   10-CPU load at 70 % read 3296 mV and the old raw 3400 mV rule powered
#   the device off. The guard now adds |current_avg| x GAUGE_R_MOHM (250,
#   a bit under the measured 0.28 Ohm so it errs towards shutting down)
#   before comparing; the raw 3.1 V floor still protects the cell.
# Also persists the gauge's learned charge_full to
# /var/lib/gemini-gauge/charge_full and restores it when the gauge appears.
# NOTE (G1): systemctl poweroff does not yet truly power the unit off
# (~0.5 W keeps flowing), but it is still ~8x less drain than running.
# Without the gauge the original BQ logic below runs unchanged.

set -u

PSY_GLOB='/sys/class/power_supply/bq25890-charger-*'
RUNDIR=/run/battery-guard
STATE=$RUNDIR/state
HIST=/var/log/battery-history.csv
POLL_S=${BATTERY_GUARD_POLL_S:-10}
WARN_LOW_MV=${BATTERY_GUARD_WARN_LOW_MV:-3650}
CRIT_MV=${BATTERY_GUARD_CRIT_MV:-3500}
ALERT_COOLDOWN_S=${BATTERY_GUARD_ALERT_COOLDOWN_S:-300}
STUCK_CHARGE_MIN=${BATTERY_GUARD_STUCK_CHARGE_MIN:-15}
GAUGE=/sys/class/power_supply/mt6351-battery
G_WARN_PCT=${BATTERY_GUARD_GAUGE_WARN_PCT:-10}
G_WARN_MV=${BATTERY_GUARD_GAUGE_WARN_MV:-3450}
G_CRIT_MV=${BATTERY_GUARD_GAUGE_CRIT_MV:-3400}
G_CAP0_MV=${BATTERY_GUARD_GAUGE_CAP0_MV:-3500}
G_FLOOR_MV=${BATTERY_GUARD_GAUGE_FLOOR_MV:-3100}
G_R_MOHM=${BATTERY_GUARD_GAUGE_R_MOHM:-250}
FCC_SAVE=/var/lib/gemini-gauge/charge_full
# Charge across power-offs (2026-10-09): after a real power-off the gauge
# starts from a voltage estimate taken under boot load, which on this worn
# cell reads far too low (100 % at night -> ~40 % next morning, then lower).
# Save the charge while running and at shutdown; at start, give the gauge
# max(saved, its own estimate) — "max" so charge gained while switched off
# (the charging screen) is not thrown away. Older than CHG_SAVE_MAX_AGE_S
# (or another battery) -> the gauge's estimate stands.
CHG_SAVE=/var/lib/gemini-gauge/charge_now
CHG_SAVE_MAX_AGE_S=${BATTERY_GUARD_CHARGE_SAVE_MAX_AGE_S:-1209600}
g_chg_saved=""
g_seen=0
g_fcc_last=""
g_dis_since=0
g_low_hits=0
CAP=?
SRC=bq
# Charger input limit (uA). The BQ25896 falls back to 500 mA whenever it
# re-detects its input (plug-in, or a VBUS dip without a new plug-in event),
# and then the device drains while "charging" (seen 2026-10-08: 500 mA in,
# battery -480 mA). The udev rule only fires on plug-in; restore it here on
# every poll while a charger is online. 0 = leave the limit alone.
ILIM_UA=${BATTERY_GUARD_INPUT_LIMIT_UA:-2500000}

# Put the charger input limit back to ILIM_UA if the chip reset it.
enforce_ilim() {
    [ "$ILIM_UA" -gt 0 ] 2>/dev/null || return 0
    local p cur
    for p in $PSY_GLOB; do
        [ -w "$p/input_current_limit" ] || continue
        [ "$(cat "$p/online" 2>/dev/null)" = 1 ] || continue
        cur=$(cat "$p/input_current_limit" 2>/dev/null) || continue
        case "$cur" in (*[!0-9]*|'') continue ;; esac
        if [ "$cur" -lt "$ILIM_UA" ]; then
            if echo "$ILIM_UA" > "$p/input_current_limit" 2>/dev/null; then
                log "charger input limit was ${cur} uA -> restored ${ILIM_UA} uA"
            fi
        fi
    done
}

mkdir -p "$RUNDIR"
if [ ! -s "$HIST" ]; then
    echo 'ts,online,status,charge_type,vbat_mv,ibat_uA,temp_10c,guard,source,capacity' > "$HIST"
fi

last_alert_ts=0
stuck_since=0
crit_strikes=0
prev_guard=""

now() { date '+%s'; }
stamp() { date '+%F %T'; }
log() { echo "[$(stamp)] $*" | logger -t battery-guard -p daemon.info; }

# notify_desktop MSG — critical notification on every logged-in graphical
# session (any freedesktop notification daemon; best effort, never blocks)
notify_desktop() {
    local b uid u
    for b in /run/user/[0-9]*/bus; do
        [ -S "$b" ] || continue
        uid=${b#/run/user/}; uid=${uid%/bus}
        [ "$uid" -ge 1000 ] 2>/dev/null || continue
        u=$(id -nu "$uid" 2>/dev/null) || continue
        if command -v notify-send >/dev/null 2>&1; then
            timeout 5 runuser -u "$u" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$b" \
                notify-send -u critical -a battery-guard "Battery" "$1" >/dev/null 2>&1 && continue
        fi
        if command -v gdbus >/dev/null 2>&1; then
            timeout 5 runuser -u "$u" -- env DBUS_SESSION_BUS_ADDRESS="unix:path=$b" \
                gdbus call --session --dest org.freedesktop.Notifications \
                --object-path /org/freedesktop/Notifications \
                --method org.freedesktop.Notifications.Notify battery-guard 0 battery-caution \
                "Battery" "$1" '[]' '{"urgency": <byte 2>}' 0 >/dev/null 2>&1
        fi
    done
}

# alert LEVEL MSG — rate-limited (ALERT_COOLDOWN_S); WARN/CRIT also notify
alert() {
    local t
    t=$(now)
    if [ "$t" -ge $((last_alert_ts + ALERT_COOLDOWN_S)) ]; then
        log "ALERT $1: $2"
        last_alert_ts=$t
        case "$1" in WARN|CRIT) notify_desktop "$2" ;; esac
    fi
}

# gauge_poll — battery decisions from the MT6351 gauge (see header).
# Sets ONLINE STATUS CHGT VBAT_MV IBAT TEMP CAP GUARD; may power off.
# Give the gauge back the charge saved before the last shutdown, if that is
# more than its own start estimate.
restore_charge() {
    local saved at cur age
    [ -s "$CHG_SAVE" ] && [ -w "$GAUGE/charge_now" ] || return 0
    read -r saved at < "$CHG_SAVE" 2>/dev/null || return 0
    case "$saved$at" in (*[!0-9]*|'') return 0 ;; esac
    age=$(( $(now) - at ))
    if [ "$age" -lt 0 ] || [ "$age" -gt "$CHG_SAVE_MAX_AGE_S" ]; then
        log "gauge: saved charge is $((age / 3600)) h old — keeping the gauge's own estimate"
        return 0
    fi
    cur=$(cat "$GAUGE/charge_now" 2>/dev/null || echo 0)
    case "$cur" in (*[!0-9]*|'') cur=0 ;; esac
    if [ "$saved" -gt "$cur" ]; then
        if echo "$saved" > "$GAUGE/charge_now" 2>/dev/null; then
            log "gauge: restored charge ${saved} uAh saved $((age / 60)) min ago (start estimate was ${cur} uAh)"
        else
            log "gauge: could not restore charge ${saved} uAh (old gauge module?)"
        fi
    else
        log "gauge: start estimate ${cur} uAh >= saved ${saved} uAh — kept (charged while off?)"
    fi
}

# Save the charge (uAh + time) when it changed by >= 1 % of charge_full, or
# always with "force" (shutdown).
save_charge() {
    local c f
    c=$(cat "$GAUGE/charge_now" 2>/dev/null) || return 0
    case "$c" in (*[!0-9]*|'') return 0 ;; esac
    if [ "${1:-}" != force ] && [ -n "$g_chg_saved" ]; then
        f=$(cat "$GAUGE/charge_full" 2>/dev/null || echo 2600000)
        [ $(( c > g_chg_saved ? c - g_chg_saved : g_chg_saved - c )) -lt $((f / 100)) ] && return 0
    fi
    mkdir -p "${CHG_SAVE%/*}" && echo "$c $(now)" > "$CHG_SAVE.tmp" && mv -f "$CHG_SAVE.tmp" "$CHG_SAVE"
    g_chg_saved=$c
}

gauge_poll() {
    local gst v f
    SRC=gauge
    if [ "$g_seen" = 0 ]; then
        g_seen=1
        if [ -s "$FCC_SAVE" ]; then
            f=$(cat "$FCC_SAVE")
            if echo "$f" > "$GAUGE/charge_full" 2>/dev/null; then
                log "gauge: restored charge_full ${f} uAh"
            else
                log "gauge: could not restore charge_full ${f} uAh (out of range?)"
            fi
        fi
        restore_charge
        log "gauge mode: using $GAUGE (warn <=${G_WARN_PCT}% or <${G_WARN_MV}mV, crit <${G_CRIT_MV}mV or 0% & <${G_CAP0_MV}mV)"
    fi
    f=$(cat "$GAUGE/charge_full" 2>/dev/null || echo "")
    if [ -n "$f" ] && [ "$f" != "$g_fcc_last" ]; then
        mkdir -p "${FCC_SAVE%/*}" && echo "$f" > "$FCC_SAVE"
        [ -n "$g_fcc_last" ] && log "gauge: charge_full ${g_fcc_last} -> ${f} uAh (saved)"
        g_fcc_last=$f
    fi

    save_charge

    gst=$(cat "$GAUGE/status" 2>/dev/null || echo Unknown)
    CAP=$(cat "$GAUGE/capacity" 2>/dev/null || echo "?")
    v=$(cat "$GAUGE/voltage_now" 2>/dev/null || echo "")
    IBAT=$(cat "$GAUGE/current_avg" 2>/dev/null || echo "?")
    STATUS=$gst
    case "$v" in (*[!0-9]*|'') VBAT_MV=? ;; (*) VBAT_MV=$((v / 1000)) ;; esac
    case "$CAP" in (*[!0-9]*|'') CAP=? ;; esac
    if read_psy; then
        ONLINE=$(cat "$PSY/online" 2>/dev/null || echo "?")
        CHGT=$(cat "$PSY/charge_type" 2>/dev/null || echo "?")
        TEMP=$(cat "$PSY/temp" 2>/dev/null || echo "?")
        case "$TEMP" in (*[!0-9]*|'') TEMP=? ;; esac
    else
        [ "$gst" = Discharging ] && ONLINE=0 || ONLINE=1
    fi

    if [ "$ONLINE" = 1 ]; then
        crit_strikes=0
        if [ "$gst" = Discharging ]; then
            # brief dips under a load spike are normal; complain after 2 min
            [ "$g_dis_since" -eq 0 ] && g_dis_since=$t
            if [ $((t - g_dis_since)) -ge 120 ]; then
                GUARD=NOTCHARGING
                alert WARN "USB present but the battery has been discharging for 2+ min (load above charger input?) — ${CAP}%, ${VBAT_MV} mV"
            fi
        else
            g_dis_since=0
        fi
        return
    fi

    if [ "$VBAT_MV" = "?" ] || [ "$VBAT_MV" -lt 2500 ]; then
        GUARD=READERR
        alert ERROR "gauge VBAT read failed (${v:-empty}) — capacity ${CAP}%"
        [ "$CAP" = 0 ] || return
        crit_strikes=$((crit_strikes + 1))
    fi
    # IR-compensated cell voltage: current_avg is uA, + = charging
    VCOMP_MV=$VBAT_MV
    case "$VBAT_MV:$IBAT" in
        ([0-9]*:-[0-9]*) VCOMP_MV=$((VBAT_MV + (-IBAT / 1000) * G_R_MOHM / 1000)) ;;
    esac
    if [ "$VBAT_MV" = "?" ] || [ "$VBAT_MV" -lt 2500 ]; then
        :	# handled above
    elif [ "$VCOMP_MV" -lt "$G_CRIT_MV" ] || [ "$VBAT_MV" -lt "$G_FLOOR_MV" ] ||
         { [ "$CAP" = 0 ] && [ "$VBAT_MV" -lt "$G_CAP0_MV" ]; }; then
        crit_strikes=$((crit_strikes + 1))
    else
        crit_strikes=0
        # Warn only on the second low poll in a row: one low sample can be
        # a load spike (2026-10-08: one reading of 5 % at 68 %).
        if { [ "$CAP" != "?" ] && [ "$CAP" -le "$G_WARN_PCT" ]; } || [ "$VCOMP_MV" -lt "$G_WARN_MV" ]; then
            g_low_hits=$((g_low_hits + 1))
        else
            g_low_hits=0
        fi
        if [ "$g_low_hits" -ge 2 ]; then
            GUARD=LOW
            alert WARN "battery low: ${CAP}% (${VBAT_MV} mV under load, ${VCOMP_MV} mV compensated) — connect the charger"
        fi
        return
    fi

    GUARD=CRITICAL
    if [ "$crit_strikes" -ge 2 ]; then
        log "CRITICAL (gauge): ${CAP}%, VBAT ${VBAT_MV} mV (${VCOMP_MV} mV IR-compensated, I ${IBAT} uA) on battery — powering off in 10 s"
        notify_desktop "Battery empty (${CAP}%, ${VBAT_MV} mV) — powering off in 10 s. Connect the charger!"
        write_state
        sleep 10
        if [ "$(cat "${PSY:-/nonexistent}/online" 2>/dev/null || echo 0)" = 1 ]; then
            log "charger connected during the countdown — poweroff cancelled"
            crit_strikes=0
            return
        fi
        save_charge force
        exec systemctl poweroff
    fi
    alert CRIT "battery critical: ${CAP}%, ${VBAT_MV} mV (${VCOMP_MV} mV compensated) — will power off on the next sample unless the charger is connected"
}

read_psy() {
    local d
    for d in $PSY_GLOB; do
        if [ -e "$d/status" ]; then PSY=$d; return 0; fi
    done
    return 1
}

write_state() {
    cat > "$STATE" <<EOF
ts=$(now)
supply=${PSY##*/}
online=${ONLINE:-?}
status=${STATUS:-?}
charge_type=${CHGT:-?}
vbat_mv=${VBAT_MV:-?}
ibat_uA=${IBAT:-?}
temp_10c=${TEMP:-?}
guard=${GUARD:-?}
source=${SRC:-?}
capacity=${CAP:-?}
EOF
}

rotate_hist() {
    local sz
    sz=$(stat -c %s "$HIST" 2>/dev/null || echo 0)
    if [ "$sz" -gt 5242880 ]; then
        mv "$HIST" "$HIST.1"
        echo 'ts,online,status,charge_type,vbat_mv,ibat_uA,temp_10c,guard,source,capacity' > "$HIST"
    fi
}

# systemd stops us at shutdown/reboot: save the charge one last time.
trap '[ "$g_seen" = 1 ] && save_charge force; log "battery-guard stopping (charge saved)"; exit 0' TERM INT

log "battery-guard started (poll=${POLL_S}s warn<${WARN_LOW_MV}mV crit<${CRIT_MV}mV)"

while :; do
    t=$(now)
    GUARD=OK
    ONLINE=?; STATUS=?; VBAT_MV=?; IBAT=?; TEMP=?; CAP=?; SRC=bq

    if [ -e "$GAUGE/capacity" ]; then
        gauge_poll
    elif ! read_psy; then
        GUARD=NOSUPPLY
        alert ERROR "no bq25890-charger power supply under /sys/class/power_supply — charger driver did not probe (check dmesg)"
    else
        ONLINE=$(cat "$PSY/online" 2>/dev/null || echo "?")
        STATUS=$(cat "$PSY/status" 2>/dev/null || echo "Unknown")
        CHGT=$(cat "$PSY/charge_type" 2>/dev/null || echo "?")
        VBAT=$(cat "$PSY/voltage_now" 2>/dev/null || echo 0)
        IBAT=$(cat "$PSY/current_now" 2>/dev/null || echo 0)
        TEMP=$(cat "$PSY/temp" 2>/dev/null || echo 0)
        # validate numerics; fall back to ? on read errors
        case "$VBAT" in (*[!0-9]*|'') GUARD=READERR ;;
            (*) VBAT_MV=$((VBAT / 1000)) ;;
        esac
        case "$IBAT" in (*[!0-9-]*|'') IBAT=? ;; esac
        case "$TEMP" in (*[!0-9]*|'') TEMP=? ;; esac

        if [ "$ONLINE" = 1 ]; then
            # USB present — the whole point: is it charging?
            case "$STATUS" in
                Full)
                    : # holding at full charge: fine
                    ;;
                Charging)
                    # stuck-charge detection: status says Charging but the
                    # chip's charge-status register says NOT charging
                    # (charge_type=NONE) for a long time -> sense fault.
                    # (current_now is NOT usable here: the mainline driver
                    # reads it 0 while online — see header.)
                    if [ "$CHGT" = "None" ]; then
                        [ "$stuck_since" -eq 0 ] && stuck_since=$t
                        if [ $((t - stuck_since)) -gt $((STUCK_CHARGE_MIN * 60)) ]; then
                            alert WARN "status=Charging but charge_type=None for ${STUCK_CHARGE_MIN} min — sense/charger fault?"
                        fi
                    else
                        stuck_since=0
                    fi
                    ;;
                *)
                    GUARD=NOTCHARGING
                    alert WARN "USB present (online=1) but status='${STATUS}' — NOT charging (OTG/boost mode? wrong port/cable? see B-19/B-22)"
                    ;;
            esac
        else
            stuck_since=0
            case "$VBAT_MV" in
                ?)
                    GUARD=READERR
                    alert ERROR "VBAT read failed — cannot assess battery level"
                    ;;
                *)
                    if [ "$VBAT_MV" -lt 2500 ]; then
                        # below ADC floor: read error, NOT a valid reading
                        GUARD=READERR
                        alert ERROR "VBAT ${VBAT_MV} mV below ADC floor — read error, ignoring"
                    elif [ "$VBAT_MV" -lt "$CRIT_MV" ]; then
                        # two consecutive CRIT samples before pulling the plug
                        crit_strikes=$((crit_strikes + 1))
                        if [ "$crit_strikes" -ge 2 ]; then
                            GUARD=CRITICAL
                            log "CRITICAL: VBAT ${VBAT_MV} mV < ${CRIT_MV} mV with no USB — powering off in 10 s (vendor hard-off is 3400 mV)"
                            write_state
                            sleep 10
                            exec systemctl poweroff
                        else
                            GUARD=CRITICAL
                            alert CRIT "VBAT ${VBAT_MV} mV < ${CRIT_MV} mV with no USB — will power off on next sample if still critical (connect USB!)"
                        fi
                    else
                        crit_strikes=0
                        if [ "$VBAT_MV" -lt "$WARN_LOW_MV" ]; then
                            GUARD=LOW
                            alert WARN "low battery on battery power: VBAT ${VBAT_MV} mV (< ${WARN_LOW_MV} mV) — connect USB"
                        fi
                    fi
                    ;;
            esac
        fi

        # temperature sanity (rough TS-pin reading)
        if [ "$TEMP" != ? ] && [ "$TEMP" -gt 450 ]; then
            alert WARN "battery TS pin ~$((TEMP / 10)) C — hot; charging may be throttled (JEITA)"
        fi
    fi

    if [ "$GUARD" != "$prev_guard" ]; then
        log "state: ${prev_guard:-<init>} -> ${GUARD} (online=${ONLINE} status=${STATUS} vbat=${VBAT_MV}mV)"
        prev_guard=$GUARD
    fi
    enforce_ilim
    write_state
    echo "$(date '+%F %T'),${ONLINE},${STATUS},${CHGT:-?},${VBAT_MV},${IBAT},${TEMP},${GUARD},${SRC},${CAP}" >> "$HIST"
    rotate_hist
    sleep "$POLL_S" & wait $!       # wait: lets the TERM trap run at once
done
