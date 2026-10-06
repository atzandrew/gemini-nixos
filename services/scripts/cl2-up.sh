#!/bin/bash
# !!! SUPERSEDED 2026-10-06 — DO NOT RUN on a kernel with mt6797-cl2-power.
# The kernel now powers the A72 cluster itself (DA9214 under da9211-regulator
# + drivers/soc/mediatek/mt6797-cl2-power.c, vendor order; docs/cpu-dvfs.md).
# This script switched the A72 rail on outside the PMIC-wrapper SPI reset
# latch and held the latch for tens of ms (the old boot-time wedge), its WDT
# "arm" was 20/64 s and its "disarm" turned the watchdog off. It also writes
# the DA9214 over i2c-dev behind the regulator driver's back (page register!).
# Kept for reference only; no longer shipped in the Debian image.
# cl2-up.sh — bring the A72 cluster (cpu8/cpu9) online on the gemini 6.6
# kernel. Run ON THE DEVICE (a72-up.service ExecStart, or by hand via
# build/device-ssh.sh 'bash /root/cl2-up.sh').
#
# Sequence (BSP receipt: gemini-linux-kernel-3.18 arch/arm64/kernel/psci.c
# cpu_power_on_buck under CONFIG_CL2_BUCK_CTRL, validated 2026-09-04 on
# hardware): plain PSCI CPU_ON of a COLD A72 cluster hangs the secure world
# (whole box freezes); with this pre-sequence CPU_ON returns and the core
# boots:
#   1. DA9214 BUCKB enable (VPROC2/A72 rail): reg 0x5E bit0 @ 0x68 on the
#      i2c6-hw bus (0x1100e000). The chip's READS are unreliable (SCP/DVFSP
#      shares the bus — see da921x-i2c6-a72.md); i2cset success (ACK) is
#      taken as landed.
#   2. SPM 0x10006218 bit0; 3. SWSYSRST latch (0x10007018 |= 0x88000800);
#   4. SPM 0x10006290 &= ~3 (clear EXT_BUCK_ISO); 5. SRAM-LDO SMC
#      0xC20003BF arg 110000 via /dev/idvfs-sramldo; 6. PSCI CPU_ON.
#
# Safety (2026-09-04 10th — the unguarded service run wedged the box when
# the DA9214 bus was contended ~2 min after boot): the MTK WDT (15 s,
# 0x10007004 = s<<5|0x8) is armed right before CPU_ON and disarmed after,
# so ANY freeze self-recovers via EXRST; the whole attempt retries with
# backoff so a transiently-busy DA9214 bus doesn't fail the boot.
#
# cpu9 then comes up with a plain warm hotplug (cluster already on).
set -u
TARGET="${1:-both}"
WDT_SECS=15

bus() { # i2c adapter whose controller base is $1 (hex, no 0x)
  local d h
  for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
    [ -f "$d" ] || continue
    h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
    [ "${h:8:8}" = "$1" ] && { echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
  done
  return 1
}

wdt_arm() { busybox devmem 0x10007004 32 $(( (WDT_SECS<<5)|8 )); }
# [corrected 2026-09-04] the MODE register is key-protected: a plain 0 write
# is IGNORED and the armed WDT fires later (~2 s/unit) — must OR the key
# (MTK_WDT_MODE_KEY 0x22000000, BSP mt_wdt.h) with enable bit clear.
wdt_disarm() { busybox devmem 0x10007000 32 $(( 0x22000000 )); }

log() { echo "cl2-up: $*"; logger -t cl2-up "$*"; }

up_cold() { # $1=cpu — full cold-cluster sequence, retried with backoff
  local cpu=$1 attempt i ok B
  for attempt in 1 2 3 4 5 6; do
    # Re-resolve the DA9214 bus EVERY attempt: the i2c6 controller can probe
    # LATE (deferred probe), so a bus computed once at script start is empty
    # when a72-up.service runs ~1 min after boot (observed 2026-09-07 on #329:
    # service run = "bus i2c-" all 6 attempts; the same script by hand 2 min
    # later found i2c-2 and brought cpu8/9 up on attempt 1).
    B=$(bus 1100e000)
    log "[cpu$cpu] attempt $attempt (bus i2c-$B)"
    ok=0
    if [ -n "$B" ]; then
      for i in 1 2 3 4 5 6 7 8 9 10; do
        if i2cset -y "$B" 0x68 0x5e 0x01 2>/dev/null; then ok=1; break; fi
        sleep 0.2
      done
    fi
    if [ "$ok" != 1 ]; then
      if [ -z "$B" ]; then
        log "[cpu$cpu] DA9214 i2c adapter (0x1100e000) not probed yet — backing off 20s"
      else
        log "[cpu$cpu] BUCKB write never ACKed — bus busy (SCP?), backing off 20s"
      fi
      sleep 20; continue
    fi
    busybox devmem 0x10006218 32 $(( $(busybox devmem 0x10006218 32) | 1 ))
    local sr; sr=$(busybox devmem 0x10007018 32)
    busybox devmem 0x10007018 32 $(( (sr | 0x88000800) ))
    busybox devmem 0x10006290 32 $(( $(busybox devmem 0x10006290 32) & ~3 ))
    sr=$(busybox devmem 0x10007018 32)
    busybox devmem 0x10007018 32 $(( (sr | 0x88000000) & ~0x800 ))
    if [ -e /dev/idvfs-sramldo ]; then
      echo 110000 > /dev/idvfs-sramldo 2>/dev/null
    else
      log "[cpu$cpu] /dev/idvfs-sramldo missing — insmod sramldo-smc.ko"
    fi
    sleep 0.1
    log "[cpu$cpu] armed WDT ${WDT_SECS}s, PSCI CPU_ON"
    wdt_arm
    if echo 1 > /sys/devices/system/cpu/cpu$cpu/online 2>/dev/null; then
      wdt_disarm
      log "[cpu$cpu] ONLINE — $(cat /sys/devices/system/cpu/online)"
      return 0
    fi
    wdt_disarm
    log "[cpu$cpu] hotplug returned nonzero — retrying"
    sleep 10
  done
  log "[cpu$cpu] GAVE UP after 6 attempts"
  return 1
}

up_warm() { echo 1 > /sys/devices/system/cpu/cpu$1/online 2>/dev/null; log "cpu$1 warm rc=$? online=$(cat /sys/devices/system/cpu/online)"; }

case "$TARGET" in
  cpu8) up_cold 8 ;;
  cpu9) up_warm 9 ;;
  *)    up_cold 8 && up_warm 9 ;;
esac
log "final online=$(cat /sys/devices/system/cpu/online)"
