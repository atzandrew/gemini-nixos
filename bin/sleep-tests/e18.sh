#!/bin/sh
# E18: trace which pwrap DVFS commands the SPM suspend firmware sends. Each of the 16 table entries
# (PMIC_WRAP_DVFS_ADRn/WDATAn, 0x1000D10C + 8n) is pointed at a voltage-select register of a DISABLED buck
# (VMODEM/VMD1/VSRAM_MD/VPA CON4-6; EN bit0 = 0) with a unique marker. After the run, the markers that landed show
# which command indexes ran. Run A: flags 0x5fc102ff, markers 0x10+n. Run B: bit 3 cleared, markers 0x20+n.
# Panel protected (e16-run.sh: DCS 0x28/0x29), CONN off, 3 cycles each. Table restored to all-zero at the end.
O=/home/andrew/connsys/e18.log; : > $O
T="0630 0632 0634 0644 0646 0648 0658 065a 065c 0694 0696 0698 0630 0632 0634 0644"
pm() { sudo -n busybox cat /sys/kernel/debug/regmap/1000d000.pwrap/registers | grep -E "^($(echo $T | tr " " "|")):" | sort -u | tr "\n" " "; }
table() {	# $1 = marker base
	n=0; for a in $T; do busybox devmem $((0x1000D10C + n*8)) 32 $((0x$a)); busybox devmem $((0x1000D110 + n*8)) 32 $(( $1 + n )); n=$((n+1)); done
	echo "table base $1: ADR0 $(busybox devmem 0x1000D10C 32) WDATA0 $(busybox devmem 0x1000D110 32) ADR15 $(busybox devmem 0x1000D184 32) WDATA15 $(busybox devmem 0x1000D188 32)" >> $O
}
zero() { for n in $(seq 0 15); do busybox devmem $((0x1000D10C + n*8)) 32 0; busybox devmem $((0x1000D110 + n*8)) 32 0; done; echo "table zeroed" >> $O; }
echo "before: $(pm)" >> $O
table 0x10; sh /home/andrew/connsys/e16-run.sh 3 0x5fc102ff /home/andrew/connsys/e18-A.log; echo "after A: $(pm)" >> $O
sleep 10
table 0x20; sh /home/andrew/connsys/e16-run.sh 3 0x5fc102f7 /home/andrew/connsys/e18-B.log; echo "after B: $(pm)" >> $O
zero; echo "=== all done $(date +%T)" >> $O
