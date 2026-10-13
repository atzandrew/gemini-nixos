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
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/workqueue.h>
#include <asm/unaligned.h>
#include <asm/barrier.h>
#include <asm/sysreg.h>

static char *fw = "mt6797-spm/pcm_suspend.bin";
module_param(fw, charp, 0444);
MODULE_PARM_DESC(fw, "PCM firmware under /lib/firmware");

static int stage;
module_param(stage, int, 0444);
MODULE_PARM_DESC(stage, "0 parse only (no writes), 1 load into IM (no kick), 2 kick-and-park, 3 = 2 + CPU wake event, 4 = standby entry (one WFI) with wake pending, 5 = 4 with PCM timer wake");

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

static int __init spm_load_init(void)
{
	const struct firmware *f;
	const struct pcm_tail *t;
	void __iomem *infra;
	u32 words, im_ptr, fsm, con1, i;
	bool mode4g;
	char ver[64];
	int ret;

	if (stage < 0 || stage > 5) {
		pr_err("mt6797-spm-load: stage must be 0..5\n");
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
		pr_info("mt6797-spm-load: stage 0 done, nothing written\n");
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
		if (verify || stage == 2)
			vret = do_verify(page_address(page), words);
		if (stage >= 2) {
			if (ready && !vret)
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
MODULE_DESCRIPTION("MT6797 SPM PCM firmware load into IM, no kick (step 2a; Gemini PDA)");
MODULE_LICENSE("GPL");
