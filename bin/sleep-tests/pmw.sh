#!/bin/sh
# pmw.sh ADDR VALUE: write one MT6351 register through the SPM pwrap DVFS port (table entry 0 + SPM_DVFS_CON kick,
# as stock _spm_dvfs_ctrl_volt), then zero entry 0 again.
a=$1; v=$2
busybox devmem 0x1000D10C 32 $a; busybox devmem 0x1000D110 32 $v
busybox devmem 0x10006000 32 $(( (0xb16 << 16) | 1 ))
c=$(busybox devmem 0x10006400 32); busybox devmem 0x10006400 32 $(( c & ~0xF ))
i=0; while [ $(( $(busybox devmem 0x10006400 32) & 0x80000000 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 100 ] && { echo "pmw $a: no ack"; break; }; done
busybox devmem 0x1000D10C 32 0; busybox devmem 0x1000D110 32 0
