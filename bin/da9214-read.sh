#!/bin/bash
# da9214-read.sh — find a RELIABLE way to read the DA9214 VPROC bucks
# (docs/cpu-dvfs.md, step 1b). Run ON THE GEMINI as root, with the A72s up:
#
#   scp bin/da9214-read.sh atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 sudo bash /tmp/da9214-read.sh
#   scp 'atzero@192.168.0.139:da9214-read-*.txt' logs/
#
# Why: in cpu-clocks.sh, `i2cget` (SMBus read-byte-data = write register
# address + repeated START + read) failed on ~1/3 of attempts and returned
# 0x00 otherwise — including BUCKB_CONT (0x5e) bit0 = 0 while the A72s
# were running on that buck, so the 0x00s are wrong too.
#
# Ground truth: with cpu8/9 online, 0x5e bit0 (BUCKB_EN) MUST read 1
# (cl2-up.sh sets it). A method that reads 0x5e as 0x?1/0x?3... and gives
# stable values elsewhere is trustworthy.
#
# Nothing here writes a DA9214 register. Method C sends a 1-byte "send
# byte" that only sets the chip's register-address pointer (no data byte),
# which is how a plain-I2C register read starts anyway.
#
# Methods, 10 reads each of 0x5e (BUCKB_CONT), 0x5d (BUCKA_CONT),
# 0xd7 (VBUCKA_A = VPROC1), 0xd9 (VBUCKB_A = VPROC2):
#   A  i2cget byte-data            (what cpu-clocks.sh did)
#   B  i2ctransfer w1 + r1         (one combined transfer, repeated START)
#   C  i2cset send-byte, then i2cget receive-byte (two separate transfers, STOP between)
set -u
[ "$(id -u)" = 0 ] || { echo "run as root"; exit 1; }
USER_HOME=$(getent passwd "${SUDO_USER:-atzero}" | cut -d: -f6)
OUT="$USER_HOME/da9214-read-$(date +%Y%m%d-%H%M%S).txt"
: > "$OUT"; chown "${SUDO_USER:-atzero}" "$OUT" 2>/dev/null
say() { echo "$*" | tee -a "$OUT"; }

bus() {
  local d h
  for d in /sys/class/i2c-adapter/i2c-*/of_node/reg; do
    [ -f "$d" ] || continue
    h=$(od -An -N8 -tx1 "$d" 2>/dev/null | tr -d ' \n')
    [ "${h:8:8}" = "$1" ] && { echo "$d" | sed -n 's#.*i2c-\([0-9]*\)/.*#\1#p'; return 0; }
  done
  return 1
}
B=$(bus 1100e000) || { say "DA9214 bus not found"; exit 1; }
A=/sys/class/i2c-adapter/i2c-$B
say "date $(date -Is)  online cpus $(cat /sys/devices/system/cpu/online)"
say "bus i2c-$B: name=$(cat $A/name)  dt clock-frequency=$(od -An -t u4 --endian=big $A/of_node/clock-frequency 2>/dev/null | tr -d ' ')"
say "i2c-tools: $(i2cget -V 2>&1 | head -1)"
say "kernel i2c messages since boot:"
dmesg | grep -iE 'i2c.*(1100e000|i2c-'"$B"'|timeout|arbitration|ack)' | tail -10 | tee -a "$OUT"

REGS="0x5e 0x5d 0xd7 0xd9"
for r in $REGS; do
  say ""
  say "--- reg $r ---"
  line="A byte-data:"
  for i in $(seq 10); do line="$line $(i2cget -y "$B" 0x68 $r b 2>&1 | sed 's/Error: Read failed/FAIL/')"; done
  say "$line"
  line="B w1+r1 (rep. START):"
  for i in $(seq 10); do line="$line $(i2ctransfer -y "$B" w1@0x68 $r r1 2>&1 | sed 's/Error: Sending messages failed.*/FAIL/')"; done
  say "$line"
  line="C send-byte then receive-byte:"
  for i in $(seq 10); do
    if i2cset -y "$B" 0x68 $r 2>/dev/null; then
      line="$line $(i2cget -y "$B" 0x68 2>&1 | sed 's/Error: Read failed/FAIL/')"
    else
      line="$line WFAIL"
    fi
  done
  say "$line"
done

say ""
say "Decode: VBUCKx = 300 mV + (value & 0x7f) x 10 mV. 0x5e bit0 must be 1 with cpu8/9 online."
say "report: $OUT"
sync
