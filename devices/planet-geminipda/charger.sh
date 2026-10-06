# charger.sh — off-mode-charging screen for the Gemini PDA initrd (step 1: text MVP)
#
# Sourced by /init (busybox ash) right after /proc, /sys and /dev are mounted.
# Docs: project claude/charger-mode.md.
#
# WHY: a charger plugged into a switched-off Gemini powers the PMIC on, and LK
# then boots the normal boot image in its off-mode-charging mode. Without this
# file that is a full Debian boot. Here we instead show the battery state and:
#   - long-press Esc/On (KEY_POWER, sent by mt6351-keys after 800 ms) -> return,
#     /init carries on with the normal boot (no reboot);
#   - charger unplugged (2 polls) -> real poweroff;
#   - any key -> screen on; backlight off after CHG_BL_TIMEOUT s idle.
# The rootfs is never touched in charger mode.
#
# DETECTION: LK's /chosen "atag,boot" = 3 x u32 BE {size 3, tag 0x41000802,
# boot mode}, stored little-endian; mode 8 = KERNEL_POWER_OFF_CHARGING_BOOT (0 = normal). Verified on
# glass 2026-10-05 (logs/boot-mode-20261005-160955.txt). CONFIG_CMDLINE_FORCE
# only stops the kernel *using* LK's cmdline; /chosen stays readable.
#
# FAIL-SAFE: anything missing (no charger supply, no tty) -> return, i.e. a
# normal boot. Nothing here uses `exit` (this runs inside PID 1).
#
# Testing off-device: CHG_DT / CHG_SYS / CHG_TTY / CHG_RUN point it at a fake tree.

CHG_DT=${CHG_DT:-/proc/device-tree}
CHG_SYS=${CHG_SYS:-/sys}
CHG_RUN=${CHG_RUN:-/tmp}
CHG_GAUGE_KO=${CHG_GAUGE_KO:-/lib/modules/mt6351-gauge.ko}
CHG_IINLIM=2500000        # BQ input current limit (uA); Debian's udev rule isn't here
CHG_BL_TIMEOUT=15         # s without a key before the backlight goes off
CHG_MIN_START_PCT=3       # below this a start needs a second long-press
CHG_POLL=1                # s per loop

chg_log() { echo "<6>gemini-charger: $*" > /dev/kmsg 2>/dev/null; echo "==> charger: $*"; }

# 0 = this is a charger boot
# LK writes the atag words LITTLE-endian (raw bytes on glass 2026-10-05:
# 03 00 00 00 | 02 08 00 41 | 08 00 00 00), unlike normal big-endian DT
# cells. Compare raw bytes (dd + cmp) against LE 8, and BE 8 as a fallback
# for other LK builds; no od formatting involved.
charger_mode_detect() {
    CHG_BOOTREASON=$(tr '\0 ' '\n\n' < "$CHG_DT/chosen/bootargs" 2>/dev/null | sed -n 's/^androidboot\.bootreason=//p' | sed -n 1p)
    mkdir -p "$CHG_RUN"
    if [ -f "$CHG_DT/chosen/atag,boot" ]; then
        dd if="$CHG_DT/chosen/atag,boot" of="$CHG_RUN/chg-bm" bs=1 skip=8 count=4 2>/dev/null
        printf '\010\000\000\000' > "$CHG_RUN/chg-bm8le"   # little-endian 8 (this LK)
        printf '\000\000\000\010' > "$CHG_RUN/chg-bm8be"   # big-endian 8
        if cmp -s "$CHG_RUN/chg-bm" "$CHG_RUN/chg-bm8le" || cmp -s "$CHG_RUN/chg-bm" "$CHG_RUN/chg-bm8be"; then
            CHG_BOOTMODE=8; return 0
        fi
        CHG_BOOTMODE=other
        return 1
    fi
    # no atag,boot at all: fall back to LK's bootreason
    CHG_BOOTMODE=none
    [ "$CHG_BOOTREASON" = usb ] && return 0
    return 1
}

# ---- drawing ----------------------------------------------------------------
# Screen = fbcon text console (geminipda-fb, rotated to landscape, TER16x32):
# 135 x 33 cells of 16 x 32 px. A "big pixel" = 4 cols x 2 rows = 64 x 64 px.

chg_out() { printf '%b' "$*" >> "$CHG_TTY"; }

chg_glyph() {
    case "$1" in
    0) echo "111 101 101 101 111" ;;  1) echo "010 110 010 010 111" ;;
    2) echo "111 001 111 100 111" ;;  3) echo "111 001 111 001 111" ;;
    4) echo "101 101 111 001 001" ;;  5) echo "111 100 111 001 111" ;;
    6) echo "111 100 111 101 111" ;;  7) echo "111 001 001 001 001" ;;
    8) echo "111 101 111 101 111" ;;  9) echo "111 101 111 001 111" ;;
    %) echo "101 001 010 100 101" ;;  *) echo "111 001 011 000 010" ;;  # ?
    esac
}

# chg_big ROW COLOR CHAR... — 5x2 text rows per glyph row, glyph 12 cols + 4 gap
chg_big() {
    row=$1; colr=$2; shift 2
    chars="$*"   # set -- below reuses the positional params
    ng=$#; width=$((ng * 16 - 4)); col=$(( (CHG_COLS - width) / 2 + 1 ))
    B="\033[${colr}m    \033[0m"; S="    "
    out=""
    pr=1
    while [ $pr -le 5 ]; do
        line=""
        for ch in $chars; do
            set -- $(chg_glyph "$ch")
            eval "bits=\$$pr"
            case "$bits" in
            111) seg="$B$B$B" ;; 101) seg="$B$S$B" ;; 110) seg="$B$B$S" ;; 011) seg="$S$B$B" ;;
            100) seg="$B$S$S" ;; 010) seg="$S$B$S" ;; 001) seg="$S$S$B" ;; *) seg="$S$S$S" ;;
            esac
            line="$line$seg$S"
        done
        r1=$((row + (pr - 1) * 2)); r2=$((r1 + 1))
        out="$out\033[${r1};1H\033[2K\033[${r1};${col}H$line\033[${r2};1H\033[2K\033[${r2};${col}H$line"
        pr=$((pr + 1))
    done
    chg_out "$out"
}

# chg_center ROW TEXT [SGR]
chg_center() {
    t=$2; col=$(( (CHG_COLS - ${#t}) / 2 + 1 )); [ $col -lt 1 ] && col=1
    chg_out "\033[$1;1H\033[2K\033[$1;${col}H\033[${3:-0}m$t\033[0m"
}

# chg_battery ROW PCT FILLCOLOR — 80 x 7 cells outline + nub
chg_battery() {
    row=$1; pct=$2; fc=$3
    bw=80; col=$(( (CHG_COLS - bw) / 2 + 1 ))
    inner=$((bw - 4)); fill=$(( inner * pct / 100 )); [ $fill -lt 0 ] && fill=0; [ $fill -gt $inner ] && fill=$inner
    rest=$((inner - fill))
    edge=$(printf "%${bw}s" ''); fs=$(printf "%${fill}s" ''); rs=$(printf "%${rest}s" '')
    W="\033[47m"; Z="\033[0m"
    out="\033[${row};1H\033[2K\033[${row};${col}H$W$edge$Z"
    i=1
    while [ $i -le 5 ]; do
        r=$((row + i)); nub=""
        [ $i -ge 2 ] && [ $i -le 4 ] && nub="$W  $Z"
        out="$out\033[${r};1H\033[2K\033[${r};${col}H$W  $Z\033[${fc}m$fs$Z$rs$W  $Z$nub"
        i=$((i + 1))
    done
    r=$((row + 6))
    out="$out\033[${r};1H\033[2K\033[${r};${col}H$W$edge$Z"
    chg_out "$out"
}

# ---- hardware ---------------------------------------------------------------

chg_read() { cat "$1" 2>/dev/null; }

chg_bl() {   # chg_bl on|off
    [ -n "$CHG_BL" ] || return
    if [ "$1" = on ]; then echo 0 > "$CHG_BL/bl_power"; else echo 4 > "$CHG_BL/bl_power"; fi 2>/dev/null
}

chg_start_readers() {
    : > "$CHG_RUN/chg-keys"
    # byte patterns (little-endian) for struct input_event fields
    printf '\001\000' > "$CHG_RUN/p-evkey"                 # type  = EV_KEY (1)
    printf '\164\000' > "$CHG_RUN/p-power"                 # code  = KEY_POWER (116)
    printf '\001\000\000\000' > "$CHG_RUN/p-press"         # value = 1 (press)
    CHG_READERS=""
    for d in "$CHG_SYS"/class/input/event*; do
        [ -e "$d" ] || continue
        name=$(chg_read "$d/device/name")
        case "$name" in *ouch*) continue ;; esac          # skip touchscreens
        dev=/dev/input/$(basename "$d")
        [ -c "$dev" ] || continue
        (
            ev=$CHG_RUN/chg-$(basename "$dev")
            # Keep ONE open file on the device for the whole loop: evdev
            # buffers per open file, so a dd that re-opened the device for
            # every event lost whatever arrived in between — e.g. the
            # keyboard's EV_KEY right after its EV_MSC scan code (on glass
            # 2026-10-05: keyboard presses never showed up).
            exec 3< "$dev" || exit 0
            while :; do
                # struct input_event (arm64, 24 B): timeval 16 B, u16 type @16,
                # u16 code @18, s32 value @20 — compared as raw bytes (no od).
                # One read() on evdev returns whole events; bs=24 = one event.
                dd of="$ev" bs=24 count=1 <&3 2>/dev/null || { sleep 1; continue; }
                dd if="$ev" of="$ev.t" bs=1 skip=16 count=2 2>/dev/null
                cmp -s "$ev.t" "$CHG_RUN/p-evkey" || continue
                dd if="$ev" of="$ev.v" bs=1 skip=20 count=4 2>/dev/null
                cmp -s "$ev.v" "$CHG_RUN/p-press" || continue
                dd if="$ev" of="$ev.c" bs=1 skip=18 count=2 2>/dev/null
                if cmp -s "$ev.c" "$CHG_RUN/p-power"; then echo "116 1"; else echo "0 1"; fi >> "$CHG_RUN/chg-keys"
            done
        ) &
        CHG_READERS="$CHG_READERS $!"
        chg_log "keys: $dev ($name)"
    done
}

chg_stop_readers() {
    for p in $CHG_READERS; do kill "$p" 2>/dev/null; done
    pkill -x dd 2>/dev/null; sleep 1; pkill -x dd 2>/dev/null
}

chg_leave() {   # restore the console for the normal boot
    chg_stop_readers
    chg_bl on
    chg_out "\033[0m\033[2J\033[H\033[?25h"
    [ -n "$CHG_SAVED_PRINTK" ] && echo "$CHG_SAVED_PRINTK" > /proc/sys/kernel/printk 2>/dev/null
}

chg_poweroff() {
    chg_log "charger removed: powering off"
    chg_bl on
    chg_out "\033[2J"
    chg_center 16 "Charger removed - switching off" 1
    sleep 2
    chg_stop_readers
    sync
    poweroff -f
    # still alive? (charger re-plugged -> kernel WDT fallback -> LK charger boot)
    sleep 5
    chg_log "poweroff returned: continuing normal boot"
}

# ---- main -------------------------------------------------------------------
# Returns when the normal boot should continue (long-press, poweroff failure,
# or missing hardware).
charger_main() {
    chg_log "charger boot (atag,boot mode ${CHG_BOOTMODE:-none}, bootreason ${CHG_BOOTREASON:-none})"

    CHG_SAVED_PRINTK=$(cat /proc/sys/kernel/printk 2>/dev/null | tr '\t' ' ')
    echo "1 4 1 7" > /proc/sys/kernel/printk 2>/dev/null   # keep kernel messages off the screen

    if [ -f "$CHG_GAUGE_KO" ] && [ ! -d "$CHG_SYS/module/mt6351_gauge" ]; then
        insmod "$CHG_GAUGE_KO" system=1 2>/dev/null || chg_log "gauge insmod failed (showing charger data only)"
    fi
    BAT=$CHG_SYS/class/power_supply/mt6351-battery
    [ -d "$BAT" ] || BAT=""
    CHG=""
    for c in "$CHG_SYS"/class/power_supply/bq25890-charger*; do [ -d "$c" ] && CHG=$c && break; done
    if [ -z "$CHG" ]; then
        chg_log "no bq25890 charger supply: normal boot"
        [ -n "$CHG_SAVED_PRINTK" ] && echo "$CHG_SAVED_PRINTK" > /proc/sys/kernel/printk
        return
    fi

    CHG_TTY=${CHG_TTY:-/dev/tty0}
    if ! ( : > "$CHG_TTY" ) 2>/dev/null; then
        chg_log "no console tty: normal boot"
        [ -n "$CHG_SAVED_PRINTK" ] && echo "$CHG_SAVED_PRINTK" > /proc/sys/kernel/printk
        return
    fi
    set -- $(stty -F "$CHG_TTY" size 2>/dev/null)
    CHG_ROWS=${1:-33}; CHG_COLS=${2:-135}
    [ "$CHG_COLS" -ge 90 ] 2>/dev/null || CHG_COLS=135

    CHG_BL=""
    for b in "$CHG_SYS"/class/backlight/*; do [ -d "$b" ] && CHG_BL=$b && break; done

    chg_start_readers
    chg_bl on
    chg_out "\033[0m\033[?25l\033[2J"

    now=$(cut -d. -f1 /proc/uptime)
    last=0; lit_at=$now; lit=1; off_n=0; force_until=0; msg=""; msg_until=0; frame=""
    while :; do
        now=$(cut -d. -f1 /proc/uptime)

        # -- keys
        start=0
        n=$(wc -l < "$CHG_RUN/chg-keys")
        if [ "$n" -gt "$last" ]; then
            keys=$(sed -n "$((last + 1)),${n}p" "$CHG_RUN/chg-keys")
            last=$n
            while read -r code val; do
                [ "$val" = 1 ] || continue
                lit_at=$now
                [ $lit = 1 ] || { chg_bl on; lit=1; frame=""; }
                [ "$code" = 116 ] && start=1        # KEY_POWER (Esc/On held 800 ms)
            done <<EOF
$keys
EOF
        fi

        # -- supplies
        online=$(chg_read "$CHG/online")
        if [ -n "$BAT" ]; then
            cap=$(chg_read "$BAT/capacity"); st=$(chg_read "$BAT/status")
            ua=$(chg_read "$BAT/current_now"); uv=$(chg_read "$BAT/voltage_now")
        else
            cap=""; st=$(chg_read "$CHG/status"); ua=""; uv=$(chg_read "$CHG/voltage_now")
        fi
        [ "$st" = Full ] && cap=100

        if [ "$online" = 1 ]; then
            off_n=0
            lim=$(chg_read "$CHG/input_current_limit")
            [ -n "$lim" ] && [ "$lim" != "$CHG_IINLIM" ] && echo "$CHG_IINLIM" > "$CHG/input_current_limit" 2>/dev/null
        else
            off_n=$((off_n + 1))
            if [ $off_n -ge 2 ]; then
                chg_poweroff
                chg_leave; return
            fi
        fi

        # -- start request
        if [ $start = 1 ]; then
            if [ -n "$cap" ] && [ "$cap" -lt "$CHG_MIN_START_PCT" ] && [ "$now" -gt "$force_until" ]; then
                msg="Battery very low - keep charging (hold Esc/On again to start anyway)"
                msg_until=$((now + 10)); force_until=$((now + 10))
                chg_log "start refused at ${cap}%"
            else
                chg_log "long-press: continuing normal boot (${cap:-?}%, $st)"
                chg_out "\033[2J"; chg_center 16 "Starting..." 1
                chg_leave; return
            fi
        fi
        [ "$now" -ge "$msg_until" ] && msg=""

        # -- backlight timeout
        if [ $lit = 1 ] && [ $((now - lit_at)) -ge $CHG_BL_TIMEOUT ]; then
            chg_bl off; lit=0
        fi

        # -- draw (only when something visible changed)
        if [ $lit = 1 ]; then
            ima=""; [ -n "$ua" ] && ima=$((ua / 1000))
            case "$st" in
                Full) stxt="Fully charged" ;; Charging) stxt="Charging" ;;
                "Not charging") stxt="Not charging" ;; Discharging) stxt="Charger too weak" ;;
                *) stxt=${st:-Unknown} ;;
            esac
            detail=""
            if [ -n "$ima" ]; then
                sign="+"; a=$ima; [ $a -lt 0 ] && { sign="-"; a=$((-a)); }
                detail=$(printf '%s%d.%02d A' "$sign" $((a / 1000)) $(( (a % 1000) / 10 )))
            fi
            [ -n "$uv" ] && detail="$detail   $(printf '%d.%02d V' $((uv / 1000000)) $(( (uv % 1000000) / 10000 )))"
            new="$cap|$stxt|$((${ima:-0} / 50))|$msg"
            if [ "$new" != "$frame" ]; then
                frame=$new
                if [ -n "$cap" ]; then
                    fc=42; [ "$cap" -lt 30 ] && fc=43; [ "$cap" -lt 15 ] && fc=41
                    chg_big 3 "$fc" $(echo "$cap" | sed 's/./& /g') %
                    chg_battery 15 "$cap" "$fc"
                else
                    chg_big 3 47 "?" ; chg_battery 15 0 42
                fi
                chg_center 24 "$stxt" 1
                chg_center 26 "$detail"
                chg_center 29 "$msg" 33
                chg_center 32 "Hold Esc/On to start   |   Unplug to switch off" 2
            fi
        fi

        sleep $CHG_POLL
    done
}
