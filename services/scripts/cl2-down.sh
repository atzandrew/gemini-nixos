#!/bin/bash
# !!! SUPERSEDED 2026-10-06 — DO NOT RUN on a kernel with mt6797-cl2-power.
# The kernel now powers the A72 cluster itself (DA9214 under da9211-regulator
# + drivers/soc/mediatek/mt6797-cl2-power.c, vendor order; docs/cpu-dvfs.md).
# This script switched the A72 rail on outside the PMIC-wrapper SPI reset
# latch and held the latch for tens of ms (the old boot-time wedge), its WDT
# "arm" was 20/64 s and its "disarm" turned the watchdog off. It also writes
# the DA9214 over i2c-dev behind the regulator driver's back (page register!).
# Kept for reference only; no longer shipped in the Debian image.
# cl2-down.sh — power DOWN the A72 cluster (cpu8/cpu9) on the gemini 6.6
# kernel, returning the box to its cold-boot power state: cluster off,
# B_EXT_BUCK_ISO re-asserted (0x10006290 bit1), MP2_CPUSYS_PWR_CON bit0
# clear (0x10006218), external DA9214 BUCKB rail off. Run ON THE DEVICE.
#
# The reverse of cl2-up.sh (same dir). cl2-up.sh brings the COLD cluster up
# with a Linux-side pre-sequence; cl2-down.sh is the power-saving path.
#
# What actually happens on offline (receipts: session-log 2026-09-07 A72-down
# session; vendor 3.18 arch/arm64/kernel/psci.c cpu_power_off_buck; secure-
# firmware attribution in the sibling mainline project's
# 2026-08-05-a72-secure-cpu-off-attribution effect-inventory.tsv):
#   - Per-core CPU_OFF (cpu9 while cpu8 is up) is SAFE: the secure world
#     waits for the core's WFI, clears only its per-core power-control words
#     and its big_on ledger bit; the cluster branch is not entered. Proven
#     live 2026-09-07 ("psci: CPU9 killed (polled 0 ms)").
#   - The LAST A72's teardown is NOT symmetric with the up path: it runs
#     inside the controlling CPU's AFFINITY_INFO SMC (the secure world's
#     power_off_big/power_off_cl3): CCI snoop withdrawal, B mux/PLL control,
#     SPM MP2 cluster power-down, then RMW-SET B_EXT_BUCK_ISO and clear of
#     0x10006218 bit0. It has EIGHT+ UNBOUNDED wait sites: a stall = cpu0
#     stuck in the secure world = whole-box freeze, so the MTK WDT (armed
#     below, 20 s) is the ONLY recovery (EXRST). Nobody had run this branch
#     on mainline before 2026-09-07; stock Android 3.18 did it routinely.
#   - The secure world does NOT drop the external DA9214 BUCKB rail (no
#     direct i2c access); that is Linux's post-teardown job (vendor
#     cpu_power_off_buck = i2cset 0x5E bit0 = 0). Do it ONLY after the
#     teardown has re-asserted ISO (0x10006290 bit1).
#
# Usage: bash cl2-down.sh [cpu9|cpu8|both]
#   cpu9   per-core off only (safe; cpu8 stays up)
#   cpu8   last-A72 off — REQUIRES cpu9 already offline, else refuses
#   both   (default) cpu9 then cpu8 = FULL cluster power-down
#
# Re-enable: bash /root/cl2-up.sh   (cold-cluster up path — the state after
# a clean down is byte-identical to the cold-boot state cl2-up was built for).
set -u
TARGET="${1:-both}"
WDT_SECS=20

bus() { # i2c adapter whose controller base is $1 (hex, no 0x)
  local d h
  for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
    [ -f "$d" ] || continue
    h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
    [ "${h:8:8}" = "$1" ] && { echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
  done
  return 1
}

wdt_arm()   { busybox devmem 0x10007004 32 $(( (WDT_SECS<<5)|8 )); log "WDT armed (${WDT_SECS}s)"; }
# key-protected MODE write (MTK_WDT_MODE_KEY 0x22000000) — a plain 0 is ignored
wdt_disarm(){ busybox devmem 0x10007000 32 0x22000000; log "WDT disarmed"; }

log() { echo "cl2-down: $*"; logger -t cl2-down "$*"; }

iso_set() { [ $(( $(busybox devmem 0x10006290 32) & 0x2 )) -eq 2 ]; }
pwrcon_bit0_clear() { [ $(( $(busybox devmem 0x10006218 32) & 0x1 )) -eq 0 ]; }

offline_one() { # $1=cpu — WDT-armed PSCI offline; returns 0 iff it left the map
  local cpu=$1
  wdt_arm
  log "[cpu$cpu] echo 0 > cpu$cpu/online (WDT armed; hang => EXRST in ${WDT_SECS}s)"
  echo 0 > /sys/devices/system/cpu/cpu$cpu/online 2>/dev/null
  local rc=$?
  if ! grep -qw "$cpu" /sys/devices/system/cpu/online; then
    log "[cpu$cpu] OFFLINE rc=$rc online=$(cat /sys/devices/system/cpu/online)"
    wdt_disarm
    return 0
  fi
  log "[cpu$cpu] FAILED rc=$rc (still online) — aborting down; WDT disarmed"
  wdt_disarm
  return 1
}

buckb_off() { # DA9214 BUCKB (A72 rail) disable: reg 0x5E bit0 = 0. READS on
  local B i ok=0      # this bus are unreliable (SCP/DVFSP shares i2c6 —
  B=$(bus 1100e000)   # even/odd-ACK garbage); i2cset ACK is the trusted channel.
  [ -n "$B" ] || { log "DA9214 bus (0x1100e000) not probed — rail left ON"; return 1; }
  for i in 1 2 3 4 5 6 7 8 9 10; do
    if i2cset -y "$B" 0x68 0x5e 0x00 2>/dev/null; then ok=1; break; fi
    sleep 0.2
  done
  if [ "$ok" = 1 ]; then log "BUCKB (A72 rail) DISABLED (i2c-$B 0x68 reg 0x5e=0)"; return 0; fi
  log "BUCKB write never ACKed — bus busy; rail left ON (power saving = cluster only)"
  return 1
}

down_cpu9() { offline_one 9; }

down_cpu8() { # the LAST-A72 branch — see header for the risk
  local cpu=8
  if grep -qw 9 /sys/devices/system/cpu/online; then
    log "REFUSING cpu8 offline: cpu9 still online. Run 'bash cl2-down.sh both' or offline cpu9 first."
    return 1
  fi
  log "[cpu$cpu] LAST-A72 offline — secure power_off_cl3 (CCI/snoop/SPM/ISO) runs in the affinity SMC;"
  log "[cpu$cpu] UNBOUNDED secure waits; WDT ${WDT_SECS}s is the only recovery if it stalls."
  offline_one $cpu || return 1
  # teardown completed: the secure path RMW-set B_EXT_BUCK_ISO + cleared 0x218 bit0
  log "[cpu$cpu] post-offline state: 0x10006290(ISO)=$(busybox devmem 0x10006290 32) 0x10006218(PWR_CON)=$(busybox devmem 0x10006218 32)"
  if iso_set && pwrcon_bit0_clear; then
    log "cluster ISOLATED from rail (ISO bit1 set, PWR_CON bit0 clear) — dropping the DA9214 rail"
    buckb_off
  else
    log "WARNING: ISO/PWR_CON not in the expected off state — rail LEFT ON (cluster off only)"
  fi
  return 0
}

log "== cl2-down: target=$TARGET online=$(cat /sys/devices/system/cpu/online)"
case "$TARGET" in
  cpu9) down_cpu9 ;;
  cpu8) down_cpu8 ;;
  both) down_cpu9 && down_cpu8 ;;
  *)    echo "usage: bash cl2-down.sh [cpu9|cpu8|both]"; exit 2 ;;
esac
rc=$?
log "final online=$(cat /sys/devices/system/cpu/online)   (re-enable: bash /root/cl2-up.sh)"
exit $rc
