#!/usr/bin/env python3
# Read the ATF log buffer (0x7FF40000, 256 KB) / leftover text + WDT dump (0x7FF6A000) via /dev/mem mmap.
# Run on the Gemini as root: sudo -n busybox sh -c "python3 -I /home/andrew/connsys/atfraw.py [TAIL]"  (notes/vproc-research.md)
import mmap, os, re
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
m = mmap.mmap(fd, 0x40000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0x7ff40000)
b = bytes(m[0x100:0x40000])
first_nul = b.find(b"\0")
print("new text ends at", hex(first_nul))
# printable runs after the new text
for mo in re.finditer(rb"[\x20-\x7e\n\r\t]{40,}", b[first_nul:]):
    off = first_nul + mo.start()
    t = mo.group().decode()
    print("RUN at +%#x len %d" % (off, len(t)))
    print(t[:300].replace("\n", " | "))
    print("... last:", t[-1500:])
    break
