// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-spm-probe.c — READ-ONLY look at the MT6797 SPM (System Power
 * Manager, 0x10006000) as LK/ATF/our kernel leave it, before any SPM
 * firmware is loaded (claude/idle-power.md, SPM port step 1).
 *
 * Vendor receipts (gemian/gemini-linux-kernel-3.18):
 *   drivers/misc/mediatek/base/power/include/spm_v2/mt_spm_reg_mt6797.h  (map)
 *   drivers/misc/mediatek/base/power/include/spm_v2/pcm_def_mt6797.h     (R0/R7/R12/R13 bits)
 *   drivers/misc/mediatek/base/power/include/spm_v2/mt_spm_internal.h    (defaults)
 *   drivers/misc/mediatek/base/power/spm_v2/mt_spm.c, mt_spm_internal.c  (sequences)
 *
 * NOTHING IS WRITTEN. Only readl() on an ioremap of 0x10006000..0x10006fff
 * (the same block mainline scpsys already uses for PWR_STATUS/PWR_CON).
 * Deliberately NOT read: PCM_IM_HOST_RW_PTR/DAT (0x038/0x03C — host access
 * port into the IM, reads may auto-increment).
 *
 * Guard: if POWERON_CONFIG_EN bit0 (BCLK_CG_EN, vendor writes 0x0b160001
 * before any access) is clear, only POWERON_CONFIG_EN and PWR_STATUS
 * (which scpsys reads anyway) are read, unless force=1.
 *
 * Parameters:
 *   force=1    read everything even if BCLK_CG_EN is clear
 *   samples=N  re-read PC (r15) / FSM / timer N times 1 ms apart (default 5)
 *              -> tells a running PCM program from a parked one
 *   im=1       if PCM_IM_PTR/LEN point into DRAM, memremap that range
 *              read-only, crc32 it and match against the known blobs
 *
 * Output: kernel log, prefix "mt6797-spm:", and
 * /sys/kernel/debug/mt6797_spm_probe/status (re-reads on every cat).
 */

#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/seq_file.h>

static bool force;
module_param(force, bool, 0644);
MODULE_PARM_DESC(force, "read everything even if POWERON_CONFIG_EN bit0 is clear");

static int samples = 5;
module_param(samples, int, 0644);
MODULE_PARM_DESC(samples, "PC/FSM samples 1 ms apart (0..50)");

static bool im;
module_param(im, bool, 0644);
MODULE_PARM_DESC(im, "memremap the IM program at PCM_IM_PTR (read-only) and identify it by crc32");

#define SPM_PHYS	0x10006000
#define SPM_SIZE	0x1000

/* --- register offsets (mt_spm_reg_mt6797.h) --- */
#define POWERON_CONFIG_EN	0x000
#define SPM_POWER_ON_VAL0	0x004
#define SPM_POWER_ON_VAL1	0x008
#define SPM_CLK_CON		0x00C
#define SPM_CLK_SETTLE		0x010
#define SPM_AP_STANDBY_CON	0x014
#define PCM_CON0		0x018
#define PCM_CON1		0x01C
#define PCM_IM_PTR		0x020
#define PCM_IM_LEN		0x024
#define PCM_REG_DATA_INI	0x028
#define PCM_PWR_IO_EN		0x02C
#define PCM_TIMER_VAL		0x030
#define PCM_WDT_VAL		0x034
#define PCM_EVENT_VECTOR0	0x040	/* ..15 at +4 */
#define PCM_EVENT_VECTOR_EN	0x080
#define SPM_SCP_MAILBOX		0x098
#define SPM_SCP_IRQ		0x09C
#define SPM_TWAM_CON		0x0A0
#define SPM_CPU_WAKEUP_EVENT	0x0B0
#define SPM_IRQ_MASK		0x0B4
#define SPM_SRC_REQ		0x0B8
#define SPM_SRC_MASK		0x0BC
#define SPM_SRC2_MASK		0x0C0
#define SPM_WAKEUP_EVENT_MASK	0x0C4
#define SPM_WAKEUP_EVENT_EXT_MASK 0x0C8
#define PCM_DEBUG_CON		0x0D4
#define PCM_REG0_DATA		0x100	/* ..15 at +4 */
#define PCM_REG12_MASK_B_STA	0x140
#define PCM_REG12_EXT_DATA	0x144
#define PCM_REG12_EXT_MASK_B_STA 0x148
#define PCM_EVENT_REG_STA	0x14C
#define PCM_TIMER_OUT		0x150
#define PCM_WDT_OUT		0x154
#define SPM_IRQ_STA		0x158
#define SPM_WAKEUP_STA		0x15C
#define SPM_WAKEUP_EXT_STA	0x160
#define SPM_WAKEUP_MISC		0x164
#define BUS_PROTECT_RDY		0x168
#define BUS_PROTECT2_RDY	0x16C
#define SUBSYS_IDLE_STA		0x170
#define CPU_IDLE_STA		0x174
#define CPU_IDLE_STA2		0x178
#define PCM_FSM_STA		0x17C
#define PWR_STATUS		0x180
#define PWR_STATUS_2ND		0x184
#define CPU_PWR_STATUS		0x188
#define CPU_PWR_STATUS_2ND	0x18C
#define PCM_WDT_LATCH_0		0x190
#define PCM_WDT_LATCH_1		0x194
#define PCM_WDT_LATCH_2		0x198
#define DRAMC_DBG_LATCH		0x19C
#define PCM_WDT_LATCH_3		0x1C4
#define PCM_WDT_LATCH_4		0x1C8
#define SPM_SRC_RDY_STA		0x1D0
#define SPM_WAKEUP_MISC2	0x1E0
#define CPU_RET_STATUS		0x1E4
#define MCUSYS_PWR_CON		0x200
#define MP_TOP_PWR_CON		0x204
#define MP0_CPUSYS_PWR_CON	0x210	/* MP0..MP3 at +4 */
#define MP0_CPU0_PWR_CON	0x220	/* 16 at +4: MP0 c0-3, MP1 c0-3, MP2 c0-3, MP3 c0-3 */
#define CPU_EXT_BUCK_ISO	0x290
#define BIG_CLK_CON		0x2C0
#define LITTLE_CLK_CON		0x2C4
#define SPM_DVFS_CON		0x400
#define SPM_MAS_PAUSE_MASK_B	0x408
#define SPM_MAS_PAUSE2_MASK_B	0x40C
#define SPM_BSI_D0_SR		0x41C
#define SPM_BSI_D1_SR		0x420
#define SPM_BSI_D2_SR		0x424
#define SPM_AP_SEMA		0x428
#define SPM_SPM_SEMA		0x42C
#define DRAMC_DPY_CLK_SW_CON	0x440
#define DPY_LP_CON		0x444
#define CPU_DVFS_REQ		0x448
#define SPM_PLL_CON		0x44C
#define ULPOSC_CON		0x458
#define MP0_CPU0_IRQ_MASK	0x500	/* 16 at +4 */
#define MP0_CPU0_WFI_EN		0x540	/* 16 at +4 */
#define SPM_SW_FLAG		0x600
#define SPM_SW_DEBUG		0x604
#define SPM_SW_RSV_0		0x608	/* ..5 at +4 */
#define SPM_RSV_CON		0x620
#define SPM_RSV_STA		0x624
#define SPM_PASR_DPD_0		0x630	/* ..3 at +4 */

#define PCM_FSM_STA_DEF		0x00048490	/* after PCM reset (mt_spm_internal.h) */
#define SPM_WAKEUP_EVENT_MASK_DEF 0xf0f83ebb
#define POWER_ON_VAL1_DEF	0x00015820

static void __iomem *spm;

static inline u32 rd(u32 off) { return readl(spm + off); }

static const char * const r12_names[32] = {
	"PCM_TIMER", "MD32_WDT", "KP_IRQ", "APWDT", "APXGPT1", "CONN2AP_SPM_WAKEUP",
	"EINT", "CONN_WDT", "CCIF0", "LOWBATTERY", "MD32_SPM_IRQ", "26M_WAKE",
	"26M_SLEEP", "PCM_WDT_WAKEUP", "USB_CDSC", "USB_POWERDWN", "C2K_WDT",
	"EINT_SECURE", "CCIF1", "UART0", "AFE_IRQ_MCU", "THERM_CTRL", "SYS_CIRQ",
	"MD2_WDT", "CSYSPWREQ", "MD1_WDT", "CLDMA", "SEJ_WDT_GPT", "ALL_MD32_WAKEUP",
	"CPU_IRQ", "APSRC_WAKE", "APSRC_SLEEP",
};

/* PWR_STATUS bits: mainline scpsys mt6797 + vendor mt_idle_mt6797.h + 2026-10-07 notes */
static const char * const pwr_names[32] = {
	[0] = "MD1", [1] = "CONN", [2] = "DPY", [3] = "DIS(MM)", [5] = "ISP",
	[6] = "IFR", [7] = "VDE", [12] = "MFG", [13] = "MFG_ASYNC", [20] = "MJC",
	[21] = "VEN", [24] = "AUDIO", [28] = "C2K",
};

static const char * const cpu_names[10] = {
	"cpu0", "cpu1", "cpu2", "cpu3", "cpu4", "cpu5", "cpu6", "cpu7", "cpu8", "cpu9",
};

struct known_fw { u32 words; u32 crc; const char *name; };
/* crc32 (zlib) over the program words, computed from Hydra spm-fw/ and vendor mt_spm_mcdi.c */
static const struct known_fw known[] = {
	{  597, 0x35b7b0a3, "mcdi (vendor mt_spm_mcdi.c, pcm_mcdi_ddrdfs_v6.1)" },
	{ 1657, 0xa3aaf33f, "pcm_deepidle.bin (fpb_v17)" },
	{ 1657, 0x4b8354ef, "pcm_deepidle_by_mp1.bin" },
	{ 1583, 0xd2d4b3b3, "pcm_sodi.bin (fpb_v15)" },
	{ 1583, 0xfd9669fb, "pcm_sodi_by_mp1.bin" },
	{ 1822, 0x455771a0, "pcm_suspend.bin (v20)" },
	{ 1822, 0x6f770543, "pcm_suspend_by_mp1.bin" },
	{ 1993, 0x12d4444b, "pcm_vcorefs_hpm.bin (v10p8)" },
	{ 2037, 0xe31ca448, "pcm_vcorefs_lpm.bin (v10p8)" },
	{ 1993, 0x473d239d, "pcm_vcorefs_ultra.bin (v10p8)" },
};

static void bits(struct seq_file *s, const char *label, u32 v, const char * const *names, int n)
{
	int i;

	seq_printf(s, "  %-22s", label);
	if (!v)
		seq_puts(s, " (none)");
	for (i = 0; i < n; i++)
		if (v & BIT(i))
			seq_printf(s, " %s", names[i] ? names[i] : "?");
	seq_puts(s, "\n");
}

#define R(name) seq_printf(s, "  %-26s @%03x = 0x%08x\n", #name, name, rd(name))

static void dump_im(struct seq_file *s, u32 ptr, u32 len)
{
	u32 words = len + 1, crc, i;
	void *p;
	const u32 *w;

	if (ptr < 0x40000000 || words < 16 || words > 0x1000) {
		seq_printf(s, "  im: ptr 0x%08x / %u words not plausible, skipped\n", ptr, words);
		return;
	}
	p = memremap(ptr, words * 4, MEMREMAP_WB);
	if (!p) {
		seq_puts(s, "  im: memremap failed\n");
		return;
	}
	w = p;
	crc = crc32_le(~0U, p, words * 4) ^ ~0U;
	seq_printf(s, "  im: %u words at 0x%08x crc32 %08x first %08x %08x %08x %08x\n",
		   words, ptr, crc, w[0], w[1], w[2], w[3]);
	for (i = 0; i < ARRAY_SIZE(known); i++)
		if (known[i].words == words && known[i].crc == crc) {
			seq_printf(s, "  im: = %s\n", known[i].name);
			break;
		}
	if (i == ARRAY_SIZE(known))
		seq_puts(s, "  im: no match among known blobs\n");
	memunmap(p);
}

static int status_show(struct seq_file *s, void *unused)
{
	u32 pcfg, fsm, con0, con1, mask, v, i;
	int n;

	pcfg = rd(POWERON_CONFIG_EN);
	seq_printf(s, "SPM @0x%08x (read-only)\n", SPM_PHYS);
	seq_printf(s, "  POWERON_CONFIG_EN = 0x%08x (BCLK_CG_EN=%u, key field 0x%03x)\n",
		   pcfg, pcfg & 1, pcfg >> 16);
	v = rd(PWR_STATUS);
	seq_printf(s, "  PWR_STATUS = 0x%08x  2ND = 0x%08x\n", v, rd(PWR_STATUS_2ND));
	bits(s, "domains on:", v, pwr_names, 32);
	if (!(pcfg & 1) && !force) {
		seq_puts(s, "BCLK_CG_EN clear: stopping here (force=1 to read the rest)\n");
		return 0;
	}

	seq_puts(s, "\n[PCM core]\n");
	con0 = rd(PCM_CON0);
	con1 = rd(PCM_CON1);
	fsm = rd(PCM_FSM_STA);
	seq_printf(s, "  PCM_CON0 = 0x%08x: PCM_CK_EN=%u EN_IM_SLEEP_DVS=%u IM_AUTO_PDN=%u (KICK bits %u/%u)\n",
		   con0, !!(con0 & BIT(2)), !!(con0 & BIT(3)), !!(con0 & BIT(4)),
		   !!(con0 & BIT(0)), !!(con0 & BIT(1)));
	seq_printf(s, "  PCM_CON1 = 0x%08x: IM_SLAVE=%u IM_SLEEP=%u MIF_APBEN=%u IM_PDN=%u TIMER_EN=%u IM_NONRP=%u WDT_EN=%u WDT_WAKE=%u SRAM_SLEEP_B=%u SRAM_ISOINT_B=%u EVENT_LOCK=%u SCP_APB=%u\n",
		   con1, !!(con1 & BIT(0)), !!(con1 & BIT(1)), !!(con1 & BIT(3)),
		   !!(con1 & BIT(4)), !!(con1 & BIT(5)), !!(con1 & BIT(6)),
		   !!(con1 & BIT(8)), !!(con1 & BIT(9)), !!(con1 & BIT(10)),
		   !!(con1 & BIT(11)), !!(con1 & BIT(12)), !!(con1 & BIT(14)));
	seq_printf(s, "  PCM_FSM_STA = 0x%08x: inst_op=%u pc_st=%u im_st=%u master=%u event_fsm=%u clk_sel=%u PCM_KICK=%u IM_KICK=%u srcclken=%u srcvolten=%u%s\n",
		   fsm, fsm & 0xf, (fsm >> 4) & 7, (fsm >> 7) & 7, (fsm >> 10) & 0x1f,
		   (fsm >> 15) & 7, (fsm >> 18) & 7, !!(fsm & BIT(21)), !!(fsm & BIT(22)),
		   (fsm >> 23) & 3, !!(fsm & BIT(25)),
		   (fsm & 0x7fffff) == PCM_FSM_STA_DEF ? "  [= reset default 0x48490]" : "");
	R(PCM_IM_PTR); R(PCM_IM_LEN);
	R(PCM_REG_DATA_INI); R(PCM_PWR_IO_EN);
	R(PCM_TIMER_VAL); R(PCM_TIMER_OUT);
	R(PCM_WDT_VAL); R(PCM_WDT_OUT);
	R(PCM_EVENT_VECTOR_EN); R(PCM_DEBUG_CON);
	for (i = 0; i < 16; i++) {
		v = rd(PCM_EVENT_VECTOR0 + 4 * i);
		if (v)
			seq_printf(s, "  PCM_EVENT_VECTOR%-2u = 0x%08x\n", i, v);
	}
	for (i = 0; i < 16; i += 4)
		seq_printf(s, "  r%-2u..r%-2u = %08x %08x %08x %08x\n", i, i + 3,
			   rd(PCM_REG0_DATA + 4 * i), rd(PCM_REG0_DATA + 4 * i + 4),
			   rd(PCM_REG0_DATA + 4 * i + 8), rd(PCM_REG0_DATA + 4 * i + 12));
	seq_printf(s, "  (r15 = PC; r1 == 1 means vendor vcorefs firmware resident; r0/r7 drive clocks/PLLs when PWR_IO_EN set)\n");

	n = clamp(samples, 0, 50);
	if (n) {
		seq_printf(s, "  samples (1 ms): r15/FSM/TIMER_OUT:");
		for (i = 0; i < n; i++) {
			seq_printf(s, " [%x %x %x]", rd(PCM_REG0_DATA + 60), rd(PCM_FSM_STA),
				   rd(PCM_TIMER_OUT));
			mdelay(1);
		}
		seq_puts(s, "\n");
	}
	if (im)
		dump_im(s, rd(PCM_IM_PTR), rd(PCM_IM_LEN));

	seq_puts(s, "\n[power-on values / clocks]\n");
	R(SPM_POWER_ON_VAL0); R(SPM_POWER_ON_VAL1);
	seq_printf(s, "  (POWER_ON_VAL1 vendor default 0x%08x)\n", POWER_ON_VAL1_DEF);
	R(SPM_CLK_CON); R(SPM_CLK_SETTLE); R(SPM_AP_STANDBY_CON);
	R(SPM_SRC_REQ); R(SPM_SRC_MASK); R(SPM_SRC2_MASK); R(SPM_SRC_RDY_STA);
	R(SPM_PLL_CON); R(ULPOSC_CON); R(BIG_CLK_CON); R(LITTLE_CLK_CON);
	R(SPM_DVFS_CON); R(CPU_DVFS_REQ); R(DRAMC_DPY_CLK_SW_CON); R(DPY_LP_CON);
	R(SPM_MAS_PAUSE_MASK_B); R(SPM_MAS_PAUSE2_MASK_B);

	seq_puts(s, "\n[wake / irq]\n");
	mask = rd(SPM_WAKEUP_EVENT_MASK);
	seq_printf(s, "  SPM_WAKEUP_EVENT_MASK = 0x%08x%s\n", mask,
		   mask == SPM_WAKEUP_EVENT_MASK_DEF ? " [= vendor init default]" :
		   mask == 0xffffffff ? " [all masked]" : "");
	bits(s, "unmasked wake srcs:", ~mask, r12_names, 32);
	R(SPM_WAKEUP_EVENT_EXT_MASK);
	v = rd(SPM_WAKEUP_STA);
	seq_printf(s, "  SPM_WAKEUP_STA = 0x%08x\n", v);
	bits(s, "raw wake pending:", v, r12_names, 32);
	R(SPM_WAKEUP_EXT_STA); R(SPM_WAKEUP_MISC); R(SPM_WAKEUP_MISC2);
	R(PCM_REG12_MASK_B_STA); R(PCM_REG12_EXT_DATA); R(PCM_REG12_EXT_MASK_B_STA);
	R(PCM_EVENT_REG_STA);
	R(SPM_CPU_WAKEUP_EVENT); R(SPM_IRQ_MASK); R(SPM_IRQ_STA);
	R(SPM_SCP_MAILBOX); R(SPM_SCP_IRQ); R(SPM_TWAM_CON);
	seq_puts(s, "  last-sleep backups (vendor copies wake info here):\n");
	R(SPM_BSI_D0_SR); R(SPM_BSI_D1_SR); R(SPM_BSI_D2_SR);
	R(SPM_AP_SEMA); R(SPM_SPM_SEMA);

	seq_puts(s, "\n[idle / bus status]\n");
	R(SUBSYS_IDLE_STA); R(CPU_IDLE_STA); R(CPU_IDLE_STA2);
	R(BUS_PROTECT_RDY); R(BUS_PROTECT2_RDY); R(CPU_RET_STATUS);

	seq_puts(s, "\n[CPU power]\n");
	v = rd(CPU_PWR_STATUS);
	seq_printf(s, "  CPU_PWR_STATUS = 0x%08x  2ND = 0x%08x\n", v, rd(CPU_PWR_STATUS_2ND));
	/*
	 * CPU_PWR_STATUS bits do NOT track hotplug 1:1 (2026-10-13: all cores
	 * online read 0x003f82c8; vendor "cpu0 = bit15" map gave cpu1-5 = 0).
	 * Decode the per-core MTCMOS from MPx_CPUy_PWR_CON bit2 (PWR_ON)
	 * instead: cpu0-3 = MP0 c0-3, cpu4-7 = MP1 c0-3, cpu8-9 = MP2 c0-1.
	 */
	seq_puts(s, "  cores powered (PWR_CON bit2):");
	for (i = 0; i < 10; i++)
		seq_printf(s, " %s=%u", cpu_names[i],
			   !!(rd(MP0_CPU0_PWR_CON + 4 * i) & BIT(2)));
	seq_puts(s, "\n");
	R(MCUSYS_PWR_CON); R(MP_TOP_PWR_CON); R(CPU_EXT_BUCK_ISO);
	for (i = 0; i < 4; i++)
		seq_printf(s, "  MP%u_CPUSYS_PWR_CON        @%03x = 0x%08x\n", i,
			   MP0_CPUSYS_PWR_CON + 4 * i, rd(MP0_CPUSYS_PWR_CON + 4 * i));
	seq_puts(s, "  MPx_CPUy_PWR_CON (bit0 RST_B,1 ISO,2 ON,3 ON_2ND,4 CLK_DIS,8 SRAM_PDN,12 PDN_ACK):\n");
	for (i = 0; i < 16; i += 4)
		seq_printf(s, "    MP%u c0..3 = %08x %08x %08x %08x   IRQ_MASK %x%x%x%x  WFI_EN %x%x%x%x\n",
			   i / 4,
			   rd(MP0_CPU0_PWR_CON + 4 * i), rd(MP0_CPU0_PWR_CON + 4 * i + 4),
			   rd(MP0_CPU0_PWR_CON + 4 * i + 8), rd(MP0_CPU0_PWR_CON + 4 * i + 12),
			   rd(MP0_CPU0_IRQ_MASK + 4 * i), rd(MP0_CPU0_IRQ_MASK + 4 * i + 4),
			   rd(MP0_CPU0_IRQ_MASK + 4 * i + 8), rd(MP0_CPU0_IRQ_MASK + 4 * i + 12),
			   rd(MP0_CPU0_WFI_EN + 4 * i), rd(MP0_CPU0_WFI_EN + 4 * i + 4),
			   rd(MP0_CPU0_WFI_EN + 4 * i + 8), rd(MP0_CPU0_WFI_EN + 4 * i + 12));

	seq_puts(s, "\n[software registers (shared with ATF / firmware)]\n");
	R(SPM_SW_FLAG); R(SPM_SW_DEBUG);
	for (i = 0; i < 6; i++)
		seq_printf(s, "  SPM_SW_RSV_%u               @%03x = 0x%08x\n", i,
			   SPM_SW_RSV_0 + 4 * i, rd(SPM_SW_RSV_0 + 4 * i));
	R(SPM_RSV_CON); R(SPM_RSV_STA);
	for (i = 0; i < 4; i++)
		seq_printf(s, "  SPM_PASR_DPD_%u             @%03x = 0x%08x\n", i,
			   SPM_PASR_DPD_0 + 4 * i, rd(SPM_PASR_DPD_0 + 4 * i));

	seq_puts(s, "\n[PCM watchdog latches (read only; last PCM WDT event)]\n");
	R(PCM_WDT_LATCH_0); R(PCM_WDT_LATCH_1); R(PCM_WDT_LATCH_2);
	R(PCM_WDT_LATCH_3); R(PCM_WDT_LATCH_4); R(DRAMC_DBG_LATCH);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static struct dentry *dir;

static int log_show(struct seq_file *s, void *unused) { return status_show(s, NULL); }

static int __init probe_init(void)
{
	struct seq_file s = { };
	char *buf, *line, *p;

	spm = ioremap(SPM_PHYS, SPM_SIZE);
	if (!spm)
		return -ENOMEM;

	/* print the same report to the kernel log, line by line */
	buf = kzalloc(PAGE_SIZE * 4 + 1, GFP_KERNEL);	/* +1: always NUL-terminated */
	if (buf) {
		s.buf = buf;
		s.size = PAGE_SIZE * 4;
		log_show(&s, NULL);
		p = buf;
		while ((line = strsep(&p, "\n")))
			if (*line)
				pr_info("mt6797-spm: %s\n", line);
		kfree(buf);
	}

	dir = debugfs_create_dir("mt6797_spm_probe", NULL);
	debugfs_create_file("status", 0444, dir, NULL, &status_fops);
	return 0;
}

static void __exit probe_exit(void)
{
	debugfs_remove_recursive(dir);
	iounmap(spm);
}

module_init(probe_init);
module_exit(probe_exit);
MODULE_DESCRIPTION("MT6797 SPM state probe (read-only; Gemini PDA)");
MODULE_LICENSE("GPL");
