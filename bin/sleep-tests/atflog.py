#!/usr/bin/env python3
# Read the ATF log buffer (0x7FF40000, 256 KB) / leftover text + WDT dump (0x7FF6A000) via /dev/mem mmap.
# Run on the Gemini as root: sudo -n busybox sh -c "python3 -I /home/andrew/connsys/atflog.py [TAIL]"
import mmap, os, struct, sys
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x40000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x7ff40000)
hdr = m[0:0x100]
wp = struct.unpack_from("<Q", hdr, 8)[0]
txt = m[0x100:0x40000].split(b"\0")[0].decode("latin1", "replace")
print("hdr", hdr[:32].hex(), "wp", hex(wp), "len", len(txt))
sys.stdout.write(txt[-int(sys.argv[1]):] if len(sys.argv) > 1 else txt)
