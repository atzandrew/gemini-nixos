// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-spm-load.c — SPM port step 2: load a vendor PCM firmware blob
 * into the MT6797 SPM instruction memory (IM). stage<=1 never kicks the
 * PCM; stage=2 kicks it with the sleep condition held impossible.
 * claude/idle-power.md, "SPM port".
 *
 * Vendor receipts (gemian/gemini-linux-kernel-3.18):
 *   base/power/spm_v2/mt_spm.c          spm_load_pcm_firmware(), spm_register_init()
 *   base/power/spm_v2/mt_spm_internal.c __spm_reset_and_init_pcm(), __spm_kick_im_to_fetch()
 *   base/power/mt6797/mt_cpufreq_hybrid.c FSM_IM_EMPT/FILL/REDY bits (same PCM core in the CSPM)
 *   emi_mpu/mt6797/emi_mpu.c            4 GB mode = INFRACFG_AO+0xf00 bit13
 *   include/mt-plat/mt_lpae.h           MAPPING_DRAM_ACCESS_ADDR: in 4 GB mode a
 *                                       DRAM master sees CPU phys [0x40000000,0xC0000000)
 *                                       at +0x40000000
 *
 * Parameters (one stage per insmod; default = nothing written):
 *   fw=NAME     firmware file (request_firmware_direct, /lib/firmware/NAME),
 *               default "mt6797-spm/pcm_suspend.bin"
 *   stage=0     parse + validate the blob, read 4 GB mode and the PCM state. NO writes.
 *   stage=1     2a: PCM reset -> CON0/CON1 init -> IM_PTR/LEN -> IM_KICK toggle,
 *               wait for FSM IM_REDY (bit 9). No PCM_KICK, PWR_IO_EN untouched,
 *               PCM WDT bits preserved (found off), no wake/IRQ/timer changes.
 *   stage=2     2b: stage 1, then kick-and-park on cpu0 (see kick_test()):
 *               the PCM runs for watch_us with the AP-standby (sleep) condition
 *               held false by a busy cpu0, PWR_IO_EN = 0, SW_FLAG = flags;
 *               then vendor PCM reset + restore. Traces PC/FSM/SW_DEBUG.
 *   stage=3     2c: as stage 2, plus (only) wake mask bit0 unmasked and
 *               SPM_CPU_WAKEUP_EVENT = 1 at wake_at_us -> firmware abort/return
 *               path; wake status read back, vendor clean-after-wakeup, reset.
 *   stage=4     2d: first real standby entry, everything disabled. Kick, raise
 *               the CPU wake event (R12 bit0) right away, then cpu0 WFIs with
 *               IRQs masked (wakes on the next IRQ), IRQs briefly on between
 *               tries, until the firmware moves (max 50). Runs via work_on_cpu(0)
 *               with preemption off.
 *               Standby condition = cpu0 WFI only: WFI_OP_AND, WFI_EN cpu0 = 1,
 *               all others 0 (vendor SODI runs AND with the offline A72s'
 *               WFI_EN = 0, so 0 = ignored), all six cluster/mcusys idle masks
 *               set. SW_FLAG |= DIS_SRCCLKEN_LOW. No dormant/PSCI, no ACINACTM,
 *               PWR_IO_EN = 0, pwrap table -> HWCID. Expect the firmware's
 *               sleep-abort path. Same status read / clean / reset / restore.
 *   stage=5     2e: as stage 4, but the wake is the PCM timer (vendor
 *               __spm_set_wakeup_event: PCM_TIMER_VAL = timer_ticks, PCM_CON1
 *               TIMER_EN, R12 bit0 PCM_TIMER unmasked) instead of the CPU wake
 *               event (which pcm_suspend_v20 ignored in 2d).
 *   stage=6     3: FIRST REAL SLEEP (outputs on). Requires cpu0 to be the only
 *               online CPU (offline cpu1-9 first). Per cycle (cycles x sleep_ms,
 *               gap_ms awake between): IM reload, same setup as stage 5 (PCM
 *               timer wake = sleep_ms, cpu0-only standby, SW_FLAG = flags |
 *               DIS_SRCCLKEN_LOW, pwrap table -> HWCID), plus
 *                 - PCM_PWR_IO_EN = io_en right before the kick (vendor
 *                   __spm_kick_pcm_to_run); only 0x80 (r7) or 0x81 (r0+r7);
 *                   refused unless r0/r7 equal POWER_ON_VAL0/1 at that moment
 *                   (= enabling the outputs changes nothing until the
 *                   firmware sleeps);
 *                 - SPM IRQ (GIC SPI 180, level-low via sysirq) requested and
 *                   given GIC priority 0x80; cpu0 WFIs with ICC_PMR_EL1 = 0x90,
 *                   so only the SPM IRQ (PCM_RETURN) can end the WFI (vendor:
 *                   mt_irq_mask_all + unmask SPM_IRQ0); SPM IRQ_MASK = all but
 *                   PCM_RETURN;
 *                 - with r0 (CPU/ARM PLL clock off): ACINACTM on the clusters
 *                   in acinactm (bit0 MP0, bit1 MP1; bit2 MP2 refused until
 *                   0x1022220C is confirmed) through ATF SMC
 *                   0x82000201 (vendor dpidle/SODI MCUSYS_SMC_WRITE), cleared
 *                   after the wake; r0 is refused without acinactm.
 *               After PCM_RETURN: wake status; with r0, r0 -> POWER_ON_VAL0 (vendor
 *               mem-control backup, kept after the module); clean-after-wakeup, PWR_IO_EN = 0,
 *               PCM reset. No PCM_RETURN by sleep_ms + 2 s (only checked when the
 *               WFI returns, i.e. never if the SPM IRQ never comes) -> PWR_IO_EN = 0
 *               first (outputs back to POWER_ON_VAL), then reset (abort path).
 *               No SYS_CIRQ, no PSCI/dormant, PCM WDT off, toprgu untouched.
 *   axi_test=1  (stage=0 only) ATF SMC 0x82000201 no-op test: write MP0/MP1
 *               AXI_CONFIG back with the value just read, report a0 + readback.
 *   verify=1    (with stage>=1) read the IM back through PCM_IM_HOST_RW_PTR/DAT
 *               (bit31 HOST_EN, W_EN = 0) and compare with the blob. Unvalidated
 *               protocol (no vendor user) — separate run.
 *   force=1     go ahead even if the PCM is not in the reset-default state
 *               (refused otherwise: something else owns the SPM).
 *
 * Unload (rmmod) after stage=1: PCM reset again and restore PCM_CON0/CON1/
 * IM_PTR/IM_LEN to the values found at load, then unmap/free the buffer.
 *
 * Buffer: alloc_pages(GFP_DMA) on a private platform device (32-bit DMA
 * mask, non-coherent -> dma_map_page cleans the cache), retried until the
 * block lies below 0xC0000000 so it is reachable in both 4 GB and normal
 * mode. Only a DMA read of our own buffer by the SPM happens.
 */

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/arm-smccc.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include <asm/barrier.h>
#include <asm/sysreg.h>
#include <asm/arch_gicv3.h>

static char *fw = "mt6797-spm/pcm_suspend.bin";
module_param(fw, charp, 0444);
MODULE_PARM_DESC(fw, "PCM firmware under /lib/firmware");

static int stage;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "0 parse only (no writes), 1 load into IM (no kick), 2 kick-and-park, 3 = 2 + CPU wake event, 4 = standby entry (one WFI) with wake pending, 5 = 4 with PCM timer wake, 6 = real sleep (PWR_IO_EN = io_en; cpu0 alone)");

static bool verify;
module_param(verify, bool, 0444);
MODULE_PARM_DESC(verify, "stage=1: read IM back via the host port and compare (always done before a kick)");

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "proceed even if the PCM is not in its reset-default state");

static uint flags = 0xffc102ff;
module_param(flags, uint, 0444);
MODULE_PARM_DESC(flags, "stage=2: SPM_SW_FLAG (pcm_flags); default = every DIS_* + DONT_TOUCH_* bit");

static uint watch_us = 20000;
module_param(watch_us, uint, 0444);
MODULE_PARM_DESC(watch_us, "stage=2: how long the PCM runs (busy-watched on cpu0, IRQs off), max 100000");

static uint wake_at_us = 5000;
module_param(wake_at_us, uint, 0444);
MODULE_PARM_DESC(wake_at_us, "stage=3: when to write SPM_CPU_WAKEUP_EVENT = 1 (us after the kick)");

static bool pwrap_safe = true;
module_param(pwrap_safe, bool, 0444);
MODULE_PARM_DESC(pwrap_safe, "stage=3: point all 16 pwrap SPM (DVFS) commands at the read-only MT6351 HWCID (0x0200) for the run (default on; restored after)");

static uint timer_ticks = 33;
module_param(timer_ticks, uint, 0444);
MODULE_PARM_DESC(timer_ticks, "stage=5: PCM timer wake after this many 32 kHz ticks (default 33 = ~1 ms)");

static uint wake_unmask = 0x1;
module_param(wake_unmask, uint, 0444);
MODULE_PARM_DESC(wake_unmask, "stage=3: R12 wake bits unmasked during the run (bit0 = PCM_TIMER/CPU wake, vendor MCDI/vcorefs)");

static uint io_en = 0x80;
module_param(io_en, uint, 0444);
MODULE_PARM_DESC(io_en, "stage=6: PCM_PWR_IO_EN during the sleep: 0x80 = r7 only (default), 0x81 = r0 + r7");

static uint sleep_ms = 100;
module_param(sleep_ms, uint, 0444);
MODULE_PARM_DESC(sleep_ms, "stage=6: PCM timer per cycle (ms, 1..5000, default 100)");

static uint cycles = 1;
module_param(cycles, uint, 0444);
MODULE_PARM_DESC(cycles, "stage=6: sleep cycles (1..60, default 1)");

static uint gap_ms = 200;
module_param(gap_ms, uint, 0444);
MODULE_PARM_DESC(gap_ms, "stage=6: awake time between cycles (ms, >= 50, default 200)");

static uint acinactm;
module_param(acinactm, uint, 0444);
MODULE_PARM_DESC(acinactm, "stage=6: clusters whose ACINACTM is set around the WFI (bit0 MP0, bit1 MP1, bit2 MP2; required with r0)");

static uint src_mask = ~0U;
module_param(src_mask, uint, 0444);
MODULE_PARM_DESC(src_mask, "stage=6: SPM_SRC_MASK (0x0BC) during the run; default 0xffffffff = keep LK's (vendor suspend 0x10aa007e)");

static uint src2_mask = ~0U;
module_param(src2_mask, uint, 0444);
MODULE_PARM_DESC(src2_mask, "stage=6: SPM_SRC2_MASK (0x0C0) during the run; default 0xffffffff = keep LK's (vendor suspend 0x00f1001f)");

static bool axi_test;
module_param(axi_test, bool, 0444);
MODULE_PARM_DESC(axi_test, "stage=0: SMC 0x82000201 no-op write test on MP0/MP1 AXI_CONFIG");



#define SPM_PHYS		0x10006000
#define INFRA_AO_PHYS		0x10001000
#define INFRA_MISC		0xf00		/* bit13 = DRAM 4 GB mode */
#define PWRAP_PHYS		0x1000d000	/* PMIC wrapper (vendor pwrap_hal.h offsets) */
#define PWRAP_HIPRIO_ARB_EN	0x054		/* bit3 DVFSINF = SPM's PMIC command channel */
#define PWRAP_HARB_STA0		0x058
#define PWRAP_HARB_STA1		0x05c
#define PWRAP_DVFS_ADR0		0x10c		/* ADRn = 0x10c + 8n, WDATAn = +4, n = 0..15 */
#define PWRAP_SPMINF_STA	0x1b0
#define PWRAP_DVFSINF		BIT(3)

#define POWERON_CONFIG_EN	0x000
#define PCM_CON0		0x018
#define PCM_CON1		0x01C
#define PCM_IM_PTR		0x020
#define PCM_IM_LEN		0x024
#define PCM_PWR_IO_EN		0x02C
#define PCM_IM_HOST_RW_PTR	0x038
#define PCM_IM_HOST_RW_DAT	0x03C
#define PCM_REG15_DATA		0x13C
#define PCM_FSM_STA		0x17C

#define KEY			(0xb16U << 16)	/* SPM_REGWR_CFG_KEY */
/* PCM_CON0 */
#define PCM_KICK_L		BIT(0)
#define IM_KICK_L		BIT(1)
#define PCM_CK_EN		BIT(2)
#define EN_IM_SLEEP_DVS		BIT(3)
#define PCM_SW_RESET		BIT(15)
/* PCM_CON1 */
#define MIF_APBEN		BIT(3)
#define IM_NONRP_EN		BIT(6)
#define PCM_WDT_EN		BIT(8)
#define PCM_WDT_WAKE_MODE	BIT(9)
#define SPM_SRAM_SLEEP_B	BIT(10)
#define SPM_SRAM_ISOINT_B	BIT(11)
#define EVENT_LOCK_EN		BIT(12)
#define SRCCLKEN_FAST_RESP	BIT(13)
#define SCP_APB_INTERNAL_EN	BIT(14)
/* PCM_FSM_STA */
#define FSM_DEF			0x48490
#define FSM_IM_EMPT		BIT(7)
#define FSM_IM_FILL		BIT(8)
#define FSM_IM_REDY		BIT(9)
#define FSM_PCM_KICK		BIT(21)
#define FSM_IM_KICK		BIT(22)
/* PCM_IM_HOST_RW_PTR */
#define IM_HOST_EN		BIT(31)

#define IM_MAX_WORDS		2048		/* 11-bit host pointer */

struct pcm_tail {			/* struct pcm_desc from .size on */
	u16 size;
	u8 sess;
	u8 replace;
	u16 addr_2nd;
	u16 reserved;
	u32 vec[16];
} __packed;

static void __iomem *spm;
static void __iomem *pwrap;
static struct platform_device *pdev;
static struct page *page;
static unsigned int page_order;
static dma_addr_t dma;
static size_t dma_len;
static bool loaded;
static u32 saved_con0, saved_con1, saved_ptr, saved_len;

static inline u32 rd(u32 off) { return readl(spm + off); }
static inline void wr(u32 off, u32 v) { writel(v, spm + off); }

static void show_pcm(const char *when)
{
	u32 fsm = rd(PCM_FSM_STA);

	pr_info("mt6797-spm-load: [%s] CON0 %08x CON1 %08x IM_PTR %08x IM_LEN %08x PWR_IO_EN %08x FSM %08x (IM %s%s%s, PCM_KICK %u, IM_KICK %u) PC %08x\n",
		when, rd(PCM_CON0), rd(PCM_CON1), rd(PCM_IM_PTR), rd(PCM_IM_LEN),
		rd(PCM_PWR_IO_EN), fsm,
		fsm & FSM_IM_EMPT ? "EMPTY" : "", fsm & FSM_IM_FILL ? "FILL" : "",
		fsm & FSM_IM_REDY ? "READY" : "",
		!!(fsm & FSM_PCM_KICK), !!(fsm & FSM_IM_KICK), rd(PCM_REG15_DATA));
}

/* vendor reset: CON0 key|CK_EN|SW_RESET, key|CK_EN, poll FSM == default (<= 5 ms) */
static int pcm_reset(void)
{
	int i;

	wr(PCM_CON0, KEY | PCM_CK_EN | PCM_SW_RESET);
	for (i = 0; i < 5000; i++) {
		if ((rd(PCM_FSM_STA) & 0x7fffff) == FSM_DEF)
			break;
		udelay(1);
	}
	wr(PCM_CON0, KEY | PCM_CK_EN);
	if (i == 5000) {
		pr_err("mt6797-spm-load: PCM reset: FSM %08x != default\n", rd(PCM_FSM_STA));
		return -EIO;
	}
	return 0;
}

static int alloc_buf(struct device *dev, const void *prog, size_t bytes)
{
	struct page *bad[32];
	int nbad = 0, i, ret = -ENOMEM;

	page_order = get_order(bytes);
	while (nbad < ARRAY_SIZE(bad)) {
		struct page *p = alloc_pages(GFP_KERNEL | GFP_DMA | __GFP_NOWARN, page_order);
		phys_addr_t pa;

		if (!p)
			break;
		pa = page_to_phys(p);
		if (pa >= 0x40000000 && pa + (PAGE_SIZE << page_order) <= 0xC0000000) {
			page = p;
			ret = 0;
			break;
		}
		bad[nbad++] = p;
	}
	for (i = 0; i < nbad; i++)
		__free_pages(bad[i], page_order);
	if (ret) {
		pr_err("mt6797-spm-load: no buffer below 0xC0000000 (%d tries)\n", nbad);
		return ret;
	}

	memcpy(page_address(page), prog, bytes);
	dma_len = bytes;
	dma = dma_map_page(dev, page, 0, bytes, DMA_TO_DEVICE);	/* cleans D-cache */
	if (dma_mapping_error(dev, dma) || dma != page_to_phys(page)) {
		pr_err("mt6797-spm-load: dma_map_page gave %pad for phys %pa (bounce?) — refusing\n",
		       &dma, &(phys_addr_t){ page_to_phys(page) });
		if (!dma_mapping_error(dev, dma))
			dma_unmap_page(dev, dma, bytes, DMA_TO_DEVICE);
		__free_pages(page, page_order);
		page = NULL;
		return -EIO;
	}
	return 0;
}

static int do_verify(const u32 *prog, u32 words)
{
	u32 i, v, bad = 0, first = 0;

	for (i = 0; i < words; i++) {
		wr(PCM_IM_HOST_RW_PTR, IM_HOST_EN | i);
		v = rd(PCM_IM_HOST_RW_DAT);
		if (v != prog[i]) {
			if (!bad)
				first = i;
			bad++;
		}
		if (i < 4)
			pr_info("mt6797-spm-load: IM[%u] = %08x (blob %08x)\n", i, v, prog[i]);
	}
	wr(PCM_IM_HOST_RW_PTR, 0);
	if (bad)
		pr_err("mt6797-spm-load: verify: %u of %u words differ, first at %u\n", bad, words, first);
	else
		pr_info("mt6797-spm-load: verify: all %u words match\n", words);
	return bad ? -EIO : 0;
}

/* ------------------------------------------------------------------ stage 2 */
/*
 * Kick-and-park. The vendor suspend firmware waits for the AP standby
 * condition (SPM_AP_STANDBY_CON: WFI_OP_AND over the WFI_EN cores + cluster
 * idle bits) before it starts any sleep sequence. We make that condition
 * impossible: cpu0 participates (MP0_CPU0_WFI_EN = 1), MP0 cluster idle is
 * required (mp0top_idle_mask = 0), and cpu0 sits in a busy loop with IRQs
 * off for the whole time the PCM runs, so it never asserts STANDBYWFI —
 * whichever way the hardware treats the other cores. On top of that:
 * PCM_PWR_IO_EN = 0 (r0/r7 cannot drive clocks/PLLs/power), SW_FLAG has every
 * DIS_* / DONT_TOUCH_* bit, wake events all masked, PCM timer and PCM WDT off.
 * After watch_us the PCM is SW-reset (vendor reset) and every register we
 * wrote is restored. We record PC/FSM/SW_DEBUG/IRQ_STA changes and a
 * before/after diff of the SPM registers (= what the firmware itself wrote).
 */
#define AP_STANDBY_CON		0x014
#define PCM_REG_DATA_INI	0x028
#define PCM_EVENT_VECTOR0	0x040
#define SPM_IRQ_STA		0x158
#define SPM_MAS_PAUSE_MASK_B	0x408
#define SPM_MAS_PAUSE2_MASK_B	0x40C
#define MP0_CPU0_WFI_EN		0x540
#define SPM_SW_FLAG		0x600
#define SPM_SW_DEBUG		0x604
#define SPM_SW_RSV_0		0x608
#define SPM_POWER_ON_VAL0	0x004
#define SPM_POWER_ON_VAL1	0x008
#define SPM_CPU_WAKEUP_EVENT	0x0B0
#define SPM_SW_INT_CLEAR	0x094
#define SPM_IRQ_MASK		0x0B4
#define ISRM_RET_IRQ_AUX	(0x3ffU << 8)
#define ISRM_TWAM		BIT(2)
#define SPM_FLAG_DIS_SRCCLKEN_LOW BIT(15)
#define PCM_TIMER_VAL		0x030
#define PCM_TIMER_OUT		0x150
#define PCM_TIMER_EN		BIT(5)
#define PCM_WDT_VAL_OFF		0x034
#define SPM_SRC_REQ		0x0B8
#define SPM_SRC_MASK		0x0BC
#define SPM_SRC2_MASK		0x0C0
#define SPM_WAKEUP_EVENT_MASK	0x0C4
#define PCM_REG12_EXT_DATA	0x144
#define PCM_REG13_DATA		0x134
#define PCM_EVENT_REG_STA	0x14C
#define SPM_WAKEUP_STA		0x15C
#define SPM_WAKEUP_EXT_STA	0x160
#define SPM_WAKEUP_MISC		0x164
#define SUBSYS_IDLE_STA		0x170
#define SPM_BSI_D0_SR		0x41C
#define SPM_BSI_D1_SR		0x420
#define SPM_BSI_D2_SR		0x424
#define ISRS_PCM_RETURN		BIT(3)
#define PCM_RF_SYNC_R0		BIT(16)
#define PCM_RF_SYNC_R7		BIT(23)

/* registers diffed before/after the run (never 0x038/0x03C) */
static const u16 snap_offs[] = {
	0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020, 0x024, 0x028,
	0x02c, 0x030, 0x034, 0x080, 0x098, 0x09c, 0x0a0, 0x0b0, 0x0b4, 0x0b8, 0x0bc,
	0x0c0, 0x0c4, 0x0c8, 0x0d4,
	0x100, 0x104, 0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128,
	0x12c, 0x130, 0x134, 0x138, 0x13c, 0x140, 0x144, 0x148, 0x14c, 0x150, 0x154,
	0x158, 0x15c, 0x160, 0x164, 0x168, 0x16c, 0x170, 0x174, 0x178, 0x17c, 0x180,
	0x184, 0x188, 0x18c, 0x190, 0x194, 0x198, 0x19c, 0x1c4, 0x1c8, 0x1d0, 0x1e0, 0x1e4,
	0x200, 0x204, 0x210, 0x214, 0x218, 0x21c,
	0x220, 0x224, 0x228, 0x22c, 0x230, 0x234, 0x238, 0x23c, 0x240, 0x244,
	0x290, 0x2c0, 0x2c4,
	0x300, 0x304, 0x308, 0x30c, 0x310, 0x314, 0x318, 0x31c, 0x320, 0x324, 0x328,
	0x32c, 0x330, 0x334, 0x338, 0x33c, 0x340, 0x344, 0x348, 0x34c,
	0x400, 0x408, 0x40c, 0x41c, 0x420, 0x424, 0x428, 0x42c, 0x440, 0x444, 0x448,
	0x44c, 0x458,
	0x600, 0x604, 0x608, 0x60c, 0x610, 0x614, 0x618, 0x61c, 0x620, 0x624,
	0x630, 0x634, 0x638, 0x63c,
};

struct trace_ent { u32 t_us, pc, fsm, dbg, isr; };

static const u16 pw_offs[] = {
	PWRAP_HIPRIO_ARB_EN, PWRAP_HARB_STA0, PWRAP_HARB_STA1, PWRAP_SPMINF_STA,
	0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128, 0x12c, 0x130, 0x134, 0x138,
	0x13c, 0x140, 0x144, 0x148, 0x14c, 0x150, 0x154, 0x158, 0x15c, 0x160, 0x164, 0x168,
	0x16c, 0x170, 0x174, 0x178, 0x17c, 0x180, 0x184, 0x188,
};

struct kick_ctx {
	u32 pre[ARRAY_SIZE(snap_offs)], post[ARRAY_SIZE(snap_offs)];
	struct trace_ent tr[64];
	int ntr, cpu, reset_ret;
	u32 loops, wake_t;	/* wake_t: us when the wake event was written (0 = not) */
	u32 swflag;
	u64 wfi_ticks, cntfrq;
	int wfi_done, wfi_tries;
	struct trace_ent pre_wfi, post_wfi;
	u32 ws[16];		/* wake status, read before the reset */
	u32 pwpre[ARRAY_SIZE(pw_offs)], pwpost[ARRAY_SIZE(pw_offs)];
};

static void kick_on_cpu0(void *arg)
{
	struct kick_ctx *c = arg;
	u32 con0, i, us, last_pc = ~0, last_fsm = ~0, last_dbg = ~0, last_isr = ~0;

	c->cpu = smp_processor_id();
	if (c->cpu != 0)
		return;
	for (i = 0; i < ARRAY_SIZE(snap_offs); i++)
		c->pre[i] = rd(snap_offs[i]);
	for (i = 0; pwrap && i < ARRAY_SIZE(pw_offs); i++)
		c->pwpre[i] = readl(pwrap + pw_offs[i]);

	/* __spm_kick_pcm_to_run(), with PWR_IO_EN forced to 0 */
	wr(SPM_MAS_PAUSE_MASK_B, 0xffffffff);
	wr(SPM_MAS_PAUSE2_MASK_B, 0xffffffff);
	wr(PCM_REG_DATA_INI, 0);
	wr(SPM_SW_FLAG, c->swflag);
	wr(SPM_SW_RSV_0, 0);
	wr(PCM_PWR_IO_EN, 0);
	if (stage == 5) {	/* __spm_set_wakeup_event(): PCM timer as the wake source */
		wr(PCM_TIMER_VAL, timer_ticks);
		wr(PCM_CON1, (rd(PCM_CON1) & 0xffff) | KEY | PCM_TIMER_EN);
	}
	con0 = rd(PCM_CON0) & 0xffff & ~(IM_KICK_L | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN);
	if (stage == 4) {
		wr(SPM_CPU_WAKEUP_EVENT, 1);	/* wake pending before standby -> abort path */
		c->wake_t = 1;
	}

	for (us = 0; us < watch_us; us += 10) {
		u32 pc = rd(PCM_REG15_DATA), fsm = rd(PCM_FSM_STA);
		u32 dbg = rd(SPM_SW_DEBUG), isr = rd(SPM_IRQ_STA);

		if ((pc != last_pc || fsm != last_fsm || dbg != last_dbg || isr != last_isr) &&
		    c->ntr < ARRAY_SIZE(c->tr)) {
			c->tr[c->ntr++] = (struct trace_ent){ us, pc, fsm, dbg, isr };
			last_pc = pc; last_fsm = fsm; last_dbg = dbg; last_isr = isr;
		}
		c->loops++;
		if (stage >= 4 && !c->wfi_done && us >= 100) {
			u64 t0, t1;

			c->pre_wfi = (struct trace_ent){ us, rd(PCM_REG15_DATA), rd(PCM_FSM_STA),
							 rd(SPM_SW_DEBUG), rd(SPM_IRQ_STA) };
			/*
			 * Stage >= 4 runs in process context (work_on_cpu, preemption
			 * off, IRQs on). Each try: IRQs off, WFI (returns on the next
			 * IRQ), IRQs on again so pending IRQs are serviced and the next
			 * WFI really waits. Stop once the firmware has moved (standby
			 * seen) or after 50 tries. 2e showed a single masked WFI can
			 * return at once (0 ticks) when an IRQ is already pending.
			 */
			t0 = t1 = 0;
			for (c->wfi_tries = 0; c->wfi_tries < 50; c->wfi_tries++) {
				u64 a, b;

				local_irq_disable();
				a = read_sysreg(cntvct_el0);
				dsb(sy);
				wfi();
				b = read_sysreg(cntvct_el0);
				local_irq_enable();
				t1 += b - a;
				if (rd(PCM_REG15_DATA) != c->pre_wfi.pc) {
					c->wfi_tries++;
					break;
				}
			}
			c->post_wfi = (struct trace_ent){ us, rd(PCM_REG15_DATA), rd(PCM_FSM_STA),
							  rd(SPM_SW_DEBUG), rd(SPM_IRQ_STA) };
			c->wfi_ticks = t1 - t0;
			c->cntfrq = read_sysreg(cntfrq_el0);
			c->wfi_done = 1;
			last_pc = ~0;	/* force a trace entry after the WFI */
		}
		if (stage == 3 && !c->wake_t && us >= wake_at_us) {
			wr(SPM_CPU_WAKEUP_EVENT, 1);	/* "CPU wants out" (vendor MCDI exit / vcorefs offload) */
			c->wake_t = us ? us : 1;
		}
		udelay(10);	/* busy: cpu0 never reaches WFI */
	}

	for (i = 0; i < ARRAY_SIZE(snap_offs); i++)
		c->post[i] = rd(snap_offs[i]);
	for (i = 0; pwrap && i < ARRAY_SIZE(pw_offs); i++)
		c->pwpost[i] = readl(pwrap + pw_offs[i]);
	/* __spm_get_wakeup_status() */
	c->ws[0] = rd(PCM_REG_DATA_INI);	/* assert PC */
	c->ws[1] = rd(SPM_SW_RSV_0);		/* r12 wake reason */
	c->ws[2] = rd(PCM_REG12_EXT_DATA);
	c->ws[3] = rd(SPM_WAKEUP_STA);
	c->ws[4] = rd(SPM_WAKEUP_EXT_STA);
	c->ws[5] = rd(SPM_BSI_D0_SR);		/* wake misc backup */
	c->ws[6] = rd(SPM_BSI_D1_SR);		/* timer_out backup */
	c->ws[7] = rd(PCM_REG13_DATA);
	c->ws[8] = rd(SUBSYS_IDLE_STA);
	c->ws[9] = rd(SPM_SW_DEBUG);
	c->ws[10] = rd(SPM_BSI_D2_SR);		/* event_reg backup */
	c->ws[11] = rd(SPM_IRQ_STA);
	c->ws[12] = rd(SPM_WAKEUP_MISC);
	c->ws[13] = rd(PCM_EVENT_REG_STA);
	c->ws[14] = rd(PCM_TIMER_OUT);
	c->ws[15] = rd(PCM_CON1);
	if (stage >= 3) {
		/* __spm_clean_after_wakeup() (IRQ_MASK restored by the caller) */
		wr(SPM_CPU_WAKEUP_EVENT, 0);
		wr(SPM_WAKEUP_EVENT_MASK, ~0U);
		wr(SPM_IRQ_STA, ISRS_PCM_RETURN);	/* W1C */
		wr(SPM_SW_INT_CLEAR, 0x3ff);
		/* PCM timer off again (vendor __spm_reset_and_init_pcm clears TIMER_EN) */
		wr(PCM_CON1, KEY | (rd(PCM_CON1) & 0xffff & ~PCM_TIMER_EN));
	}
	c->reset_ret = pcm_reset();	/* stop the PCM (vendor reset) */
}

static long kick_work(void *arg)
{
	preempt_disable();	/* stay on cpu0, never schedule (no idle WFI behind our back) */
	kick_on_cpu0(arg);
	preempt_enable();
	return 0;
}

static void kick_test(const struct pcm_tail *t)
{
	static struct kick_ctx c;	/* big; module init is single-threaded */
	u32 sv_apsb, sv_wfi[10], sv_flag, sv_rsv0, sv_mp1, sv_mp2, sv_ini, sv_vec[16], sv_wmask, sv_cwe;
	u32 sv_pw[32], sv_irqm, sv_tval, sv_wdtv, sv_bsi[3];
	u32 i;

	if (watch_us > 100000)
		watch_us = 100000;
	memset(&c, 0, sizeof(c));

	sv_apsb = rd(AP_STANDBY_CON);
	sv_irqm = rd(SPM_IRQ_MASK);
	sv_tval = rd(PCM_TIMER_VAL);
	sv_wdtv = rd(PCM_WDT_VAL_OFF);	/* the firmware's wake path writes it (2e4: 0x000f0000) */
	for (i = 0; i < 3; i++)
		sv_bsi[i] = rd(SPM_BSI_D0_SR + 4 * i);
	sv_wmask = rd(SPM_WAKEUP_EVENT_MASK);
	sv_cwe = rd(SPM_CPU_WAKEUP_EVENT);
	for (i = 0; i < 10; i++)
		sv_wfi[i] = rd(MP0_CPU0_WFI_EN + 4 * i);
	sv_flag = rd(SPM_SW_FLAG);
	sv_rsv0 = rd(SPM_SW_RSV_0);
	sv_mp1 = rd(SPM_MAS_PAUSE_MASK_B);
	sv_mp2 = rd(SPM_MAS_PAUSE2_MASK_B);
	sv_ini = rd(PCM_REG_DATA_INI);
	for (i = 0; i < 16; i++)
		sv_vec[i] = rd(PCM_EVENT_VECTOR0 + 4 * i);

	/* __spm_init_pcm_register(): r0 <- POWER_ON_VAL0, r7 <- POWER_ON_VAL1 (enables stay 0) */
	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL0));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R0);
	wr(PCM_PWR_IO_EN, 0);
	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL1));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	wr(PCM_PWR_IO_EN, 0);
	/* __spm_init_event_vector() */
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, t->vec[i]);
	/* standby condition: WFI_OP_AND + mp3top masked (vendor suspend), MP0/1/2 + mcusys required */
	c.swflag = flags;
	if (stage >= 4) {
		/* standby = cpu0 WFI only (see header); SPM ISR like vendor __spm_set_wakeup_event */
		wr(AP_STANDBY_CON, (sv_apsb & ~0x7fU) | 0x7f);
		for (i = 0; i < 10; i++)
			wr(MP0_CPU0_WFI_EN + 4 * i, i == 0);
		wr(SPM_IRQ_MASK, (sv_irqm & ISRM_TWAM) | ISRM_RET_IRQ_AUX);
		c.swflag |= SPM_FLAG_DIS_SRCCLKEN_LOW;
	} else {
		wr(AP_STANDBY_CON, (sv_apsb & ~0x7fU) | BIT(0) | BIT(4));
		for (i = 0; i < 10; i++)	/* MP0 c0-3, MP1 c0-3, MP2 c0-1 (vendor suspend: all 1) */
			wr(MP0_CPU0_WFI_EN + 4 * i, 1);
	}

	if (stage >= 3) {
		/*
		 * The SPM's PMIC command table is empty (all adr 0 / wdata 0) and
		 * its pwrap channel (DVFSINF) is enabled: any SPM PMIC command would
		 * write 0 to MT6351 STRUP_CON0 (THR_HWPDN_EN etc.). Point every
		 * entry at the read-only HWCID (0x0200) for the run.
		 */
		for (i = 0; i < 32; i++)
			sv_pw[i] = readl(pwrap + PWRAP_DVFS_ADR0 + 4 * i);
		for (i = 0; i < 16; i++) {
			writel(0x0200, pwrap + PWRAP_DVFS_ADR0 + 8 * i);
			writel(0, pwrap + PWRAP_DVFS_ADR0 + 8 * i + 4);
		}
		wr(SPM_WAKEUP_EVENT_MASK, ~wake_unmask);
	}
	pr_info("mt6797-spm-load: stage %d: kicking PCM on cpu0 for %u us, SW_FLAG %08x, AP_STANDBY_CON %08x, WAKEUP_EVENT_MASK %08x, IRQ_MASK %08x%s\n",
		stage, watch_us, c.swflag, rd(AP_STANDBY_CON), rd(SPM_WAKEUP_EVENT_MASK), rd(SPM_IRQ_MASK),
		stage == 3 ? ", CPU wake event at wake_at_us" :
		stage == 4 ? ", CPU wake event at kick, one WFI at 100 us" :
		stage == 5 ? ", PCM timer wake (timer_ticks), one WFI at 100 us" : "");
	if (stage >= 4)
		work_on_cpu(0, kick_work, &c);	/* process context: WFI loop needs IRQs on between tries */
	else
		smp_call_function_single(0, kick_on_cpu0, &c, 1);

	/* restore everything stage 2 wrote (CON0/CON1/IM_* are restored by rmmod) */
	wr(PCM_PWR_IO_EN, 0);
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, sv_vec[i]);
	wr(PCM_REG_DATA_INI, sv_ini);
	wr(SPM_MAS_PAUSE_MASK_B, sv_mp1);
	wr(SPM_MAS_PAUSE2_MASK_B, sv_mp2);
	wr(SPM_SW_RSV_0, sv_rsv0);
	wr(SPM_SW_FLAG, sv_flag);
	for (i = 0; i < 10; i++)
		wr(MP0_CPU0_WFI_EN + 4 * i, sv_wfi[i]);
	wr(AP_STANDBY_CON, sv_apsb);
	wr(SPM_CPU_WAKEUP_EVENT, sv_cwe);
	wr(SPM_WAKEUP_EVENT_MASK, sv_wmask);
	wr(SPM_IRQ_MASK, sv_irqm);
	wr(PCM_TIMER_VAL, sv_tval);
	wr(PCM_WDT_VAL_OFF, sv_wdtv);	/* value only; PCM WDT_EN stays 0 throughout */
	for (i = 0; i < 3; i++)
		wr(SPM_BSI_D0_SR + 4 * i, sv_bsi[i]);
	if (stage >= 3)
		for (i = 0; i < 32; i++)
			writel(sv_pw[i], pwrap + PWRAP_DVFS_ADR0 + 4 * i);

	if (c.cpu != 0) {
		pr_err("mt6797-spm-load: stage 2: ran on cpu%d, not cpu0 — PCM NOT kicked\n", c.cpu);
		return;
	}
	pr_info("mt6797-spm-load: stage %d: %u samples, %d changes, wake event at %u us, PCM reset after run: %s\n",
		stage, c.loops, c.ntr, c.wake_t, c.reset_ret ? "FAILED" : "ok");
	if (stage >= 4) {
		pr_info("mt6797-spm-load: WFI %s: %d tries, %llu ticks in WFI @ %llu Hz (~%llu us)\n",
			c.wfi_done ? "done" : "NOT done", c.wfi_tries, c.wfi_ticks, c.cntfrq,
			c.cntfrq ? div64_u64(c.wfi_ticks * 1000000, c.cntfrq) : 0);
		pr_info("mt6797-spm-load:   before WFI: PC %08x FSM %08x SW_DEBUG %08x IRQ_STA %08x\n",
			c.pre_wfi.pc, c.pre_wfi.fsm, c.pre_wfi.dbg, c.pre_wfi.isr);
		pr_info("mt6797-spm-load:   after  WFI: PC %08x FSM %08x SW_DEBUG %08x IRQ_STA %08x\n",
			c.post_wfi.pc, c.post_wfi.fsm, c.post_wfi.dbg, c.post_wfi.isr);
	}
	for (i = 0; i < c.ntr; i++)
		pr_info("mt6797-spm-load:   t=%6u us PC %08x FSM %08x SW_DEBUG %08x IRQ_STA %08x\n",
			c.tr[i].t_us, c.tr[i].pc, c.tr[i].fsm, c.tr[i].dbg, c.tr[i].isr);
	pr_info("mt6797-spm-load: wake status: assert_pc %08x r12(RSV_0) %08x r12_ext %08x raw_sta %08x raw_ext %08x wake_misc(BSI_D0) %08x timer_out(BSI_D1) %08x\n",
		c.ws[0], c.ws[1], c.ws[2], c.ws[3], c.ws[4], c.ws[5], c.ws[6]);
	pr_info("mt6797-spm-load: wake status: r13 %08x idle_sta %08x SW_DEBUG %08x event_reg(BSI_D2) %08x IRQ_STA %08x WAKEUP_MISC %08x EVENT_REG_STA %08x\n",
		c.ws[7], c.ws[8], c.ws[9], c.ws[10], c.ws[11], c.ws[12], c.ws[13]);
	pr_info("mt6797-spm-load: wake status: PCM_TIMER_OUT %08x PCM_CON1 %08x (before clean)\n",
		c.ws[14], c.ws[15]);
	pr_info("mt6797-spm-load: registers changed while the PCM ran (pre -> post):\n");
	for (i = 0; i < ARRAY_SIZE(snap_offs); i++)
		if (c.pre[i] != c.post[i])
			pr_info("mt6797-spm-load:   @%03x %08x -> %08x\n",
				snap_offs[i], c.pre[i], c.post[i]);
	for (i = 0; pwrap && i < ARRAY_SIZE(pw_offs); i++)
		if (c.pwpre[i] != c.pwpost[i])
			pr_info("mt6797-spm-load:   pwrap @%03x %08x -> %08x\n",
				pw_offs[i], c.pwpre[i], c.pwpost[i]);
	show_pcm(stage == 5 ? "after stage 5" : stage == 4 ? "after stage 4" :
		 stage == 3 ? "after stage 3" : "after stage 2");
}

/* ------------------------------------------------------------------ stage 6 */
/*
 * First real sleep: the PCM's r7 (and optionally r0) outputs drive hardware.
 * See the header. Everything not listed there is as in stage 5.
 */
#define PCM_REG0_DATA		0x100
#define PCM_REG7_DATA		0x11C
#define PCM_PWRIO_EN_R0		BIT(0)
#define PCM_PWRIO_EN_R7		BIT(7)
#define SPM_CLK_CON		0x00C
#define SPM_CLK_SETTLE		0x010
#define MP2_CPUSYS_PWR_CON	0x218
#define CPU_EXT_BUCK_ISO	0x290

#define MCUCFG_PHYS		0x10220000
#define MP0_AXI_CONFIG		0x02C
#define MP1_AXI_CONFIG		0x22C
#define MP2_AXI_CONFIG		0x220C	/* MCUCFG2 0x10222000 + 0x20C (mt_spm_sleep.c); dpidle's 0x1022020C read 0a0a090a = not it (3a) */
#define MCUCFG_MAP_LEN		0x3000
#define ACINACTM		BIT(4)
#define ACINACTM_MP2		0x11
#define SMC_MCUSYS_WRITE	0x82000201	/* MTK_SIP_KERNEL_MCUSYS_WRITE (phys, val) */

#define GICD_PHYS		0x19000000
#define GICD_IPRIORITYR		0x400
#define SPM_IRQ_SPI		180		/* vendor DT sleep@10006000, level-low */
#define SPM_IRQ_PRIO		0x80		/* NS view; Linux default 0xa0 */
#define SLEEP_PMR		0x90		/* passes 0x80, blocks 0xa0 */

static const u32 axi_off[3] = { MP0_AXI_CONFIG, MP1_AXI_CONFIG, MP2_AXI_CONFIG };
static const u32 axi_bits[3] = { ACINACTM, ACINACTM, ACINACTM_MP2 };

static unsigned long mcusys_smc_write(u32 off, u32 val)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_MCUSYS_WRITE, MCUCFG_PHYS + off, val, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static int im_load(const struct pcm_tail *t, u32 im_ptr, u32 words)
{
	u32 con0, con1, i;
	int ret = pcm_reset();

	if (ret)
		return ret;
	wr(PCM_CON0, KEY | PCM_CK_EN | EN_IM_SLEEP_DVS);
	con1 = saved_con1 & (PCM_WDT_EN | PCM_WDT_WAKE_MODE | SRCCLKEN_FAST_RESP);
	wr(PCM_CON1, con1 | KEY | EVENT_LOCK_EN | SPM_SRAM_ISOINT_B | SPM_SRAM_SLEEP_B |
		     (t->replace ? 0 : IM_NONRP_EN) | MIF_APBEN | SCP_APB_INTERNAL_EN);
	wr(PCM_IM_PTR, im_ptr);
	wr(PCM_IM_LEN, words - 1);
	con0 = rd(PCM_CON0) & 0xffff & ~(IM_KICK_L | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN | IM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN);
	for (i = 0; i < 10000; i++) {
		if (rd(PCM_FSM_STA) & FSM_IM_REDY)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

struct sleep_res {
	int ran, refused, returned, abort_reset;
	u32 exits, r0_pre, r7_pre, r0_ret, r7_ret, pc_ret, fsm_ret, val0_pre;
	u32 ws[16];
	u64 ticks;		/* CNTVCT across the WFI loop */
	u32 axi_pre[3], axi_set[3];
	unsigned long smc_ret[3];
	u32 pmr_old;
};

struct sleep_ctx {
	const struct pcm_tail *t;
	u32 swflag;
	struct sleep_res r;
};

static irqreturn_t spm_isr(int irq, void *dev)
{
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);	/* W1C: TWAM, PCM_RETURN, RET_IRQ0-9 */
	wr(SPM_SW_INT_CLEAR, 0x3ff);			/* IRQ_STA bit4 (SW_INT0) seen at return in 2e4 */
	return IRQ_HANDLED;
}

static void __iomem *mcucfg;

static long sleep_work(void *arg)
{
	struct sleep_ctx *c = arg;
	struct sleep_res *r = &c->r;
	u32 con0, i, isr;
	u64 a, b, limit;

	preempt_disable();
	if (smp_processor_id() != 0 || num_online_cpus() != 1) {
		r->refused = 1;
		preempt_enable();
		return 0;
	}
	local_irq_disable();
	r->ran = 1;

	/* __spm_init_pcm_register(): r0 <- POWER_ON_VAL0, r7 <- POWER_ON_VAL1 */
	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL0));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R0);
	wr(PCM_PWR_IO_EN, 0);
	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL1));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	wr(PCM_PWR_IO_EN, 0);
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, c->t->vec[i]);
	/* power control as stage 5: standby = cpu0 WFI only */
	wr(AP_STANDBY_CON, (rd(AP_STANDBY_CON) & ~0x7fU) | 0x7f);
	for (i = 0; i < 10; i++)
		wr(MP0_CPU0_WFI_EN + 4 * i, i == 0);
	/* __spm_set_wakeup_event(): PCM timer; SPM ISR: only PCM_RETURN */
	wr(PCM_TIMER_VAL, sleep_ms * 32768 / 1000);
	wr(PCM_CON1, (rd(PCM_CON1) & 0xffff) | KEY | PCM_TIMER_EN);
	wr(SPM_WAKEUP_EVENT_MASK, ~wake_unmask);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	wr(SPM_SW_INT_CLEAR, 0x3ff);
	wr(SPM_IRQ_MASK, ISRM_TWAM | ISRM_RET_IRQ_AUX);
	/* __spm_kick_pcm_to_run() */
	wr(SPM_MAS_PAUSE_MASK_B, 0xffffffff);
	wr(SPM_MAS_PAUSE2_MASK_B, 0xffffffff);
	wr(PCM_REG_DATA_INI, 0);
	wr(SPM_SW_FLAG, c->swflag);
	wr(SPM_SW_RSV_0, 0);
	wr(SPM_SW_DEBUG, 0);	/* 3e: firmware step bits (SPM_DBG_DEBUG_IDX_*) per cycle, not cumulative */

	/* outputs may only be enabled while they equal the power-on values */
	r->r0_pre = rd(PCM_REG0_DATA);
	r->r7_pre = rd(PCM_REG7_DATA);
	if (r->r0_pre != rd(SPM_POWER_ON_VAL0) || r->r7_pre != rd(SPM_POWER_ON_VAL1)) {
		r->refused = 2;
		goto out;
	}
	/* ACINACTM (vendor: right before the WFI; done before the kick so a failed write never sleeps) */
	for (i = 0; i < 3; i++) {
		if (!(acinactm & BIT(i)))
			continue;
		r->axi_pre[i] = readl(mcucfg + axi_off[i]);
		r->smc_ret[i] = mcusys_smc_write(axi_off[i], r->axi_pre[i] | axi_bits[i]);
		r->axi_set[i] = readl(mcucfg + axi_off[i]);
		if ((r->axi_set[i] & axi_bits[i]) != axi_bits[i])
			r->refused = 3;
	}
	if (r->refused)
		goto out_axi;

	wr(PCM_PWR_IO_EN, io_en);
	con0 = rd(PCM_CON0) & 0xffff & ~(IM_KICK_L | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN);

	/* WFI until PCM_RETURN: with PMR 0x90 only the SPM IRQ (prio 0x80) ends it */
	r->pmr_old = gic_read_pmr();
	gic_write_pmr(SLEEP_PMR);
	isb();
	dsb(sy);
	a = read_sysreg(cntvct_el0);
	limit = (u64)(sleep_ms + 2000) * read_sysreg(cntfrq_el0) / 1000;
	for (r->exits = 0; ; r->exits++) {
		dsb(sy);
		wfi();
		if (rd(SPM_IRQ_STA) & ISRS_PCM_RETURN) {
			r->returned = 1;
			break;
		}
		if (read_sysreg(cntvct_el0) - a > limit)
			break;	/* only reached if WFI keeps returning without PCM_RETURN */
	}
	b = read_sysreg(cntvct_el0);
	r->ticks = b - a;
	gic_write_pmr(r->pmr_old);
	isb();

	if (!r->returned) {
		/* abort: outputs back to POWER_ON_VAL0/1 first, then the reset below */
		wr(PCM_PWR_IO_EN, 0);
		r->abort_reset = 1;
	}
	r->r0_ret = rd(PCM_REG0_DATA);
	r->r7_ret = rd(PCM_REG7_DATA);
	/*
	 * 3c: after PCM_RETURN r0 = 0xe8000000 (DPY_VREF_CH0_EN, DPY_DLL_EN,
	 * DPY_2ND_DLL_EN, DPY_DLL_CK_CH0_EN). With r0 driven, dropping
	 * PWR_IO_EN would switch those DRAM-PHY lines back to POWER_ON_VAL0 (0).
	 * Vendor __spm_reset_and_init_pcm: "backup mem control from r0 to
	 * POWER_ON_VAL0" before PWR_IO_EN = 0 — do the same, and keep it.
	 */
	r->val0_pre = rd(SPM_POWER_ON_VAL0);
	if (r->returned && (io_en & PCM_PWRIO_EN_R0) && r->r0_ret != r->val0_pre)
		wr(SPM_POWER_ON_VAL0, r->r0_ret);
	r->pc_ret = rd(PCM_REG15_DATA);
	r->fsm_ret = rd(PCM_FSM_STA);
	/* __spm_get_wakeup_status() */
	r->ws[0] = rd(PCM_REG_DATA_INI);
	r->ws[1] = rd(SPM_SW_RSV_0);
	r->ws[2] = rd(PCM_REG12_EXT_DATA);
	r->ws[3] = rd(SPM_WAKEUP_STA);
	r->ws[4] = rd(SPM_WAKEUP_EXT_STA);
	r->ws[5] = rd(SPM_BSI_D0_SR);
	r->ws[6] = rd(SPM_BSI_D1_SR);
	r->ws[7] = rd(PCM_REG13_DATA);
	r->ws[8] = rd(SUBSYS_IDLE_STA);
	r->ws[9] = rd(SPM_SW_DEBUG);
	r->ws[10] = rd(SPM_BSI_D2_SR);
	isr = rd(SPM_IRQ_STA);
	r->ws[11] = isr;
	r->ws[12] = rd(SPM_WAKEUP_MISC);
	r->ws[13] = rd(PCM_EVENT_REG_STA);
	r->ws[14] = rd(PCM_TIMER_OUT);
	r->ws[15] = rd(PCM_CON1);

out_axi:
	for (i = 0; i < 3; i++)
		if (acinactm & BIT(i) && r->axi_pre[i] != r->axi_set[i])
			mcusys_smc_write(axi_off[i], r->axi_pre[i]);
out:
	/* __spm_clean_after_wakeup() + PCM timer off, outputs off, PCM reset */
	wr(SPM_CPU_WAKEUP_EVENT, 0);
	wr(SPM_WAKEUP_EVENT_MASK, ~0U);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	wr(SPM_SW_INT_CLEAR, 0x3ff);
	wr(PCM_CON1, KEY | (rd(PCM_CON1) & 0xffff & ~PCM_TIMER_EN));
	wr(PCM_PWR_IO_EN, 0);
	pcm_reset();
	local_irq_enable();
	preempt_enable();
	return 0;
}

static void sleep_test(const struct pcm_tail *t, u32 im_ptr, u32 words)
{
	static struct sleep_ctx c;
	static struct sleep_res res[60];
	u32 sv_apsb, sv_wfi[10], sv_flag, sv_rsv0, sv_mp1, sv_mp2, sv_ini, sv_vec[16], sv_wmask, sv_cwe;
	u32 sv_pw[32], sv_irqm, sv_tval, sv_wdtv, sv_bsi[3], sv_dbg, sv_src, sv_src2;
	struct device_node *np;
	struct of_phandle_args oa = { .args_count = 3, .args = { 0, SPM_IRQ_SPI, IRQ_TYPE_LEVEL_LOW } };
	void __iomem *gicd;
	unsigned int irq = 0;
	u8 sv_prio = 0;
	u64 tot_ticks = 0, frq = read_sysreg(cntfrq_el0);
	u32 tot_timer = 0, n, i, ok = 0;
	int ret;

	if (io_en != 0x80 && io_en != 0x81) {
		pr_err("mt6797-spm-load: stage 6: io_en must be 0x80 or 0x81\n");
		return;
	}
	if (acinactm & ~3U) {
		pr_err("mt6797-spm-load: stage 6: acinactm bit2 (MP2) not allowed until its register is confirmed\n");
		return;
	}
	if ((io_en & PCM_PWRIO_EN_R0) && !(acinactm & 1)) {
		pr_err("mt6797-spm-load: stage 6: r0 (CPU clock off) needs acinactm (at least bit0 = MP0)\n");
		return;
	}
	if (num_online_cpus() != 1 || !cpu_online(0)) {
		pr_err("mt6797-spm-load: stage 6: offline cpu1-9 first (%u online)\n", num_online_cpus());
		return;
	}
	if (sleep_ms < 1 || sleep_ms > 5000 || cycles < 1 || cycles > 60 || gap_ms < 50) {
		pr_err("mt6797-spm-load: stage 6: sleep_ms 1..5000, cycles 1..60, gap_ms >= 50\n");
		return;
	}
	if (gic_read_pmr() <= SPM_IRQ_PRIO + 0x20) {
		pr_err("mt6797-spm-load: stage 6: ICC_PMR_EL1 = %02x (expected Linux default 0xf0) — refusing\n", gic_read_pmr());
		return;
	}
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-sysirq");
	if (!np) {
		pr_err("mt6797-spm-load: stage 6: no mediatek,mt6797-sysirq node\n");
		return;
	}
	oa.np = np;
	irq = irq_create_of_mapping(&oa);
	of_node_put(np);
	gicd = ioremap(GICD_PHYS, 0x1000);
	mcucfg = ioremap(MCUCFG_PHYS, MCUCFG_MAP_LEN);
	if (!irq || !gicd || !mcucfg) {
		pr_err("mt6797-spm-load: stage 6: irq %u / map failed\n", irq);
		goto out_unmap;
	}
	ret = request_irq(irq, spm_isr, IRQF_NO_AUTOEN, "mt6797-spm", NULL);
	if (ret) {
		pr_err("mt6797-spm-load: stage 6: request_irq(%u) = %d\n", irq, ret);
		irq_dispose_mapping(irq);
		irq = 0;
		goto out_unmap;
	}

	sv_apsb = rd(AP_STANDBY_CON);
	sv_irqm = rd(SPM_IRQ_MASK);
	sv_tval = rd(PCM_TIMER_VAL);
	sv_wdtv = rd(PCM_WDT_VAL_OFF);
	for (i = 0; i < 3; i++)
		sv_bsi[i] = rd(SPM_BSI_D0_SR + 4 * i);
	sv_wmask = rd(SPM_WAKEUP_EVENT_MASK);
	sv_cwe = rd(SPM_CPU_WAKEUP_EVENT);
	for (i = 0; i < 10; i++)
		sv_wfi[i] = rd(MP0_CPU0_WFI_EN + 4 * i);
	sv_flag = rd(SPM_SW_FLAG);
	sv_rsv0 = rd(SPM_SW_RSV_0);
	sv_dbg = rd(SPM_SW_DEBUG);
	sv_src = rd(SPM_SRC_MASK);
	sv_src2 = rd(SPM_SRC2_MASK);
	sv_mp1 = rd(SPM_MAS_PAUSE_MASK_B);
	sv_mp2 = rd(SPM_MAS_PAUSE2_MASK_B);
	sv_ini = rd(PCM_REG_DATA_INI);
	for (i = 0; i < 16; i++)
		sv_vec[i] = rd(PCM_EVENT_VECTOR0 + 4 * i);
	for (i = 0; i < 32; i++)
		sv_pw[i] = readl(pwrap + PWRAP_DVFS_ADR0 + 4 * i);
	for (i = 0; i < 16; i++) {	/* SPM PMIC commands -> read-only HWCID */
		writel(0x0200, pwrap + PWRAP_DVFS_ADR0 + 8 * i);
		writel(0, pwrap + PWRAP_DVFS_ADR0 + 8 * i + 4);
	}
	sv_prio = readb(gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	writeb(SPM_IRQ_PRIO, gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	if (src_mask != ~0U)
		wr(SPM_SRC_MASK, src_mask);
	if (src2_mask != ~0U)
		wr(SPM_SRC2_MASK, src2_mask);
	enable_irq(irq);

	c.t = t;
	c.swflag = flags | SPM_FLAG_DIS_SRCCLKEN_LOW;
	pr_info("mt6797-spm-load: stage 6: %u x %u ms sleep (gap %u ms), PWR_IO_EN %02x, SW_FLAG %08x, wake_unmask %08x, ACINACTM mask %x, SPM irq %u (GIC prio %02x -> %02x), PMR %02x -> %02x in WFI\n",
		cycles, sleep_ms, gap_ms, io_en, c.swflag, wake_unmask, acinactm, irq, sv_prio,
		readb(gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI), gic_read_pmr(), SLEEP_PMR);
	pr_info("mt6797-spm-load: stage 6: SRC_REQ %08x SRC_MASK %08x -> %08x SRC2_MASK %08x -> %08x\n",
		rd(SPM_SRC_REQ), sv_src, rd(SPM_SRC_MASK), sv_src2, rd(SPM_SRC2_MASK));

	for (n = 0; n < cycles; n++) {
		if (n && im_load(t, im_ptr, words)) {
			pr_err("mt6797-spm-load: cycle %u: IM reload failed — stopping\n", n);
			break;
		}
		memset(&c.r, 0, sizeof(c.r));
		work_on_cpu(0, sleep_work, &c);
		res[n] = c.r;
		/* one line per cycle right away (3d-3 hang: the end-of-run summary never got out) */
		pr_info("mt6797-spm-load: cyc %u/%u: %s r12 %08x timer_out %u (~%u ms) wfi %llu ms exits %u r0 %08x r7 %08x r13 %08x dbg %08x\n",
			n + 1, cycles, c.r.refused ? "REFUSED" : c.r.returned ? "ret" : "NO-RET",
			c.r.ws[1], c.r.ws[6], c.r.ws[6] * 1000 / 32768,
			frq ? div64_u64(c.r.ticks * 1000, frq) : 0, c.r.exits, c.r.r0_ret, c.r.r7_ret,
			c.r.ws[7], c.r.ws[9]);
		if (!c.r.ran || c.r.refused || !c.r.returned)
			break;
		ok++;
		tot_ticks += c.r.ticks;
		tot_timer += c.r.ws[6];
		msleep(gap_ms);
	}

	disable_irq(irq);
	free_irq(irq, NULL);
	irq_dispose_mapping(irq);
	writeb(sv_prio, gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	/* restore (CON0/CON1/IM_* by rmmod) */
	wr(PCM_PWR_IO_EN, 0);
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, sv_vec[i]);
	wr(PCM_REG_DATA_INI, sv_ini);
	wr(SPM_MAS_PAUSE_MASK_B, sv_mp1);
	wr(SPM_MAS_PAUSE2_MASK_B, sv_mp2);
	wr(SPM_SW_RSV_0, sv_rsv0);
	wr(SPM_SW_FLAG, sv_flag);
	wr(SPM_SW_DEBUG, sv_dbg);
	wr(SPM_SRC_MASK, sv_src);
	wr(SPM_SRC2_MASK, sv_src2);
	for (i = 0; i < 10; i++)
		wr(MP0_CPU0_WFI_EN + 4 * i, sv_wfi[i]);
	wr(AP_STANDBY_CON, sv_apsb);
	wr(SPM_CPU_WAKEUP_EVENT, sv_cwe);
	wr(SPM_WAKEUP_EVENT_MASK, sv_wmask);
	wr(SPM_IRQ_MASK, sv_irqm);
	wr(PCM_TIMER_VAL, sv_tval);
	wr(PCM_WDT_VAL_OFF, sv_wdtv);
	for (i = 0; i < 3; i++)
		wr(SPM_BSI_D0_SR + 4 * i, sv_bsi[i]);
	for (i = 0; i < 32; i++)
		writel(sv_pw[i], pwrap + PWRAP_DVFS_ADR0 + 4 * i);

	for (i = 0; i < cycles && i < n + 1 && i < ARRAY_SIZE(res); i++) {
		struct sleep_res *r = &res[i];

		if (!r->ran && !r->refused)
			break;
		pr_info("mt6797-spm-load: cycle %u: %s%s, %u WFI exits, %llu ticks (~%llu ms) in WFI loop, r0 %08x->%08x r7 %08x->%08x, PC %08x FSM %08x\n",
			i, r->refused ? "REFUSED " : r->returned ? "PCM_RETURN" : "NO RETURN — aborted",
			r->refused == 1 ? "(not alone on cpu0)" : r->refused == 2 ? "(r0/r7 != POWER_ON_VAL)" :
			r->refused == 3 ? "(ACINACTM write did not stick)" : "",
			r->exits, r->ticks, frq ? div64_u64(r->ticks * 1000, frq) : 0,
			r->r0_pre, r->r0_ret, r->r7_pre, r->r7_ret, r->pc_ret, r->fsm_ret);
		if (io_en & PCM_PWRIO_EN_R0)
			pr_info("mt6797-spm-load:   POWER_ON_VAL0 %08x -> %08x (r0 backup, kept)\n",
				r->val0_pre, rd(SPM_POWER_ON_VAL0));
		if (acinactm)
			pr_info("mt6797-spm-load:   ACINACTM: MP0 %08x->%08x (smc %ld) MP1 %08x->%08x (smc %ld) MP2 %08x->%08x (smc %ld)\n",
				r->axi_pre[0], r->axi_set[0], (long)r->smc_ret[0],
				r->axi_pre[1], r->axi_set[1], (long)r->smc_ret[1],
				r->axi_pre[2], r->axi_set[2], (long)r->smc_ret[2]);
		if (r->refused)
			continue;
		pr_info("mt6797-spm-load:   wake: r12 %08x r12_ext %08x raw %08x raw_ext %08x misc(D0) %08x timer_out(D1) %u (~%u ms) event(D2) %08x\n",
			r->ws[1], r->ws[2], r->ws[3], r->ws[4], r->ws[5], r->ws[6], r->ws[6] * 1000 / 32768, r->ws[10]);
		pr_info("mt6797-spm-load:   r13 %08x idle %08x SW_DEBUG %08x IRQ_STA %08x WAKEUP_MISC %08x EVENT_REG %08x TIMER_OUT %08x CON1 %08x assert_pc %08x\n",
			r->ws[7], r->ws[8], r->ws[9], r->ws[11], r->ws[12], r->ws[13], r->ws[14], r->ws[15], r->ws[0]);
	}
	pr_info("mt6797-spm-load: stage 6: %u of %u cycles returned; asleep (PCM timer_out) %u ms, cpu0 in WFI loop %llu ms\n",
		ok, cycles, (u32)div_u64((u64)tot_timer * 1000, 32768),
		frq ? div64_u64(tot_ticks * 1000, frq) : 0);
	show_pcm("after stage 6");
	goto out_unmap_only;

out_unmap:
	if (irq)
		irq_dispose_mapping(irq);
out_unmap_only:
	if (gicd)
		iounmap(gicd);
	if (mcucfg)
		iounmap(mcucfg);
	mcucfg = NULL;
}

/* stage 0 extras: read-only state for step 3 (+ optional SMC no-op test) */
static void stage0_extras(void)
{
	void __iomem *mc = ioremap(MCUCFG_PHYS, MCUCFG_MAP_LEN);
	void __iomem *gd = ioremap(GICD_PHYS, 0x1000);
	u32 i;

	pr_info("mt6797-spm-load: online cpus %*pbl; SPM_CLK_CON %08x CLK_SETTLE %08x MP2_CPUSYS_PWR_CON %08x CPU_EXT_BUCK_ISO %08x PWR_IO_EN %08x\n",
		cpumask_pr_args(cpu_online_mask), rd(SPM_CLK_CON), rd(SPM_CLK_SETTLE),
		rd(MP2_CPUSYS_PWR_CON), rd(CPU_EXT_BUCK_ISO), rd(PCM_PWR_IO_EN));
	pr_info("mt6797-spm-load: ICC_PMR_EL1 %02x (this cpu %d)\n", gic_read_pmr(), raw_smp_processor_id());
	if (gd)
		pr_info("mt6797-spm-load: GICD_CTLR %08x, prio SPI %u = %02x, SPI 169 (APXGPT) = %02x, GICD_ISENABLER(SPI %u word) %08x\n",
			readl(gd), SPM_IRQ_SPI, readb(gd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI),
			readb(gd + GICD_IPRIORITYR + 32 + 169), SPM_IRQ_SPI,
			readl(gd + 0x100 + 4 * ((32 + SPM_IRQ_SPI) / 32)));
	if (mc) {
		for (i = 0; i < 3; i++)
			pr_info("mt6797-spm-load: MP%u_AXI_CONFIG (0x%08x) %08x\n", i,
				MCUCFG_PHYS + axi_off[i], readl(mc + axi_off[i]));
		pr_info("mt6797-spm-load: 0x1022020c (dpidle's MP2 guess) %08x, MCUCFG2 0x10222000 %08x 0x1022200c %08x 0x1022202c %08x\n",
			readl(mc + 0x20C), readl(mc + 0x2000), readl(mc + 0x200C), readl(mc + 0x202C));
		if (axi_test)
			for (i = 0; i < 2; i++) {
				u32 v = readl(mc + axi_off[i]);
				unsigned long a0 = mcusys_smc_write(axi_off[i], v);

				pr_info("mt6797-spm-load: axi_test: SMC 0x%x(0x%08x, %08x) = %ld, readback %08x\n",
					SMC_MCUSYS_WRITE, MCUCFG_PHYS + axi_off[i], v, (long)a0,
					readl(mc + axi_off[i]));
			}
	}
	if (mc)
		iounmap(mc);
	if (gd)
		iounmap(gd);
}

static int __init spm_load_init(void)
{
	const struct firmware *f;
	const struct pcm_tail *t;
	void __iomem *infra;
	u32 words, im_ptr, fsm, con1, i;
	bool mode4g;
	char ver[64];
	int ret;

	if (stage < 0 || stage > 6) {
		pr_err("mt6797-spm-load: stage must be 0..6\n");
		return -EINVAL;
	}

	pdev = platform_device_register_simple("mt6797-spm-load", -1, NULL, 0);
	if (IS_ERR(pdev))
		return PTR_ERR(pdev);
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out_pdev;

	/* ---- parse ---- */
	ret = request_firmware_direct(&f, fw, &pdev->dev);
	if (ret) {
		pr_err("mt6797-spm-load: request_firmware_direct(%s) = %d\n", fw, ret);
		goto out_pdev;
	}
	ret = -EINVAL;
	if (f->size < 2)
		goto bad_fw;
	words = get_unaligned_le16(f->data);
	if (words < 16 || words > IM_MAX_WORDS ||
	    f->size < 2 + words * 4 + sizeof(*t) + 1)
		goto bad_fw;
	t = (const void *)(f->data + 2 + words * 4);
	if (t->size != words || t->sess > 2 || t->replace > 1)
		goto bad_fw;
	strscpy(ver, (const char *)(t + 1),
		min_t(size_t, sizeof(ver), f->size - (2 + words * 4 + sizeof(*t)) + 1));
	pr_info("mt6797-spm-load: %s: %u words, sess %u, replace %u, addr_2nd %u, version \"%s\"\n",
		fw, words, t->sess, t->replace, t->addr_2nd, ver);
	for (i = 0; i < 16; i++)
		if (t->vec[i])
			pr_info("mt6797-spm-load:   vec%u = %08x\n", i, t->vec[i]);

	/* ---- state (reads only) ---- */
	spm = ioremap(SPM_PHYS, 0x1000);
	infra = ioremap(INFRA_AO_PHYS, 0x1000);
	if (!spm || !infra) {
		ret = -ENOMEM;
		goto out_map;
	}
	mode4g = readl(infra + INFRA_MISC) & BIT(13);
	pr_info("mt6797-spm-load: INFRA_MISC %08x -> DRAM 4 GB mode %s\n",
		readl(infra + INFRA_MISC), mode4g ? "ON (IM_PTR = phys + 0x40000000)" : "off (IM_PTR = phys)");
	iounmap(infra);
	infra = NULL;
	pwrap = ioremap(PWRAP_PHYS, 0x1000);
	if (pwrap) {
		pr_info("mt6797-spm-load: pwrap HIPRIO_ARB_EN %08x (DVFSINF/SPM channel %s) HARB_STA0 %08x STA1 %08x SPMINF_STA %08x\n",
			readl(pwrap + PWRAP_HIPRIO_ARB_EN),
			readl(pwrap + PWRAP_HIPRIO_ARB_EN) & PWRAP_DVFSINF ? "ENABLED" : "off",
			readl(pwrap + PWRAP_HARB_STA0), readl(pwrap + PWRAP_HARB_STA1),
			readl(pwrap + PWRAP_SPMINF_STA));
		for (i = 0; i < 16; i++)
			pr_info("mt6797-spm-load:   pwrap DVFS cmd %2u: PMIC adr %04x wdata %04x\n", i,
				readl(pwrap + PWRAP_DVFS_ADR0 + 8 * i),
				readl(pwrap + PWRAP_DVFS_ADR0 + 8 * i + 4));
	}
	show_pcm("found");
	pr_info("mt6797-spm-load: POWERON_CONFIG_EN %08x\n", rd(POWERON_CONFIG_EN));

	fsm = rd(PCM_FSM_STA);
	if (((fsm & 0x7fffff) != FSM_DEF || rd(PCM_PWR_IO_EN) || (rd(POWERON_CONFIG_EN) & 1) == 0) && !force) {
		pr_err("mt6797-spm-load: PCM not in reset-default / PWR_IO_EN set / bus CG off — refusing (force=1 overrides)\n");
		ret = -EBUSY;
		goto out_map;
	}
	if (stage >= 3 && (!pwrap || !pwrap_safe)) {
		pr_err("mt6797-spm-load: stage 3/4 need the pwrap mapped and pwrap_safe=1 (empty SPM PMIC table = writes to MT6351 STRUP_CON0)\n");
		ret = -EPERM;
		goto out_map;
	}
	if (stage == 0) {
		stage0_extras();
		pr_info("mt6797-spm-load: stage 0 done, nothing written%s\n",
			axi_test ? " (except the SMC no-op AXI_CONFIG writes)" : "");
		ret = 0;
		goto out_keep;	/* stays loaded so rmmod is symmetrical; no writes */
	}

	/* ---- stage 1: load into IM, no kick ---- */
	ret = alloc_buf(&pdev->dev, f->data + 2, words * 4);
	if (ret)
		goto out_map;
	im_ptr = (u32)page_to_phys(page);
	if (mode4g)
		im_ptr += 0x40000000;	/* MAPPING_DRAM_ACCESS_ADDR */
	pr_info("mt6797-spm-load: buffer phys %pa, IM_PTR %08x, IM_LEN %u\n",
		&(phys_addr_t){ page_to_phys(page) }, im_ptr, words - 1);

	saved_con0 = rd(PCM_CON0);
	saved_con1 = rd(PCM_CON1);
	saved_ptr = rd(PCM_IM_PTR);
	saved_len = rd(PCM_IM_LEN);

	ret = pcm_reset();
	if (ret)
		goto restore;
	loaded = true;	/* from here on rmmod restores */

	wr(PCM_CON0, KEY | PCM_CK_EN | EN_IM_SLEEP_DVS);
	/* vendor CON1 init; keep WDT bits (found off) and SRCCLKEN_FAST_RESP as found */
	con1 = saved_con1 & (PCM_WDT_EN | PCM_WDT_WAKE_MODE | SRCCLKEN_FAST_RESP);
	wr(PCM_CON1, con1 | KEY | EVENT_LOCK_EN | SPM_SRAM_ISOINT_B | SPM_SRAM_SLEEP_B |
		     (t->replace ? 0 : IM_NONRP_EN) | MIF_APBEN | SCP_APB_INTERNAL_EN);
	wr(PCM_IM_PTR, im_ptr);
	wr(PCM_IM_LEN, words - 1);
	show_pcm("before IM_KICK");

	/* kick IM to fetch (only toggle IM_KICK) — never PCM_KICK */
	{
		u32 con0 = rd(PCM_CON0) & 0xffff & ~(IM_KICK_L | PCM_KICK_L);

		wr(PCM_CON0, con0 | KEY | PCM_CK_EN | IM_KICK_L);
		wr(PCM_CON0, con0 | KEY | PCM_CK_EN);
	}
	for (i = 0; i < 10000; i++) {
		if (rd(PCM_FSM_STA) & FSM_IM_REDY)
			break;
		udelay(1);
	}
	show_pcm(i < 10000 ? "IM ready" : "IM NOT ready after 10 ms");
	pr_info("mt6797-spm-load: fetch took ~%u us\n", i);

	{
		bool ready = rd(PCM_FSM_STA) & FSM_IM_REDY;
		int vret = 0;

		/* compare against our aligned copy (blob words sit at offset 2); always before a kick */
		if (verify || stage == 2 || stage == 6)
			vret = do_verify(page_address(page), words);
		if (stage >= 2) {
			if (ready && !vret && stage == 6)
				sleep_test(t, im_ptr, words);
			else if (ready && !vret)
				kick_test(t);
			else
				pr_err("mt6797-spm-load: stage %d: IM not ready or verify failed — NOT kicking\n", stage);
		}
	}
	pr_info("mt6797-spm-load: stage %d done. rmmod restores CON0/CON1/IM_PTR/IM_LEN.\n", stage);
	release_firmware(f);
	return 0;

restore:
	wr(PCM_CON0, KEY | (saved_con0 & 0xffff));
	dma_unmap_page(&pdev->dev, dma, dma_len, DMA_TO_DEVICE);
	__free_pages(page, page_order);
	page = NULL;
out_map:
	if (infra)
		iounmap(infra);
	if (spm)
		iounmap(spm);
	spm = NULL;
	if (pwrap)
		iounmap(pwrap);
	pwrap = NULL;
	release_firmware(f);
out_pdev:
	platform_device_unregister(pdev);
	return ret;
out_keep:
	release_firmware(f);
	return 0;
bad_fw:
	pr_err("mt6797-spm-load: %s: bad format (size %zu)\n", fw, f->size);
	release_firmware(f);
	platform_device_unregister(pdev);
	return ret;
}

static void __exit spm_load_exit(void)
{
	if (loaded) {
		pcm_reset();
		wr(PCM_IM_PTR, saved_ptr);
		wr(PCM_IM_LEN, saved_len);
		wr(PCM_CON1, KEY | (saved_con1 & 0xffff));
		wr(PCM_CON0, KEY | (saved_con0 & 0xffff));
		show_pcm("restored");
	}
	if (page) {
		dma_unmap_page(&pdev->dev, dma, dma_len, DMA_TO_DEVICE);
		__free_pages(page, page_order);
	}
	if (spm)
		iounmap(spm);
	if (pwrap)
		iounmap(pwrap);
	platform_device_unregister(pdev);
}

module_init(spm_load_init);
module_exit(spm_load_exit);
MODULE_DESCRIPTION("MT6797 SPM PCM firmware loader + suspend-firmware test stages (SPM port steps 2-3; Gemini PDA)");
MODULE_LICENSE("GPL");
