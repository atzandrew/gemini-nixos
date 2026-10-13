#!/usr/bin/env python3
"""
scanout-idle-test.py - how much of the idle drain is the display path? No build.

Before writing zero-copy scanout for idle power, measure what it could save:

  1. Copy duty at idle: geminipda-drm's stat_commits / stat_copy_us_total over
     60 s on a static desktop. Zero-copy only removes this CPU copy, so if it
     is ~0 at idle, zero-copy is a performance fix, not a battery fix.
  2. OVL0 layer 3 (LK's extra layer, page 1 of LK's vram): is it fully
     transparent? Read-only scan of its alpha bytes.
  3. OVL0 L3 on/off A/B: L3 makes OVL0 fetch a second full frame from DRAM
     at 59 Hz (~560 MB/s), screen on or blanked. Alternates L3 on/off
     ROUNDS times (SECS each) so the gauge's run-to-run drift (+-40 mA)
     cancels out.

Safety: the only write is OVL0 SRC_CON bit 3 (layer 3 enable). The original
value is restored at the end, on Ctrl-C and on SIGTERM; a reboot also
restores it (LK sets it up). If L3 is NOT fully transparent the A/B step is
skipped unless FORCE=1 (you'd see something vanish).

Usage (on the Gemini, on battery, desktop idle, don't touch it):
    sudo python3 scanout-idle-test.py            # ~10 min; screen may blank, fine
    SECS=60 ROUNDS=4 sudo -E python3 scanout-idle-test.py
"""
import mmap
import os
import signal
import struct
import sys
import time

OVL0 = 0x1400b000
SRC_CON = 0x02c
L3_BIT = 1 << 3
PS = "/sys/class/power_supply/mt6351-battery"
DRM = "/sys/module/geminipda_drm/parameters"
SECS = int(os.environ.get("SECS", "60"))
ROUNDS = int(os.environ.get("ROUNDS", "4"))
FORCE = os.environ.get("FORCE") == "1"

regs = None
orig_src = None


def rd(off):
    return regs[off // 4]


def wr(off, val):
    regs[off // 4] = val


def restore(*_):
    if regs is not None and orig_src is not None:
        wr(SRC_CON, orig_src)
        print(f"restored SRC_CON=0x{orig_src:08x}", flush=True)
    if _:
        sys.exit(1)


def sysrd(p):
    with open(p) as f:
        return f.read().strip()


def avg_ma(seconds):
    s = n = 0
    for _ in range(seconds):
        s += abs(int(sysrd(f"{PS}/current_now")))   # sign convention varies; drain = magnitude
        n += 1
        time.sleep(1)
    return s / n / 1000


def copy_duty(seconds=60):
    try:
        c0, t0 = int(sysrd(f"{DRM}/stat_commits")), int(sysrd(f"{DRM}/stat_copy_us_total"))
    except OSError:
        print("1. copy duty: no stat_commits/stat_copy_us_total parameters, skipped")
        return
    time.sleep(seconds)
    c1, t1 = int(sysrd(f"{DRM}/stat_commits")), int(sysrd(f"{DRM}/stat_copy_us_total"))
    dc, dt = c1 - c0, t1 - t0
    print(f"1. copy duty, static desktop {seconds}s: {dc} commits ({dc / seconds:.2f}/s),"
          f" {dt / 1000:.0f} ms copying = {dt / seconds / 1e4:.2f} % of one core"
          f" (avg {dt // dc if dc else 0} us/commit)")


def l3_alpha(fd):
    """Scan the alpha byte of every pixel of L3's buffer. Returns (zero, total)."""
    addr = rd(0xf40 + 0x20 * 3)
    size = rd(0x038 + 0x20 * 3)
    pitch = rd(0x044 + 0x20 * 3) & 0xffff
    con = rd(0x030 + 0x20 * 3)
    w, h = size & 0xffff, size >> 16
    print(f"2. L3: ADDR=0x{addr:08x} {w}x{h} pitch {pitch} CON=0x{con:08x}"
          f" (fmt {(con >> 12) & 0xf}, aen {(con >> 8) & 1}, plane alpha 0x{con & 0xff:02x})")
    if not (addr and w and h and pitch):
        print("   L3 has no buffer programmed; skipping alpha scan")
        return None
    page = mmap.PAGESIZE
    start = addr & ~(page - 1)
    length = (addr - start) + pitch * h
    length = (length + page - 1) & ~(page - 1)
    try:
        mm = mmap.mmap(fd, length, mmap.MAP_SHARED, mmap.PROT_READ, offset=start)
    except OSError as e:
        print(f"   can't map L3's buffer ({e}); skipping alpha scan")
        return None
    base = addr - start
    nonzero = 0
    other = {}
    for y in range(h):
        row = mm[base + y * pitch: base + y * pitch + w * 4]
        a = row[3::4]                       # ARGB8888 little-endian: byte 3 = A
        nz = len(a) - a.count(0)
        if nz:
            nonzero += nz
            if len(other) < 5:
                other[y] = nz
    mm.close()
    total = w * h
    print(f"   alpha != 0 in {nonzero} of {total} px"
          + (f" (first rows: {other})" if other else " -> fully transparent"))
    return nonzero


def main():
    global regs, orig_src
    if os.geteuid() != 0:
        sys.exit("run with sudo (needs /dev/mem)")
    st = sysrd(f"{PS}/status")
    print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} kernel {os.uname().release} battery {st}")
    if st != "Discharging":
        print("WARNING: not discharging - unplug USB for valid mA numbers")

    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    mm = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=OVL0)
    regs = memoryview(mm).cast("I")
    if rd(0) == 0 and rd(4) == 0 and rd(SRC_CON) == 0:
        sys.exit("OVL0 reads all zero (unclocked/DEVAPC) - stop, don't retry")

    orig_src = rd(SRC_CON)
    print(f"OVL0 SRC_CON=0x{orig_src:08x} (layers on: "
          + ",".join(f"L{n}" for n in range(4) if orig_src >> n & 1) + ")")
    signal.signal(signal.SIGINT, restore)
    signal.signal(signal.SIGTERM, restore)

    copy_duty(60)
    nz = l3_alpha(fd)

    if not orig_src & L3_BIT:
        print("3. L3 already off - nothing to A/B")
        return
    if nz and not FORCE:
        print("3. L3 is NOT transparent - skipping A/B (FORCE=1 to run anyway)")
        return

    on, off = [], []
    print(f"3. L3 A/B: {ROUNDS} rounds x {SECS}s on / {SECS}s off")
    try:
        for r in range(ROUNDS):
            wr(SRC_CON, orig_src)
            time.sleep(5)
            on.append(avg_ma(SECS))
            wr(SRC_CON, orig_src & ~L3_BIT)
            time.sleep(5)
            off.append(avg_ma(SECS))
            print(f"   round {r + 1}: L3 on {on[-1]:.0f} mA, off {off[-1]:.0f} mA,"
                  f" diff {on[-1] - off[-1]:+.0f}", flush=True)
    finally:
        restore()
    d = [a - b for a, b in zip(on, off)]
    print(f"   mean L3 on {sum(on) / len(on):.0f} mA, off {sum(off) / len(off):.0f} mA;"
          f" L3 costs {sum(d) / len(d):+.0f} mA (per round: {', '.join(f'{x:+.0f}' for x in d)})")


if __name__ == "__main__":
    main()
