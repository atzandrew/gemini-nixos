#!/bin/bash
# cpu-clocks.sh — READ-ONLY survey of the Gemini's CPU clocks and voltages
# (docs/cpu-dvfs.md, step 1). Run ON THE GEMINI as root:
#
#   scp bin/cpu-clocks.sh bin/cpumhz/cpumhz <probe>.ko atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 sudo bash /tmp/cpu-clocks.sh /tmp/mt6797-dvfs-probe.ko
#   scp atzero@192.168.0.139:cpu-clocks-*.txt logs/
#
# Run it after gemini-a72-up has fired (~5 min after boot, CPUs 0-9
# online), so the A72s are measured too.
#
# What it does (nothing here changes a clock, a voltage or a register):
#   1. eFuse speed-bin words from LK's devinfo blob in the DT
#      (/proc/device-tree/chosen/atag,devinfo) -> vendor CPU level + table.
#   2. DA9214 VPROC bucks over I2C (i2c6 @0x1100e000, addr 0x68): READS
#      only (i2cget). Each register is read 3x because reads on this bus
#      were seen to be unreliable (cl2-up.sh notes).
#   3. Measured clock of every online CPU (cpumhz: dependent-add chain).
#   4. The read-only kernel probe in three separate loads, each logged and
#      synced to disk BEFORE the next one starts, so a hang still leaves the
#      earlier results on disk:
#        A: plain register reads (CSPM state, CPULDO VSRAM_L, DCM),
#        B: mcu=1   ARM PLL block under the vendor HW-semaphore protocol,
#        C: smc_b=1 A72 PLL via the vendor secure-read SMC (only if cpu8/9 up).
#      Skip B/C with STAGES=A (env). If the device hangs: hold power
#      ~10 s, boot, and send the partial report (it is in $HOME, not /tmp).
#
# Output: ~atzero/cpu-clocks-YYYYMMDD-HHMMSS.txt (also printed).
set -u
KO="${1:-}"
STAGES="${STAGES:-ABC}"
USER_HOME=$(getent passwd "${SUDO_USER:-atzero}" | cut -d: -f6)
OUT="$USER_HOME/cpu-clocks-$(date +%Y%m%d-%H%M%S).txt"
CPUMHZ="$(dirname "$0")/cpumhz"
[ -f "$CPUMHZ" ] || CPUMHZ="$(dirname "$0")/cpumhz/cpumhz"   # run from the repo

if [ "$(id -u)" != 0 ]; then echo "run as root (sudo bash $0 ...)"; exit 1; fi
: > "$OUT"; chown "${SUDO_USER:-atzero}" "$OUT" 2>/dev/null

say() { echo "$*" | tee -a "$OUT"; }
section() { say ""; say "=== $* ==="; sync; }

section "host"
say "date: $(date -Is)   uptime: $(cut -d' ' -f1 /proc/uptime) s"
say "kernel: $(uname -r) $(uname -v)"
say "online: $(cat /sys/devices/system/cpu/online)   offline: $(cat /sys/devices/system/cpu/offline)"
say "cpufreq dirs: $(ls /sys/devices/system/cpu/cpufreq 2>/dev/null | tr '\n' ' ')"
say "cpuidle driver: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null)"
say "thermal zones: $(ls /sys/class/thermal 2>/dev/null | tr '\n' ' ')"
for z in /sys/class/thermal/thermal_zone*; do
  [ -d "$z" ] && say "  ${z##*/}: type=$(cat $z/type 2>/dev/null) temp=$(cat $z/temp 2>/dev/null) of_node=$(cat $z/device/of_node/name 2>/dev/null)"
done
case "$(cat /sys/devices/system/cpu/online)" in
  0-9) ;;
  *) say "NOTE: A72s (cpu8/9) not online yet - gemini-a72-up fires ~5 min after boot; cpu8/9 and stage C will be skipped." ;;
esac
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  n=${c##*cpu}
  say "cpu$n: $(cat $c/of_node/compatible 2>/dev/null | tr '\0' ' ') midr=$(cat $c/regs/identification/midr_el1 2>/dev/null)"
done

section "eFuse speed bin (LK devinfo)"
DI=/proc/device-tree/chosen/atag,devinfo
if [ -r "$DI" ]; then
  # layout (vendor devinfo.c): u32 size, u32 tag, u32 data[]; little-endian
  word() { od -An -t u4 -j $(( 8 + 4 * $1 )) -N 4 "$DI" | tr -d ' '; }
  say "blob bytes: $(stat -c %s "$DI")  header size/tag: $(od -An -t x4 -N 8 "$DI")"
  W3=$(word 3); W22=$(word 22); W61=$(word 61)
  say "devinfo[3]  = $(printf 0x%08x "$W3")  (CPUFREQ_EFUSE_INDEX)"
  say "devinfo[22] = $(printf 0x%08x "$W22")  (FUNC_CODE: fc0 = bits27:24 = $(( (W22 >> 24) & 0xf )), fc1 = bits3:0 = $(( W22 & 0xf )))"
  say "devinfo[61] = $(printf 0x%08x "$W61")  (DATE_CODE bits7:4 = $(( (W61 >> 4) & 0xf )))"
  FC1=$(( W22 & 0xf )); DC=$(( (W61 >> 4) & 0xf ))
  case $FC1 in
    0|3) LV="0 (FY)";; 1|6) LV="1 (SB)";; 2|7) LV="2 (M)";; 15) LV="3 (L)";; 4) LV="1 (SB, TT segment)";; *) LV="0 (FY, default)";;
  esac
  if [ "$DC" -lt 7 ]; then DCS="1221"; else DCS="0119"; fi
  say "=> vendor CPU level $LV, A72 table date code $DCS (mt_cpufreq.c _mt_cpufreq_get_cpu_level / _get_cpu_date_code)"
  say "   (vendor also forces level 2 (M) if the DT A72 clock-frequency is 1989 MHz; ours says 2288)"
  say "raw words 0-63:"
  od -An -t x4 -j 8 -N 256 "$DI" | tee -a "$OUT"
else
  say "$DI not present - LK did not pass the devinfo blob"
fi
sync

section "DA9214 VPROC bucks (I2C reads only)"
bus() { # i2c adapter whose controller base is $1 (same as cl2-up.sh)
  local d h
  for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
    [ -f "$d" ] || continue
    h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
    [ "${h:8:8}" = "$1" ] && { echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
  done
  return 1
}
B=$(bus 1100e000)
if [ -z "$B" ] || ! command -v i2cget >/dev/null; then
  say "DA9214 bus (0x1100e000) not found or i2cget missing"
else
  say "bus i2c-$B, address 0x68"
  for r in 0x50 0x51 0x5d 0x5e 0xd0 0xd1 0xd2 0xd3 0xd5 0xd6 0xd7 0xd8 0xd9 0xda; do
    v1=$(i2cget -y "$B" 0x68 $r 2>&1); v2=$(i2cget -y "$B" 0x68 $r 2>&1); v3=$(i2cget -y "$B" 0x68 $r 2>&1)
    line="reg $r: $v1 $v2 $v3"
    case $r in
      0xd5|0xd6|0xd7|0xd8|0xd9|0xda)
        if [ "${v1#0x}" != "$v1" ]; then line="$line  -> $(( 300 + (v1 & 0x7f) * 10 )) mV (bit7=$(( (v1 >> 7) & 1 )))"; fi ;;
      0x5d|0x5e)
        if [ "${v1#0x}" != "$v1" ]; then line="$line  -> EN=$(( v1 & 1 )) VSEL(bit4: 0=A 1=B)=$(( (v1 >> 4) & 1 )) VSEL_GPI(bits6:5, 0=off)=$(( (v1 >> 5) & 3 ))"; fi ;;
    esac
    say "$line"
  done
  say "names (DA9213/14 map, mainline da9211-regulator.h): 0x5d BUCKA_CONT, 0x5e BUCKB_CONT, 0xd0 BUCK_ILIM,"
  say "  0xd1/0xd2 BUCKA/B_CONF, 0xd3 BUCK_CONF, 0xd5/0xd6 VBUCKA/B_MAX, 0xd7 VBUCKA_A (VPROC1 = LL+L+CCI),"
  say "  0xd8 VBUCKA_B, 0xd9 VBUCKB_A (VPROC2 = A72), 0xda VBUCKB_B. Vendor writes only 0xd7/0xd9 (300 mV + n*10 mV)."
fi
sync

section "measured clocks (cpumhz, dependent-add chain)"
if [ -f "$CPUMHZ" ]; then
  chmod +x "$CPUMHZ" 2>/dev/null
  "$CPUMHZ" | tee -a "$OUT"
else
  say "cpumhz not found next to this script ($CPUMHZ)"
fi
sync

probe_stage() { # $1 = stage name, rest = insmod params
  local name=$1; shift
  section "kernel probe stage $name ($*)"
  if insmod "$KO" "$@"; then
    sleep 0.2
    # this load's lines = everything from the last "read-only probe" header on
    dmesg | grep 'mt6797-dvfs' | sed 's/^\[[^]]*\] //' | \
      awk '/read-only probe/ { n = 0 } { buf[n++] = $0 } END { for (i = 0; i < n; i++) print buf[i] }' | tee -a "$OUT"
    sync
    rmmod mt6797_dvfs_probe
  else
    say "insmod failed"
  fi
  sync
}

if [ -n "$KO" ] && [ -r "$KO" ]; then
  say ""
  say "probe module: $KO ($(modinfo -F vermagic "$KO" 2>/dev/null))"
  case "$STAGES" in *A*) probe_stage A ;; esac
  case "$STAGES" in *B*) probe_stage B mcu=1 ;; esac
  case "$STAGES" in *C*)
    if [ -d /sys/devices/system/cpu/cpu8 ] && [ "$(cat /sys/devices/system/cpu/cpu8/online 2>/dev/null)" = 1 ]; then
      probe_stage C mcu=1 smc_b=1
    else
      say ""; say "stage C skipped: cpu8 offline"
    fi ;;
  esac
else
  say ""; say "no probe module given (arg 1) - kernel register stages skipped"
fi

section "done"
say "report: $OUT"
sync
