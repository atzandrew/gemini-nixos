#!/bin/sh
# E18b: manual pwrap DVFS command via SPM_DVFS_CON (stock _spm_dvfs_ctrl_volt), idx 0 -> PMIC 0x630 (VMODEM VOSEL, buck off) = 0x15
pm() { busybox cat /sys/kernel/debug/regmap/1000d000.pwrap/registers | grep "^0630:"; }
echo "before: $(pm) POWERON_CONFIG_EN $(busybox devmem 0x10006000 32) DVFS_CON $(busybox devmem 0x10006400 32)"
busybox devmem 0x1000D10C 32 0x630; busybox devmem 0x1000D110 32 0x15
busybox devmem 0x10006000 32 $(( (0xb16 << 16) | 1 ))
c=$(busybox devmem 0x10006400 32); busybox devmem 0x10006400 32 $(( c & ~0xF ))
i=0; while [ $(( $(busybox devmem 0x10006400 32) & 0x80000000 )) -eq 0 ]; do i=$((i+1)); [ $i -gt 100 ] && { echo "no ack"; break; }; done
echo "after: $(pm) DVFS_CON $(busybox devmem 0x10006400 32) polls $i"
busybox devmem 0x1000D10C 32 0; busybox devmem 0x1000D110 32 0
