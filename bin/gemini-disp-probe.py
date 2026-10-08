#!/usr/bin/env python3
"""
gemini-disp-probe.py - READ-ONLY snapshot of the display pipeline LK left
running on the Gemini PDA (MT6797), for the tearing / page-flip work.

What/why: geminipda-drm never touches the display hardware; it blits into
LK's scanout buffer while RDMA0 scans it out, with a software vblank that is
not phase-locked to the real scan -> tearing. Before any driver change we
need facts: which OVL layer scans out the LK buffer, its pitch/format/alpha
bits, how much vram LK reserved (room for a second buffer?), the DSI mode
(video vs command), the real refresh rate, and the vblank length in lines.

Safety: mmap(/dev/mem, PROT_READ) of the OVL0 / OVL0_2L / OVL1_2L / RDMA0 /
DSI0 / MUTEX register pages, 32-bit aligned loads only. No register is
written, nothing is enabled or disabled. The MM power domain and its clocks
are kept on by geminipda-drm/-fb + clk_ignore_unused, so the reads hit
clocked hardware. (ixoo saw DEVAPC violations + all-zero reads on the stock
3.18 kernel while its DDP was powered off - if every value here reads 0,
STOP and report; do not retry in a loop.)

Usage (on the Gemini):  sudo python3 gemini-disp-probe.py [seconds]
   seconds = how long to sample the RDMA0 line counter (default 1.0)
"""
import mmap
import os
import struct
import sys
import time

BLOCKS = {
    "OVL0":    0x1400b000,
    "OVL0_2L": 0x1400d000,
    "OVL1_2L": 0x1400e000,
    "RDMA0":   0x1400f000,
    "DSI0":    0x1401c000,
    "MUTEX":   0x1401f000,
}

maps = {}


def rd(block, off):
    m = maps[block]
    return m[off // 4]


def open_maps():
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    for name, base in BLOCKS.items():
        mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ, offset=base)
        maps[name] = memoryview(mm).cast("I")   # aligned 32-bit loads
    os.close(fd)


def atag():
    p = "/proc/device-tree/chosen/atag,videolfb"
    try:
        b = open(p, "rb").read()
    except OSError as e:
        print(f"atag,videolfb: {e}")
        return None
    base, found, fps, vram = struct.unpack_from("<QIII", b, 0)
    name = b[20:].split(b"\0")[0].decode(errors="replace")
    frame = 1088 * 4 * 2160
    print(f"atag,videolfb: fb_base=0x{base:x} vram=0x{vram:x} ({vram/2**20:.1f} MiB)"
          f" = {vram/frame:.2f} frames of 1088x2160x4; fps field={fps}"
          f" lcm_found={found} lcm='{name}'")
    return base, vram


def ovl(block, nlayers, fb):
    sta, inten, en = rd(block, 0x000), rd(block, 0x004), rd(block, 0x00c)
    roi, src_con = rd(block, 0x020), rd(block, 0x02c)
    print(f"{block}: STA=0x{sta:08x} INTEN=0x{inten:08x} EN=0x{en:08x}"
          f" ROI={roi & 0xffff}x{roi >> 16} SRC_CON=0x{src_con:08x}")
    for n in range(nlayers):
        con = rd(block, 0x030 + 0x20 * n)
        size = rd(block, 0x038 + 0x20 * n)
        off = rd(block, 0x03c + 0x20 * n)
        pitch = rd(block, 0x044 + 0x20 * n)
        addr = rd(block, 0xf40 + 0x20 * n)
        on = (src_con >> n) & 1
        hit = ""
        if fb and addr:
            base, vram = fb
            if base <= addr < base + vram:
                hit = f"  <-- LK fb +0x{addr - base:x}"
        print(f"  L{n}: en={on} CON=0x{con:08x} (fmt={(con >> 12) & 0xf}"
              f" aen={(con >> 8) & 1} byteswap={(con >> 24) & 1})"
              f" SRC={size & 0xffff}x{size >> 16} OFFSET=0x{off:08x}"
              f" PITCH={pitch & 0xffff} ADDR=0x{addr:08x}{hit}")


def rdma(fb):
    regs = {n: rd("RDMA0", o) for n, o in (
        ("INT_EN", 0x000), ("INT_STA", 0x004), ("GLOBAL_CON", 0x010),
        ("SIZE0", 0x014), ("SIZE1", 0x018), ("TARGET_LINE", 0x01c),
        ("MEM_CON", 0x024), ("MEM_PITCH", 0x02c), ("MEM_START", 0xf00),
        ("IN_LINE", 0x0f4), ("OUT_LINE", 0x0fc))}
    mode = "memory (reads DRAM itself)" if regs["GLOBAL_CON"] & 2 else "direct-link (fed by OVL)"
    print("RDMA0: " + " ".join(f"{k}=0x{v:08x}" for k, v in regs.items()))
    print(f"  mode={mode}  width={regs['SIZE0'] & 0x1fff} height={regs['SIZE1'] & 0xfffff}")


def dsi():
    names = (("START", 0x00), ("STA", 0x04), ("INTEN", 0x08), ("INTSTA", 0x0c),
             ("COM_CTRL", 0x10), ("MODE_CTRL", 0x14), ("TXRX_CTRL", 0x18),
             ("PSCTRL", 0x1c), ("VSA_NL", 0x20), ("VBP_NL", 0x24),
             ("VFP_NL", 0x28), ("VACT_NL", 0x2c), ("HSA_WC", 0x50),
             ("HBP_WC", 0x54), ("HFP_WC", 0x58))
    v = {n: rd("DSI0", o) for n, o in names}
    print("DSI0: " + " ".join(f"{k}=0x{x:08x}" for k, x in v.items()))
    mode = {0: "COMMAND", 1: "VIDEO sync-pulse", 2: "VIDEO sync-event",
            3: "VIDEO burst"}[v["MODE_CTRL"] & 3]
    print(f"  mode={mode}  vsa/vbp/vfp/vact = {v['VSA_NL']}/{v['VBP_NL']}/"
          f"{v['VFP_NL']}/{v['VACT_NL']} lines")


def mutex():
    print(f"MUTEX: INTEN=0x{rd('MUTEX', 0x0):08x} INTSTA=0x{rd('MUTEX', 0x4):08x}")
    for n in range(4):
        en, mod, sof = (rd("MUTEX", 0x20 + 0x20 * n + o) for o in (0x0, 0xc, 0x10))
        if en or mod:
            print(f"  mutex{n}: EN=0x{en:x} MOD=0x{mod:08x} SOF=0x{sof:x}")


def scan(seconds):
    """Sample RDMA0 OUT_LINE_CNT: frame period, line range, vblank length."""
    t_end = time.monotonic() + seconds
    last = rd("RDMA0", 0x0fc) & 0xffff
    lo, hi, wraps, samples = last, last, [], 0
    while time.monotonic() < t_end:
        cur = rd("RDMA0", 0x0fc) & 0xffff
        samples += 1
        lo, hi = min(lo, cur), max(hi, cur)
        if cur < last - 100:          # counter restarted = new frame
            wraps.append(time.monotonic_ns())
        last = cur
    print(f"RDMA0 OUT_LINE_CNT over {seconds:.1f}s: {samples} samples,"
          f" range {lo}..{hi}, {len(wraps)} frame starts seen")
    if len(wraps) > 2:
        d = [(b - a) / 1e6 for a, b in zip(wraps, wraps[1:])]
        d.sort()
        med = d[len(d) // 2]
        print(f"  frame period median {med:.3f} ms = {1000 / med:.2f} Hz"
              f" (min {d[0]:.3f}, max {d[-1]:.3f})")


def irqs():
    try:
        for line in open("/proc/interrupts"):
            if any(s in line for s in ("217", "229", "202", "213", "disp", "rdma", "dsi")):
                print("  irq: " + " ".join(line.split()[:3] + line.split()[-3:]))
    except OSError:
        pass


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    if os.geteuid() != 0:
        sys.exit("run with sudo (needs /dev/mem)")
    print(f"kernel {os.uname().release}  {time.strftime('%Y-%m-%d %H:%M:%S')}")
    fb = atag()
    open_maps()
    if all(rd(b, 0) == 0 and rd(b, 4) == 0 for b in ("OVL0", "RDMA0", "DSI0")):
        print("WARNING: OVL0/RDMA0/DSI0 all read 0 - block unclocked or DEVAPC"
              " rejected the reads. Check `dmesg | tail` and stop here.")
    ovl("OVL0", 4, fb)
    ovl("OVL0_2L", 2, fb)
    ovl("OVL1_2L", 2, fb)
    rdma(fb)
    dsi()
    mutex()
    scan(seconds)
    irqs()
    for p in ("stat_copy_us_last", "stat_copy_us_max", "refresh_hz", "cache_sync"):
        try:
            v = open(f"/sys/module/geminipda_drm/parameters/{p}").read().strip()
            print(f"geminipda_drm.{p} = {v}")
        except OSError:
            pass


if __name__ == "__main__":
    main()
