#!/usr/bin/env bash
# emmc-pads.sh — READ-ONLY dump of the MSDC0 (eMMC) pad configuration and
# the MSDC clock rates. Run ON THE GEMINI. Writes nothing to hardware.
#
# Why (2026-10-04): HS200 at 192 MHz failed under load. pinctrl-mt6797 has
# no pinconf, so the MSDC0 pads keep whatever LK set. The vendor 3.18 kernel
# sets (IOCFG_B = 0x10002400; drivers/mmc/host/mediatek/mt6797/msdc_io.[ch],
# aeon6797_6m_n.dts mmc0_pins_default):
#   DRV   +0x1a0 [11:0] = 0x249 (drive 1 on clk, cmd/dsl/rstb, dat7-4, dat3-0)
#   SMT   +0x060 [3:0]  = 0xf
#   TDSEL +0x0d0 [15:0] = 0xcccc
#   RDSEL +0x080 [17:0] = 0
# This shows what the running system has, to compare. (The mt6797 driver
# build also logs these at probe: dmesg | grep 'mt6797 pads'.)
#
# Usage (from Hydra):
#   scp bin/emmc-pads.sh atzero@192.168.0.139:/tmp/
#   ssh -t atzero@192.168.0.139 bash /tmp/emmc-pads.sh
#
# Needs python3 on the device (mmap of /dev/mem; a plain read() of /dev/mem
# is not safe for MMIO on arm64). Only the IOCFG_B pad block is read: it is
# always clocked. MSDC controller registers are NOT read here (runtime PM
# may gate their clock, and a read then can hang the bus).
set -eu
command -v python3 >/dev/null || { echo "python3 not found on the device"; exit 1; }

echo "== MSDC clocks (clk_summary)"
sudo grep -E 'msdc|univpll1_d4|syspll2_d2' /sys/kernel/debug/clk/clk_summary || :

echo "== MSDC0 pads (IOCFG_B 0x10002400)"
sudo python3 - <<'EOF'
import mmap, os, struct
BASE = 0x10002400
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=BASE & ~0xfff)
off0 = BASE & 0xfff
def rd(o):
    return struct.unpack("<I", m[off0 + o:off0 + o + 4])[0]
regs = [("IES", 0x020, 0xfff, None), ("SR", 0x030, 0xffffffff, None),
        ("SMT", 0x060, 0xf, 0xf), ("RDSEL", 0x080, 0x3ffff, 0x0),
        ("TDSEL", 0x0d0, 0xffff, 0xcccc), ("PUPD", 0x100, 0xffffffff, None),
        ("R0", 0x110, 0xffffffff, None), ("R1", 0x120, 0xffffffff, None),
        ("DRV", 0x1a0, 0xfff, 0x249)]
for name, o, mask, vendor in regs:
    v = rd(o)
    line = "%-6s +0x%03x = 0x%08x  field=0x%x" % (name, o, v, v & mask)
    if vendor is not None:
        line += "  vendor=0x%x %s" % (vendor, "same" if (v & mask) == vendor else "DIFFERENT")
    print(line)
d = rd(0x1a0)
print("DRV per group: dat3-0=%d dat7-4=%d cmd/dsl/rstb=%d clk=%d" %
      (d & 7, (d >> 3) & 7, (d >> 6) & 7, (d >> 9) & 7))
m.close(); os.close(fd)
EOF
