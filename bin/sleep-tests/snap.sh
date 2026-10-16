#!/bin/sh
# snap.sh OUT: read-only register snapshot (no BTIF RBR/LSR/IIR reads)
O=$1; : > $O
d() { echo "$1 $(busybox devmem $1 32)" >> $O; }
r() { a=$1; e=$2; while [ $((a)) -le $((e)) ]; do d $(printf 0x%08x $((a))); a=$((a+4)); done; }
r 0x10006000 0x100066fc
d 0x10007018; d 0x10001f00
for o in 0x04 0x0c 0x48 0x4c 0x60 0x6c; do d $(printf 0x%08x $((0x1100c000+o))); done
for b in 0x11000a00 0x11000a80; do for o in 0x04 0x08 0x1c 0x24 0x28 0x38 0x3c 0x40 0x54; do d $(printf 0x%08x $((b+o))); done; done
r 0x18070000 0x180700fc; d 0x18070110; d 0x18070160
for m in dummy-infracfg_ao@0x0000000010001000 dummy-infracfg@0x0000000010201000 dummy-topckgen@0x0000000010000000 1000d000.pwrap; do
  sed "s/^/$m:/" /sys/kernel/debug/regmap/$m/registers >> $O 2>/dev/null; done
echo "snap $O $(wc -l < $O) lines"
