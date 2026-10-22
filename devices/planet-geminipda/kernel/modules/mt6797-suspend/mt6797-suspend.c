// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-suspend — system suspend ("deep", /sys/power/mem_sleep) for the
 * MT6797 Gemini PDA, as a loadable module (suspend_set_ops is exported).
 *
 * mode=0 (probe): .enter only busy-waits probe_ms. Tests the driver
 *   suspend/resume callbacks and secondary-CPU offline/online, no SPM.
 *
 * mode=1 (SPM sleep): the vendor suspend firmware (pcm_suspend.bin) on the
 *   SPM, the sequence proven by mt6797-spm-load stage 6 pdn=1
 *   (docs/spm-sleep.md): cpu0 alone, PSCI CPU_SUSPEND cluster power-down,
 *   PCM timer wake. There is no wake interrupt yet (pinctrl-mt6797 has no
 *   EINT), so .enter loops: sleep poll_ms, wake, check the PMIC keys
 *   (TOPSTATUS over the pwrap WACS2 port) and the lid (GPIO66), go back to
 *   sleep if nothing changed. Leaves .enter on a key press, the lid
 *   opening, a non-timer SPM wake reason, a failed cycle, or after max_s.
 *   The reason is left in the last_wake parameter. .prepare refuses while
 *   the display is still on (disp_check).
 *   Lid EINT (lid_eint, 2026-10-20): when the lid is shut at entry, the
 *   hall pin GPIO66 is switched to its EINT5 function (vendor DWS: HALL =
 *   EINT5) and EINT5 is set to fire on "lid open" and unmasked as an SPM
 *   wake source (R12 bit 6), so opening the lid wakes the SPM at once and
 *   the PCM timer only needs to be a slow safety net (lid_poll_ms). Nothing
 *   in Linux owns the EINT block (mainline pinctrl-mt6797 has no EINT) and
 *   its GIC line (SPI 170) stays disabled; everything is put back after.
 *   Caller's job (userspace hook for now): panel DCS 0x28/0x29, connsys
 *   off, cpu8/9 offline (CPU_ON(8) hangs after SPM sleep).
 *
 * Watchdog: mtk-wdt starts the TOPRGU WDT at boot (31 s, dual mode: IRQ,
 *   then reset at ~62 s) and the watchdog core feeds it; its suspend
 *   callback only stops it when userspace has /dev/watchdog open, so in
 *   suspend nothing feeds it and the chip resets ~1 min in (S2 2026-10-17).
 *   Feeding it every loop was not enough: with it running, SPM sleeps
 *   randomly ended in a WDT reset (= PMIC cold reset, WDTRSTB mode 0):
 *   2 of ~90 5 s cycles, 1 of ~570 2 s cycles; with it disabled, 0 of
 *   120 5 s cycles (2026-10-17). Stock disables it around SPM suspend
 *   (wd_suspend_notify), so mode 1 does too (wdt_pause, default on) and
 *   restores WDT_MODE after the loop. Proper fix later: mtk_wdt
 *   suspend/resume + the SPM's PCM WDT as the hang backstop.
 *   2026-10-17 re-read: those "WDT resets" may be the intermittent
 *   suspend hang (seen with VPROC2 on too) caught by the WDT. Hang
 *   hunting: wdt_pause=0 wdt_int_rst=1 + breadcrumbs in NONRST_REG2
 *   (crumbs, default on; bin/sleep-tests/hunt.sh).
 *   PCM WDT (pcm_wdt, 2026-10-20, as stock __spm_set_pcm_wdt): each SPM
 *   sleep also arms the SPM's own watchdog at PCM_TIMER_VAL + pcm_wdt s.
 *   If the sleep has not ended by then, the SPM asks TOPRGU for a reset
 *   (REQ_MODE SPM_SCPSYS, reset mode). WDT_MODE EXTEN is cleared for the
 *   loop so that reset is SoC-only and the breadcrumb survives.
 *   pcm_wdt_test makes it fire 2 s into each sleep (checks the reset path).
 *   The SoC reset wipes the SPM (checked 2026-10-20), so pcm_wdt_wake uses
 *   the WDT's wake mode instead, to catch the stuck state in the log.
 *
 * Keys: a short press falls between two polls, so the PMIC key interrupts
 *   are enabled for the suspend (INT_CON0 bits 0-3, as stock) and the
 *   latched INT_STATUS0 bits are read each cycle; nothing services the
 *   PMIC interrupt line itself (no EINT yet). INT_CON0 is restored after.
 *
 * Power domains: genpd powers a domain off in suspend_noirq once all of
 * its devices have suspended. "mm" must never go off here (LK programmed
 * the display pipeline and nothing re-initialises it), and audio/conn are
 * kept for now. .begin sets GENPD_FLAG_ALWAYS_ON on the domains of the
 * devices in keep_pd[] and .end clears what it set. (Kernel fix later:
 * MTK_SCPD_ALWAYS_ON for mm in mtk-scpsys.)
 *
 * Unload restores "no platform suspend" (s2idle only) and the PCM state.
 */
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/gfp.h>
#include <linux/math64.h>
#include <linux/cpu_pm.h>
#include <linux/psci.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <asm/unaligned.h>
#include <asm/barrier.h>
#include <asm/sysreg.h>
#include <asm/arch_gicv3.h>

#define TAG "mt6797-suspend: "

static uint mode = 1;
module_param(mode, uint, 0644);
MODULE_PARM_DESC(mode, "0 = probe (busy-wait, no SPM), 1 = SPM sleep (default)");

static uint probe_ms = 5000;
module_param(probe_ms, uint, 0644);
MODULE_PARM_DESC(probe_ms, "mode 0: time spent in .enter (ms)");

static uint poll_ms = 2000;
module_param(poll_ms, uint, 0644);
MODULE_PARM_DESC(poll_ms, "mode 1: SPM sleep per cycle (PCM timer, ms, 100..5000)");

static uint gap_ms = 200;
module_param(gap_ms, uint, 0644);
MODULE_PARM_DESC(gap_ms, "mode 1: awake busy-wait between cycles (ms)");

static uint max_s;
module_param(max_s, uint, 0644);
MODULE_PARM_DESC(max_s, "mode 1: leave .enter after this many seconds asleep (0 = never)");

static bool wake_keys = true;
module_param(wake_keys, bool, 0644);
MODULE_PARM_DESC(wake_keys, "mode 1: wake on Esc/On or the silver key");

static bool wake_lid = true;
module_param(wake_lid, bool, 0644);
MODULE_PARM_DESC(wake_lid, "mode 1: wake when the lid opens (only if it was closed at entry)");

static bool lid_eint;	/* off 2026-10-20: long sleeps hang, ~0.5-1.2 A armed */
module_param(lid_eint, bool, 0644);
MODULE_PARM_DESC(lid_eint, "mode 1: lid shut at entry -> lid opening wakes the SPM directly (EINT5), cycles of lid_poll_ms");

static uint lid_poll_ms = 60000;
module_param(lid_poll_ms, uint, 0644);
MODULE_PARM_DESC(lid_poll_ms, "mode 1, lid EINT armed: SPM sleep per cycle (ms, 100..600000); keys are only checked this often");

static bool touch_rst;
module_param(touch_rst, bool, 0644);
MODULE_PARM_DESC(touch_rst, "mode 1: hold the SSD2092 touch controller in reset (CTP_RST GPIO68 low) while asleep (power test)");
static bool wdt_pause = true;
module_param(wdt_pause, bool, 0644);
MODULE_PARM_DESC(wdt_pause, "mode 1: disable the TOPRGU WDT for the SPM sleep loop (as stock wd_suspend_notify); 0 = feed it each cycle only");
static bool crumbs = true;
module_param(crumbs, bool, 0644);
MODULE_PARM_DESC(crumbs, "write a progress breadcrumb to TOPRGU NONRST_REG2 (0x10007024) at each suspend step; printed and cleared at load (hang hunting)");
static bool wdt_int_rst;
module_param(wdt_int_rst, bool, 0644);
MODULE_PARM_DESC(wdt_int_rst, "mode 1, wdt_pause=0: clear WDT_MODE EXTEN for the loop, so a WDT reset resets the SoC only (no PMIC cold reset; keeps NONRST_REG2, maybe DRAM)");
static uint pcm_wdt;
module_param(pcm_wdt, uint, 0644);
MODULE_PARM_DESC(pcm_wdt, "mode 1: arm the SPM PCM watchdog each sleep, firing this many s after the PCM timer (stock: 30; 0 = off, max 600); a stuck sleep then resets the SoC");
static bool pcm_wdt_test;
module_param(pcm_wdt_test, bool, 0644);
static bool pcm_wdt_wake;
module_param(pcm_wdt_wake, bool, 0644);
MODULE_PARM_DESC(pcm_wdt_wake, "with pcm_wdt: the PCM watchdog wakes (R12 bit 13) instead of resetting, so a stuck sleep is logged (state dump); combine with wdt_pause=0 wdt_int_rst=1 as reset backstop");
MODULE_PARM_DESC(pcm_wdt_test, "with pcm_wdt: fire the PCM watchdog 2 s into each sleep (forced reset, tests the path; use cycles > 2 s)");

static bool vgpu_low;
module_param(vgpu_low, bool, 0644);
MODULE_PARM_DESC(vgpu_low, "mode 1: VGPU (RT5735 PROGVSEL0) to 843.75 mV for the suspend (power test); restored in .finish");
static bool vproc2_off;
module_param(vproc2_off, bool, 0644);
MODULE_PARM_DESC(vproc2_off, "mode 1: VPROC2 (DA9214 BUCKB) off if the A72 cluster is torn down (power test, ~43 mA; default OFF: suspends entered with it already off hung, 3 of 5); NOT restored, the next boot re-powers it");

static uint vproc2_sleep_mv = 300;
module_param(vproc2_sleep_mv, uint, 0644);
MODULE_PARM_DESC(vproc2_sleep_mv, "mode 1: VPROC2 (DA9214 BUCKB, rail kept on) to this many mV (300..1000) for the suspend if the A72 cluster is torn down (~35-40 mA, A/B 2026-10-18); restored in .finish; 0 = leave (default 300 since 2026-10-18)");

static uint ldo_lp;
module_param(ldo_lp, uint, 0644);
MODULE_PARM_DESC(ldo_lp, "mode 1: MT6351 LDOs in low-power mode while asleep, bitmask: 1 VA18, 2 VA10, 4 VUSB33, 8 VLDO28, 0x10 VIO28, 0x20 VIO18 (power test)");
static uint ldo_lp_mode = 1;
module_param(ldo_lp_mode, uint, 0644);
MODULE_PARM_DESC(ldo_lp_mode, "1 = SW LP forced around each SPM sleep, 2 = HW LP on SRCLKEN0 for the whole loop (stock SUSPEND setting)");

static uint pmic_misc;
module_param(pmic_misc, uint, 0644);
MODULE_PARM_DESC(pmic_misc, "mode 1: stock suspend-entry PMIC bits for the loop, bitmask: 1 OSC_SEL_HW_MODE, 2 VGPU buck low-IQ (power test)");

/*
 * DRAM self-refresh hunt (2026-10-22, docs/spm-sleep.md "Vcore DVS table"): stock
 * spm_go_to_sleep clears SPM_FLAG_DIS_VCORE_DVS and the PCM firmware then drives the
 * PMIC through the SUSPEND pwrap table (vendor 3.18 mt_spm_pmic_wrap.c MT6797 set +
 * spm_suspend_pre_process). DDREN never ran with bit 3 set; this loads that table.
 */
static uint dvs;
module_param(dvs, uint, 0644);
static u32 dvs_active;	/* dvs value in use for this suspend (0 = dummy table) */
MODULE_PARM_DESC(dvs, "mode 1: 1 = stock MT6797 SUSPEND pwrap table in the SPM DVFS slots + clear SPM_FLAG_DIS_VCORE_DVS (bit 3); 2 = also clear DIS_VPROC_VSRAM_DVS (bit 9). Vcore/VSRAM PMIC regs restored after each suspend (DRAM self-refresh test)");

/* stage-6 settings (docs/spm-sleep.md "Recommended settings") */
static uint flags = 0x5fc102ff;
module_param(flags, uint, 0644);
static uint src_mask = 0x10aa007e;
module_param(src_mask, uint, 0644);
static uint io_en = 0x81;
module_param(io_en, uint, 0644);
static uint wake_unmask = 0x1;
module_param(wake_unmask, uint, 0644);
static uint pdn_state = 0x01010000;
module_param(pdn_state, uint, 0644);
static uint pdn_apsb = 0x11;
module_param(pdn_apsb, uint, 0644);

static char *fw = "mt6797-spm/pcm_suspend.bin";
module_param(fw, charp, 0444);

/*
 * The prep hook turns the display off before the suspend, but a compositor
 * that starts or wakes in the meantime (2026-10-18: the SDDM greeter, 0.3 s
 * after the hook's check) re-enables OVL0 layer 0 and the backlight, and the
 * panel stays lit all night. .prepare runs with all tasks frozen, so nothing
 * can turn it back on after this check.
 */
static bool disp_check = true;
module_param(disp_check, bool, 0644);
MODULE_PARM_DESC(disp_check, "mode 1: refuse the suspend while OVL0 layer 0 (the display) is on");

/* Why the last suspend ended, for logs and userspace hooks. */
/*
 * LCD bias check (2026-10-21): never sleep with the panel biased (6 screen
 * burns; CLAUDE.md "Never").  geminipda-drm panel_suspend=1 powers the panel
 * off in its device suspend, which runs before .prepare; this reads the two
 * bias enables at the pad (DIN) and refuses the suspend if either is high.
 */
#define LCD_ENP_GPIO		60
#define LCD_ENN_GPIO		251
static bool bias_check = true;
module_param(bias_check, bool, 0644);
MODULE_PARM_DESC(bias_check, "mode 1: refuse the suspend while the LCD bias (ENP GPIO60 / ENN GPIO251) is on (default on)");

/*
 * Hang hunt 2026-10-21/22: stock suspend never sets SPM_FLAG_DIS_SRCCLKEN_LOW
 * (only the idle modes do). Forcing it (our old default) caused the random
 * sleep hang (~1 per 20-25 min asleep, PSCI suspend -> PCM WDT reset). Left
 * out: 46 x 10 min, 0 hangs, same ~175 mA (docs/spm-sleep.md).
 * Y = leave it out, as stock; N = force it (old behaviour, for tests only).
 */
static bool srcclken_low = true;
module_param(srcclken_low, bool, 0644);
MODULE_PARM_DESC(srcclken_low, "mode 1: do not force SPM_FLAG_DIS_SRCCLKEN_LOW (stock suspend; default Y; N = old forced, hangs)");
static char bias_at_sleep[16] = "unknown";
module_param_string(bias_at_sleep, bias_at_sleep, sizeof(bias_at_sleep), 0444);
MODULE_PARM_DESC(bias_at_sleep, "LCD bias enables read in the last .prepare: \"ENP x ENN y\"");

static char last_wake[24] = "none";
module_param_string(last_wake, last_wake, sizeof(last_wake), 0444);
MODULE_PARM_DESC(last_wake, "why the last suspend ended: max_s, key, lid opened, SPM wake reason, cycle failed, pmic read failed, display on, bias on");

/* ---------------------------------------------------------------- registers */
#define SPM_PHYS		0x10006000
#define INFRA_AO_PHYS		0x10001000
#define INFRA_MISC		0xf00		/* bit13 = DRAM 4 GB mode */
#define PWRAP_PHYS		0x1000d000
#define PWRAP_WACS2_CMD		0x0a0
#define PWRAP_WACS2_RDATA	0x0a4
#define PWRAP_WACS2_VLDCLR	0x0a8
#define WACS_FSM(x)		(((x) >> 16) & 7)
#define WACS_FSM_IDLE		0
#define WACS_FSM_WFVLDCLR	6
#define PWRAP_DVFS_ADR0		0x10c		/* ADRn = 0x10c + 8n, WDATAn = +4, n = 0..15 */
#define MT6351_INT_CON0		0x2c2
#define MT6351_INT_STATUS0	0x2e0		/* bit0 PWRKEY, bit1 HOMEKEY, bit2/3 their releases; W1C */
#define KEY_INTS		0xf
#define WDT_PHYS		0x10007000
#define WDT_RESTART		0x08
#define WDT_RESTART_KEY		0x1971
#define WDT_MODE		0x00
#define WDT_MODE_KEY		0x22000000
#define WDT_MODE_EN		BIT(0)
#define WDT_MODE_EXTEN		BIT(2)	/* reset also via WDTRSTB -> PMIC cold reset */
#define WDT_NONRST2		0x24	/* survives WDT resets; unused by LK/preloader (bit15 of NONRST = DL flag) */
#define WDT_STA			0x0c		/* reset cause: bit31 HW WDT, bit30 SW, bit1 SPM WDT */
#define WDT_REQ_MODE		0x30
#define WDT_REQ_MODE_KEY	0x33000000
#define WDT_REQ_IRQ_EN		0x34
#define WDT_REQ_IRQ_KEY		0x44000000
#define WDT_REQ_SPM_SCPSYS	BIT(1)		/* SPM (PCM WDT) reset request: REQ_MODE on, IRQ_EN off = reset */
#define MT6351_TOPSTATUS	0x220		/* bit1 PWRKEY_DEB, bit2 HOMEKEY_DEB, active low */
#define KEYS_MASK		(BIT(1) | BIT(2))
#define GPIO_PHYS		0x10005000
#define OVL0_PHYS		0x1400b000
#define OVL_SRC_CON		0x02c		/* bit0 = layer 0 enable (geminipda-drm CRTC on) */
#define GPIO_DIN(pin)		(0x200 + 0x10 * ((pin) / 32))
#define LID_GPIO		66		/* hall, low = lid closed */
#define GPIO_DOUT(pin)		(0x100 + 0x10 * ((pin) / 32))
#define CTP_RST_GPIO		68		/* touch reset, low = reset; output, high while running */

#define POWERON_CONFIG_EN	0x000
#define SPM_POWER_ON_VAL0	0x004
#define SPM_POWER_ON_VAL1	0x008
#define SPM_CLK_CON		0x00C
#define SPM_CLK_SETTLE		0x010
#define AP_STANDBY_CON		0x014
#define PCM_CON0		0x018
#define PCM_CON1		0x01C
#define PCM_IM_PTR		0x020
#define PCM_IM_LEN		0x024
#define PCM_REG_DATA_INI	0x028
#define PCM_PWR_IO_EN		0x02C
#define PCM_TIMER_VAL		0x030
#define PCM_WDT_VAL_OFF		0x034
#define PCM_EVENT_VECTOR0	0x040
#define SPM_SW_INT_CLEAR	0x094
#define SPM_CPU_WAKEUP_EVENT	0x0B0
#define SPM_IRQ_MASK		0x0B4
#define SPM_SRC_REQ		0x0B8
#define SPM_SRC_MASK		0x0BC
#define SPM_SRC2_MASK		0x0C0
#define SPM_WAKEUP_EVENT_MASK	0x0C4
#define PCM_REG0_DATA		0x100
#define PCM_REG7_DATA		0x11C
#define PCM_REG13_DATA		0x134
#define PCM_REG15_DATA		0x13C
#define PCM_TIMER_OUT		0x150
#define SPM_IRQ_STA		0x158
#define SPM_WAKEUP_STA		0x15C
#define PCM_FSM_STA		0x17C
#define SPM_PWR_STATUS		0x180
#define SPM_PWR_STATUS_2ND	0x184
#define SPM_MAS_PAUSE_MASK_B	0x408
#define SPM_MAS_PAUSE2_MASK_B	0x40C
#define SPM_BSI_D0_SR		0x41C
#define SPM_BSI_D1_SR		0x420
#define SPM_BSI_D2_SR		0x424
#define MP0_CPU0_WFI_EN		0x540
#define SPM_SW_FLAG		0x600
#define SPM_SW_DEBUG		0x604
#define SPM_SW_RSV_0		0x608

#define KEY			(0xb16U << 16)	/* SPM_REGWR_CFG_KEY */
#define PCM_KICK_L		BIT(0)
#define IM_KICK_L		BIT(1)
#define PCM_CK_EN		BIT(2)
#define EN_IM_SLEEP_DVS		BIT(3)
#define PCM_SW_RESET		BIT(15)
#define MIF_APBEN		BIT(3)
#define PCM_TIMER_EN		BIT(5)
#define IM_NONRP_EN		BIT(6)
#define PCM_WDT_EN		BIT(8)
#define PCM_WDT_WAKE_MODE	BIT(9)
#define SPM_SRAM_SLEEP_B	BIT(10)
#define SPM_SRAM_ISOINT_B	BIT(11)
#define EVENT_LOCK_EN		BIT(12)
#define SRCCLKEN_FAST_RESP	BIT(13)
#define SCP_APB_INTERNAL_EN	BIT(14)
#define FSM_DEF			0x48490
#define FSM_IM_REDY		BIT(9)
#define ISRM_TWAM		BIT(2)
#define ISRM_RET_IRQ_AUX	(0x3ffU << 8)
#define ISRS_PCM_RETURN		BIT(3)
#define PCM_RF_SYNC_R0		BIT(16)
#define PCM_RF_SYNC_R7		BIT(23)
#define PCM_PWRIO_EN_R0		BIT(0)
#define SPM_FLAG_DIS_CPU_PDN	BIT(0)
#define SPM_FLAG_DIS_SRCCLKEN_LOW BIT(15)
#define SYS_SETTLE_SEL		BIT(4)
#define SPM_LOCK_INFRA_DCM	BIT(5)
#define CLK_CON_VENDOR_INIT	0x0801104C
#define SPM_SYSCLK_SETTLE	99		/* 32 kHz ticks = 3 ms */
#define R12_PCM_TIMER		BIT(0)
#define R12_EINT_EVENT_B	BIT(6)		/* pcm_def_mt6797.h */
#define R12_PCM_WDT_WAKEUP_B	BIT(13)
#define PCM_WDT_OUT		0x154

/* EINT controller (vendor "mediatek,mt-eic"; same layout as mainline mtk-eint) */
#define EINT_PHYS		0x1000b000
#define EINT_STA		0x000
#define EINT_ACK		0x040
#define EINT_MASK		0x080
#define EINT_MASK_SET		0x0c0
#define EINT_MASK_CLR		0x100
#define EINT_SENS		0x140		/* 1 = level */
#define EINT_SENS_SET		0x180
#define EINT_SENS_CLR		0x1c0
#define EINT_POL		0x300
#define EINT_POL_SET		0x340		/* 1 = active high */
#define EINT_POL_CLR		0x380
#define EINT_D0EN		0x400		/* route to the AP domain */
#define LID_EINT		5		/* GPIO66 mode 1 */
#define GPIO_MODE66		0x380		/* GPIO64..71, 4 bits each */
#define GPIO_MODE66_SHIFT	8

#define GICD_PHYS		0x19000000
#define GICD_IPRIORITYR		0x400
#define SPM_IRQ_SPI		180		/* vendor DT sleep@10006000, level-low */
#define SPM_IRQ_PRIO		0x80
#define SLEEP_PMR		0x90		/* passes 0x80, blocks 0xa0 */

#define IM_MAX_WORDS		2048

struct pcm_tail {			/* struct pcm_desc from .size on */
	u16 size;
	u8 sess;
	u8 replace;
	u16 addr_2nd;
	u16 reserved;
	u32 vec[16];
} __packed;

static void __iomem *spm, *pwrap, *gpio, *gicd, *wdt, *ovl, *eint;
static struct platform_device *pdev;
static struct page *page;
static unsigned int page_order;
static dma_addr_t dma;
static size_t dma_len;
static bool loaded;

/*
 * Firmware buffer integrity (2026-10-22): every sleep hang so far had the module
 * loaded at boot, where the buffer always lands at phys 0x41c02000, 8 KB past the
 * connsys EMI window (0x41a00000 + 2 MB). Keep a pristine copy and compare it with
 * what is really in DRAM (cache invalidated first): before and after every
 * suspend, and on demand (echo 1 > im_check_now).
 */
static void *im_copy;
static bool im_fix;
module_param(im_fix, bool, 0644);
MODULE_PARM_DESC(im_fix, "rewrite the SPM firmware buffer from the pristine copy if it was found changed");
static uint im_bad;
module_param(im_bad, uint, 0444);
MODULE_PARM_DESC(im_bad, "number of checks that found the SPM firmware buffer changed");

static void im_verify(const char *when)
{
	const u32 *a = im_copy, *b;
	u32 i, n = 0, words = dma_len / 4;

	if (!im_copy || !page)
		return;
	dma_sync_single_for_cpu(&pdev->dev, dma, dma_len, DMA_BIDIRECTIONAL);	/* invalidate */
	b = page_address(page);
	for (i = 0; i < words; i++) {
		if (a[i] == b[i])
			continue;
		if (n < 8)
			pr_warn(TAG "IM check (%s): word %u (+0x%x) %08x -> %08x\n",
				when, i, i * 4, a[i], b[i]);
		n++;
	}
	if (!n) {
		pr_info(TAG "IM check (%s): %u words OK at %pa\n", when, words, &dma);
		return;
	}
	im_bad++;
	pr_warn(TAG "IM check (%s): %u of %u words CHANGED at %pa%s\n", when, n, words, &dma,
		im_fix ? " - rewritten" : "");
	if (im_fix) {
		memcpy(page_address(page), im_copy, dma_len);
		dma_sync_single_for_device(&pdev->dev, dma, dma_len, DMA_BIDIRECTIONAL);
	}
}

static int im_check_now_set(const char *val, const struct kernel_param *kp)
{
	im_verify("manual");
	return 0;
}
static const struct kernel_param_ops im_check_now_ops = { .set = im_check_now_set };
module_param_cb(im_check_now, &im_check_now_ops, NULL, 0200);
MODULE_PARM_DESC(im_check_now, "write anything: compare the SPM firmware buffer with the pristine copy now");
static u32 saved_con0, saved_con1, saved_ptr, saved_len;
static struct pcm_tail tail;
static u32 im_ptr, im_words;
static unsigned int spm_irq;

static inline u32 rd(u32 off) { return readl(spm + off); }
static inline void wr(u32 off, u32 v) { writel(v, spm + off); }

/* ------------------------------------------------------------ power domains */
static const char * const keep_pd[] = {
	"geminipda-drm",		/* mm */
	"11220000.audio-controller",	/* audio */
	"18070000.consys",		/* conn */
};
static struct generic_pm_domain *pinned[ARRAY_SIZE(keep_pd)];

static void pin_domains(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(keep_pd); i++) {
		struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL, keep_pd[i]);
		struct generic_pm_domain *pd;

		pinned[i] = NULL;
		if (!dev) {
			pr_warn(TAG "%s not found\n", keep_pd[i]);
			continue;
		}
		if (dev->pm_domain) {
			pd = pd_to_genpd(dev->pm_domain);
			if (!(pd->flags & GENPD_FLAG_ALWAYS_ON)) {
				pd->flags |= GENPD_FLAG_ALWAYS_ON;
				pinned[i] = pd;
			}
		}
		put_device(dev);
	}
}

static void unpin_domains(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pinned); i++) {
		if (pinned[i])
			pinned[i]->flags &= ~GENPD_FLAG_ALWAYS_ON;
		pinned[i] = NULL;
	}
}

/* ------------------------------------------------------- external bucks */
/*
 * Power tests (2026-10-17). Both bucks sit on I2C, which is only usable
 * until suspend_noirq, so this runs in .prepare through the drivers' own
 * regmaps (keeps their caches right, handles the DA9214 page register).
 *
 * VGPU: lowered, not switched off: genpd powers the MFG domains back on
 * in resume_noirq, before .finish could restore the rail. 843.75 mV is
 * the lowest GPU OPP voltage, safe for that power-on.
 *
 * VPROC2 (249 -> 206 mA in suspend, A/B 2026-10-17; default OFF since 3 of
 * 5 suspends that STARTED with the rail already off hung): once
 * both A72s are offline ATF tears the cluster down
 * (EXT_BUCK_ISO 0x10006290 bit1 set, 0x10006218 bit0 clear) and the rail
 * can be dropped (vendor cpu_power_off_buck, cl2-down.sh: reg 0x5e = 0).
 * Turning it back on needs the cl2-power latch sequence, so it stays off;
 * cpu8/9 never come back after an SPM sleep anyway.
 */
#define RT5735_VSEL0		0x11
#define RT5735_EN		BIT(7)
#define VGPU_LOW_SEL		0x27		/* 600 mV + 39 * 6.25 mV = 843.75 mV */
#define DA9211_BUCKB_CONT	0x5e
#define DA9214_VBUCKB_A		0xd9	/* [7] SL_A, [6:0] 300 mV + 10 mV * n */
#define SPM_CL2_218		0x218
#define SPM_EXT_BUCK_ISO	0x290

static struct regmap *vgpu_map;
static unsigned int vgpu_saved;
static struct regmap *vproc2_map;
static unsigned int vproc2_saved;

static struct regmap *i2c_regmap(const char *name)
{
	struct device *dev = bus_find_device_by_name(&i2c_bus_type, NULL, name);
	struct regmap *map;

	if (!dev)
		return NULL;
	map = dev_get_regmap(dev, NULL);
	put_device(dev);
	return map;
}

static void bucks_down(void)
{
	if (vgpu_low) {
		struct device *gdev = bus_find_device_by_name(&platform_bus_type, NULL, "13040000.mali");
		bool gpu_off = gdev && pm_runtime_status_suspended(gdev);

		if (gdev)
			put_device(gdev);
		vgpu_map = i2c_regmap("3-001c");
		if (!gpu_off || !vgpu_map || regmap_read(vgpu_map, RT5735_VSEL0, &vgpu_saved) ||
		    !(vgpu_saved & RT5735_EN)) {
			pr_warn(TAG "vgpu_low skipped (gpu %s, map %d, VSEL0 %02x)\n",
				gpu_off ? "suspended" : "ACTIVE", !!vgpu_map, vgpu_saved);
			vgpu_map = NULL;
		} else if (regmap_write(vgpu_map, RT5735_VSEL0, RT5735_EN | VGPU_LOW_SEL)) {
			pr_warn(TAG "vgpu_low: write failed\n");
			vgpu_map = NULL;
		} else {
			pr_info(TAG "VGPU VSEL0 %02x -> %02x\n", vgpu_saved, RT5735_EN | VGPU_LOW_SEL);
		}
	}
	/*
	 * VPROC2 low voltage, rail kept on (no rail-off/re-power path): the sleep saving is the
	 * voltage, not phases or mode (e-series 2026-10-18: 1 phase, auto 1-phase: 0 mA;
	 * 0.6 V + 1 phase -33 mA; 0.3 V + 1 phase -40 mA). Same writes as da9214-test.
	 */
	if (vproc2_sleep_mv && !vproc2_off) {
		u32 iso = rd(SPM_EXT_BUCK_ISO), con = rd(SPM_CL2_218);
		unsigned int sel = (vproc2_sleep_mv - 300) / 10;

		vproc2_map = i2c_regmap("2-0068");
		if (vproc2_sleep_mv < 300 || vproc2_sleep_mv > 1000 || cpu_online(8) || cpu_online(9) ||
		    !(iso & BIT(1)) || (con & BIT(0)) || !vproc2_map ||
		    regmap_read(vproc2_map, DA9214_VBUCKB_A, &vproc2_saved)) {
			pr_warn(TAG "vproc2_sleep_mv skipped (%u mV, cpu8/9 %d%d, ISO %08x, 218 %08x, map %d)\n",
				vproc2_sleep_mv, cpu_online(8), cpu_online(9), iso, con, !!vproc2_map);
			vproc2_map = NULL;
		} else if (regmap_update_bits(vproc2_map, DA9214_VBUCKB_A, 0x7f, sel)) {
			pr_warn(TAG "vproc2_sleep_mv: write failed\n");
			regmap_write(vproc2_map, DA9214_VBUCKB_A, vproc2_saved);
			vproc2_map = NULL;
		} else {
			pr_info(TAG "VPROC2 VBUCKB_A %02x -> %02x (%u mV)\n", vproc2_saved,
				(vproc2_saved & 0x80) | sel, 300 + sel * 10);
		}
	}
	if (vproc2_off) {
		u32 iso = rd(SPM_EXT_BUCK_ISO), con = rd(SPM_CL2_218);
		struct regmap *map = i2c_regmap("2-0068");
		int ret;

		if (cpu_online(8) || cpu_online(9) || !(iso & BIT(1)) || (con & BIT(0)) || !map) {
			pr_warn(TAG "vproc2_off skipped (cpu8/9 %d%d, ISO %08x, 218 %08x, map %d)\n",
				cpu_online(8), cpu_online(9), iso, con, !!map);
		} else {
			/* write only: reads on this bus are unreliable (shared with the SCP) */
			ret = regmap_write(map, DA9211_BUCKB_CONT, 0);
			pr_info(TAG "VPROC2 (BUCKB) off: %d\n", ret);
		}
	}
}

static void bucks_up(void)
{
	if (vproc2_map) {
		/* cluster stays off (cpu8/9 never come back after SPM): no settle needed */
		int ret = regmap_write(vproc2_map, DA9214_VBUCKB_A, vproc2_saved);

		pr_info(TAG "VPROC2 VBUCKB_A restored %02x: %d\n", vproc2_saved, ret);
		vproc2_map = NULL;
	}
	if (!vgpu_map)
		return;
	regmap_write(vgpu_map, RT5735_VSEL0, vgpu_saved);
	udelay(400);	/* ramp + vendor 350 us settle */
	vgpu_map = NULL;
}

/* ------------------------------------------------------------- wake checks */
/* One PMIC register read on the AP's WACS2 port (mtk-pmic-wrap pwrap_read16, no ARB cap). */
static int pmic_read(u32 adr, u32 *val)
{
	int i;

	for (i = 0; i < 10000 && WACS_FSM(readl(pwrap + PWRAP_WACS2_RDATA)) != WACS_FSM_IDLE; i++)
		udelay(1);
	if (i == 10000)
		return -ETIMEDOUT;
	writel((adr >> 1) << 16, pwrap + PWRAP_WACS2_CMD);
	for (i = 0; i < 10000; i++) {
		u32 v = readl(pwrap + PWRAP_WACS2_RDATA);

		if (WACS_FSM(v) == WACS_FSM_WFVLDCLR) {
			*val = v & 0xffff;
			writel(1, pwrap + PWRAP_WACS2_VLDCLR);
			return 0;
		}
		udelay(1);
	}
	return -ETIMEDOUT;
}

static int pmic_write(u32 adr, u32 val)
{
	int i;

	for (i = 0; i < 10000 && WACS_FSM(readl(pwrap + PWRAP_WACS2_RDATA)) != WACS_FSM_IDLE; i++)
		udelay(1);
	if (i == 10000)
		return -ETIMEDOUT;
	writel(BIT(31) | ((adr >> 1) << 16) | (val & 0xffff), pwrap + PWRAP_WACS2_CMD);
	return 0;
}

static void wdt_feed(void)
{
	writel(WDT_RESTART_KEY, wdt + WDT_RESTART);
}

/*
 * Breadcrumbs (hang hunting, 2026-10-17): 0xB s ss nnnn in NONRST_REG2 =
 * step s, suspend number ss since load, cycle nnnn. Read back so the
 * write has landed before the next step can hang.
 */
enum { CR_PREPARE = 1, CR_LOOP, CR_SETUP, CR_KICKED, CR_PSCI, CR_PSCI_RET, CR_CYC_DONE,
       CR_CHECKED, CR_LOOP_EXIT, CR_FINISH, CR_FINISH_DONE, CR_END };
static const char *const cr_txt[16] = { "?", "prepare", "loop top", "SPM setup", "PCM kicked",
	"PSCI suspend", "PSCI returned", "cycle done", "wake checks done", "loop exit",
	"finish", "finish done", "end" };
static u32 cr_susp, cr_n;

static void crumb(u32 step)
{
	if (!crumbs || !wdt)
		return;
	writel(0xB0000000 | (step & 0xf) << 24 | (cr_susp & 0xff) << 16 | (cr_n & 0xffff),
	       wdt + WDT_NONRST2);
	readl(wdt + WDT_NONRST2);
}

static bool lid_closed(void)
{
	return !(readl(gpio + GPIO_DIN(LID_GPIO)) & BIT(LID_GPIO % 32));
}

/*
 * Lid EINT, armed only for a suspend that starts with the lid shut. Called
 * from .enter (cpu0 alone, IRQs off), so plain read-modify-writes are safe.
 * DIN still reads the lid with the pin in mode 1 (checked 2026-10-20).
 */
static struct { u32 mode, d0en; bool pol, sens, masked; } lid_sv;

static void lid_eint_arm(void)
{
	u32 m = readl(gpio + GPIO_MODE66), b = BIT(LID_EINT);

	lid_sv.mode = m;
	lid_sv.d0en = readl(eint + EINT_D0EN);
	lid_sv.pol = readl(eint + EINT_POL) & b;
	lid_sv.sens = readl(eint + EINT_SENS) & b;
	lid_sv.masked = readl(eint + EINT_MASK) & b;

	writel(b, eint + EINT_MASK_SET);
	m &= ~(0xfU << GPIO_MODE66_SHIFT);
	writel(m | (1U << GPIO_MODE66_SHIFT), gpio + GPIO_MODE66);
	writel(b, eint + EINT_SENS_SET);	/* level: stays set while the lid is open */
	writel(b, eint + EINT_POL_SET);		/* active high = lid open */
	writel(lid_sv.d0en | b, eint + EINT_D0EN);
	writel(b, eint + EINT_ACK);
	writel(b, eint + EINT_MASK_CLR);
}

static void lid_eint_disarm(void)
{
	u32 b = BIT(LID_EINT);

	writel(b, eint + EINT_MASK_SET);
	writel(b, eint + EINT_ACK);
	writel(b, lid_sv.pol ? eint + EINT_POL_SET : eint + EINT_POL_CLR);
	if (!lid_sv.sens)
		writel(b, eint + EINT_SENS_CLR);
	writel(lid_sv.d0en, eint + EINT_D0EN);
	writel(lid_sv.mode, gpio + GPIO_MODE66);
	if (!lid_sv.masked)
		writel(b, eint + EINT_MASK_CLR);
}

/* ------------------------------------------------------------ PMIC LDOs */
/*
 * Power test (2026-10-17). Stock (mt_spm.c spm_pmic_power_mode SUSPEND)
 * puts these LDOs in hardware low-power mode, entered when SRCLKEN0 drops
 * in sleep; mainline leaves them in normal mode. LDO CON0 (upmu_hw.h):
 * b0 MODE_SET (SW LP), b2 MODE_CTRL (1 = HW LP), b7:5 SRCLK_MODE_SEL
 * (0 = SRCLKEN0), b8 QI_MODE (status: 1 = in LP). ldo_lp_mode 1 forces
 * SW LP around each SPM sleep only (the awake gaps run in normal mode);
 * mode 2 sets the stock HW mode for the loop. Saved values are restored
 * on the way out. Plain WACS2 accesses: one CPU, IRQs off in .enter.
 */
#define LDO_MODE_SET		BIT(0)
#define LDO_MODE_CTRL		BIT(2)
#define LDO_SRCLK_MODE_SEL	(7 << 5)
#define LDO_QI_MODE		BIT(8)

static const struct { const char *name; u16 adr; } lp_ldo[] = {
	{ "VA18", 0x0a00 }, { "VA10", 0x0a6e }, { "VUSB33", 0x0a16 },
	{ "VLDO28", 0x0a9c }, { "VIO28", 0x0a34 }, { "VIO18", 0x0a4c },
};
static u32 ldo_saved[ARRAY_SIZE(lp_ldo)];
static bool ldo_lp_on;

static bool ldo_lp_save(void)
{
	int i;

	ldo_lp_on = false;
	if (!ldo_lp || (ldo_lp_mode != 1 && ldo_lp_mode != 2))
		return false;
	for (i = 0; i < ARRAY_SIZE(lp_ldo); i++)
		if ((ldo_lp & BIT(i)) && pmic_read(lp_ldo[i].adr, &ldo_saved[i]))
			return false;
	ldo_lp_on = true;
	return true;
}

static void ldo_lp_set(bool lp)
{
	int i;

	if (!ldo_lp_on)
		return;
	for (i = 0; i < ARRAY_SIZE(lp_ldo); i++) {
		u32 v = ldo_saved[i];

		if (!(ldo_lp & BIT(i)))
			continue;
		if (lp && ldo_lp_mode == 1)
			v = (v & ~LDO_MODE_CTRL) | LDO_MODE_SET;
		else if (lp)
			v = (v & ~LDO_SRCLK_MODE_SEL) | LDO_MODE_CTRL;
		pmic_write(lp_ldo[i].adr, v);
	}
}

static void ldo_lp_log(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(lp_ldo); i++) {
		u32 v = 0;

		if (!(ldo_lp & BIT(i)))
			continue;
		pmic_read(lp_ldo[i].adr, &v);
		pr_info(TAG "LDO %s %04x: %04x -> %04x (%s)\n", lp_ldo[i].name, lp_ldo[i].adr,
			ldo_saved[i], v, (v & LDO_QI_MODE) ? "in LP" : "normal");
	}
}

/*
 * Stock suspend entry (mt_spm_sleep.c spm_suspend_pre_process, set while
 * still awake): __spm_pmic_low_iq_mode(1) = VGPU_ANA_CON7 bit1, and
 * spm_pmic_power_mode(SUSPEND) -> spm_pmic_set_osc_mode(HW) = TOP_CON
 * bit6. Set before the loop, saved values restored after it.
 */
static const struct { const char *name; u16 adr; u16 bit; } misc_bit[] = {
	{ "OSC_SEL_HW_MODE", 0x0204, BIT(6) }, { "VGPU_LOWIQ", 0x0470, BIT(1) },
};
static u32 misc_saved[ARRAY_SIZE(misc_bit)];
static u32 misc_done;

static void pmic_misc_set(void)
{
	int i;

	misc_done = 0;
	for (i = 0; i < ARRAY_SIZE(misc_bit); i++) {
		u32 v;

		if (!(pmic_misc & BIT(i)) || pmic_read(misc_bit[i].adr, &misc_saved[i]))
			continue;
		pmic_write(misc_bit[i].adr, misc_saved[i] | misc_bit[i].bit);
		misc_done |= BIT(i);
		v = 0;
		pmic_read(misc_bit[i].adr, &v);
		pr_info(TAG "%s %04x: %04x -> %04x\n", misc_bit[i].name, misc_bit[i].adr, misc_saved[i], v);
	}
}

static void pmic_misc_restore(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(misc_bit); i++)
		if (misc_done & BIT(i))
			pmic_write(misc_bit[i].adr, misc_saved[i]);
	misc_done = 0;
}

/* ------------------------------------------------------------------- PCM */
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
	return i == 5000 ? -EIO : 0;
}

static int im_load(void)
{
	u32 con0, con1, i;
	int ret = pcm_reset();

	if (ret)
		return ret;
	wr(PCM_CON0, KEY | PCM_CK_EN | EN_IM_SLEEP_DVS);
	con1 = saved_con1 & (PCM_WDT_EN | PCM_WDT_WAKE_MODE | SRCCLKEN_FAST_RESP);
	wr(PCM_CON1, con1 | KEY | EVENT_LOCK_EN | SPM_SRAM_ISOINT_B | SPM_SRAM_SLEEP_B |
		     (tail.replace ? 0 : IM_NONRP_EN) | MIF_APBEN | SCP_APB_INTERNAL_EN);
	wr(PCM_IM_PTR, im_ptr);
	wr(PCM_IM_LEN, im_words - 1);
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

static irqreturn_t spm_isr(int irq, void *dev)
{
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	wr(SPM_SW_INT_CLEAR, 0x3ff);
	return IRQ_HANDLED;
}

struct cyc {
	int refused, returned, pm_ret, psci_ret;
	u32 r12, timer_out, r13, dbg;
	u32 r15, fsm, pwr, pwr2, wsta, tout, wout, irqsta;	/* raw state before the PCM reset */
};

/*
 * One SPM sleep (mt6797-spm-load sleep_work, pdn=1, no ACINACTM). Called
 * from .enter: cpu0 alone, IRQs off.
 */
static void spm_sleep_once(struct cyc *r, u32 ms, u32 wmask)
{
	u32 con0, i, swflag, pmr_old;
	u64 cval;
	u32 ctl;

	memset(r, 0, sizeof(*r));
	crumb(CR_SETUP);
	swflag = (flags | (srcclken_low ? 0 : SPM_FLAG_DIS_SRCCLKEN_LOW)) & ~SPM_FLAG_DIS_CPU_PDN;
	if (dvs_active & 1)
		swflag &= ~BIT(3);	/* SPM_FLAG_DIS_VCORE_DVS */
	if (dvs_active & 2)
		swflag &= ~BIT(9);	/* SPM_FLAG_DIS_VPROC_VSRAM_DVS */

	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL0));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R0);
	wr(PCM_PWR_IO_EN, 0);
	wr(PCM_REG_DATA_INI, rd(SPM_POWER_ON_VAL1));
	wr(PCM_PWR_IO_EN, PCM_RF_SYNC_R7);
	wr(PCM_PWR_IO_EN, 0);
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, tail.vec[i]);
	wr(AP_STANDBY_CON, (rd(AP_STANDBY_CON) & ~0x7fU) | (pdn_apsb & 0x7f));
	for (i = 0; i < 10; i++)
		wr(MP0_CPU0_WFI_EN + 4 * i, i == 0);
	wr(PCM_TIMER_VAL, (u32)div_u64((u64)ms * 32768, 1000));
	wr(PCM_CON1, (rd(PCM_CON1) & 0xffff) | KEY | PCM_TIMER_EN);
	if (pcm_wdt) {	/* stock __spm_set_pcm_wdt(1): normal (reset) mode, counts from the kick */
		u32 c1 = rd(PCM_CON1) & 0xffff & ~(PCM_WDT_WAKE_MODE | PCM_WDT_EN);

		wr(PCM_CON1, KEY | c1);
		wr(PCM_WDT_VAL_OFF, pcm_wdt_test ? 2 * 32768 :
		   rd(PCM_TIMER_VAL) + min(pcm_wdt, 600U) * 32768);
		wr(PCM_CON1, KEY | c1 | PCM_WDT_EN | (pcm_wdt_wake ? PCM_WDT_WAKE_MODE : 0));
	}
	wr(SPM_WAKEUP_EVENT_MASK, ~wmask);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	wr(SPM_SW_INT_CLEAR, 0x3ff);
	wr(SPM_IRQ_MASK, ISRM_TWAM | ISRM_RET_IRQ_AUX);
	wr(SPM_MAS_PAUSE_MASK_B, 0xffffffff);
	wr(SPM_MAS_PAUSE2_MASK_B, 0xffffffff);
	wr(PCM_REG_DATA_INI, 0);
	wr(SPM_SW_FLAG, swflag);
	wr(SPM_SW_RSV_0, 0);
	wr(SPM_SW_DEBUG, 0);

	if (rd(PCM_REG0_DATA) != rd(SPM_POWER_ON_VAL0) || rd(PCM_REG7_DATA) != rd(SPM_POWER_ON_VAL1)) {
		r->refused = 1;
		goto out;
	}

	wr(PCM_PWR_IO_EN, io_en);
	con0 = rd(PCM_CON0) & 0xffff & ~(IM_KICK_L | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN | PCM_KICK_L);
	wr(PCM_CON0, con0 | KEY | PCM_CK_EN);
	crumb(CR_KICKED);

	pmr_old = gic_read_pmr();
	gic_write_pmr(SLEEP_PMR);
	isb();
	dsb(sy);
	cval = read_sysreg(cntp_cval_el0);
	ctl = read_sysreg(cntp_ctl_el0);
	r->pm_ret = cpu_pm_enter();
	if (!r->pm_ret) {
		crumb(CR_PSCI);
		r->psci_ret = psci_cpu_suspend_enter(pdn_state);
		crumb(CR_PSCI_RET);
		cpu_pm_exit();
		write_sysreg(cval, cntp_cval_el0);
		write_sysreg(ctl, cntp_ctl_el0);
		isb();
	}
	if (rd(SPM_IRQ_STA) & ISRS_PCM_RETURN)
		r->returned = 1;
	gic_write_pmr(pmr_old);
	isb();

	if (!r->returned)
		wr(PCM_PWR_IO_EN, 0);	/* abort: outputs back to POWER_ON_VAL0/1 */
	if (r->returned && (io_en & PCM_PWRIO_EN_R0) && rd(PCM_REG0_DATA) != rd(SPM_POWER_ON_VAL0))
		wr(SPM_POWER_ON_VAL0, rd(PCM_REG0_DATA));	/* vendor r0 -> POWER_ON_VAL0 backup */
	r->r12 = rd(SPM_SW_RSV_0);
	r->timer_out = rd(SPM_BSI_D1_SR);
	r->r13 = rd(PCM_REG13_DATA);
	r->dbg = rd(SPM_SW_DEBUG);
	r->r15 = rd(PCM_REG15_DATA);
	r->fsm = rd(PCM_FSM_STA);
	r->pwr = rd(SPM_PWR_STATUS);
	r->pwr2 = rd(SPM_PWR_STATUS_2ND);
	r->wsta = rd(SPM_WAKEUP_STA);
	r->tout = rd(PCM_TIMER_OUT);
	r->wout = rd(PCM_WDT_OUT);
	r->irqsta = rd(SPM_IRQ_STA);
out:
	wr(SPM_CPU_WAKEUP_EVENT, 0);
	wr(SPM_WAKEUP_EVENT_MASK, ~0U);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	wr(SPM_SW_INT_CLEAR, 0x3ff);
	wr(PCM_CON1, KEY | (rd(PCM_CON1) & 0xffff & ~(PCM_TIMER_EN | PCM_WDT_EN)));
	wr(PCM_PWR_IO_EN, 0);
	pcm_reset();
	crumb(CR_CYC_DONE);
}

/* saved around one suspend (prepare -> finish) */
static struct {
	bool active;
	u8 prio;
	u32 apsb, irqm, tval, wdtv, bsi[3], wmask, cwe, wfi[10], flag, rsv0, dbg;
	u32 src, src2, clkcon, settle, mp1, mp2, ini, vec[16], pw[32];
	u32 pm[5];		/* dvs: PMIC 0x060a 0x060c 0x044a 0x045c 0x06a8 */
} sv;

static const u16 dvs_reg[5] = { 0x060a, 0x060c, 0x044a, 0x045c, 0x06a8 };

/*
 * Stock MT6797 PMIC_WRAP_PHASE_SUSPEND table (IDX_SP_*), 6.25 mV steps from 0.6 V.
 * Slot 0 (HPM) uses the live VOSEL_ON so a wake returns to today's Vcore.
 * Returns -EIO if a PMIC read fails (then the dummy table is used).
 */
static int dvs_table(void)
{
	u32 adr[16], dat[16], i;

	for (i = 0; i < 5; i++)
		if (pmic_read(dvs_reg[i], &sv.pm[i]))
			return -EIO;
	adr[0] = 0x060a; dat[0] = sv.pm[0];			/* VCORE_HPM (live, 0x40 = 1.0 V) */
	adr[1] = 0x060a; dat[1] = 0x3e;				/* TRANS1 0.98333 V */
	adr[2] = 0x060a; dat[2] = 0x3b;				/* TRANS2 0.96666 V */
	adr[3] = 0x060a; dat[3] = 0x38;				/* TRANS3 0.94999 V */
	adr[4] = 0x060a; dat[4] = 0x36;				/* TRANS4 0.93333 V */
	adr[5] = 0x060a; dat[5] = 0x33;				/* TRANS5 0.91666 V */
	adr[6] = 0x060a; dat[6] = 0x30;				/* LPM 0.9 V */
	adr[7] = 0x060c; dat[7] = 0x00;				/* VOSEL_SLEEP 0.6 V */
	adr[8] = 0x044a; dat[8] = sv.pm[2] | (3 << 8);		/* VSLEEP_SEL 0.6 V */
	adr[9] = 0x060a; dat[9] = 0x1c;				/* DPIDLE_SODI 0.775 V */
	adr[10] = 0x06a8; dat[10] = 0x40;			/* VSRAM_CORE 1.0 V */
	adr[11] = 0x06a8; dat[11] = 0x50;			/* VSRAM_CORE 1.1 V */
	adr[12] = 0x045c; dat[12] = sv.pm[3] | BIT(1);		/* VCORE low-IQ on */
	adr[13] = 0x045c; dat[13] = sv.pm[3] & ~BIT(1);		/* VCORE low-IQ off */
	adr[14] = 0x060c; dat[14] = 0x10;			/* VOSEL_SLEEP 0.7 V */
	adr[15] = 0x044a; dat[15] = sv.pm[2] & ~(3 << 8);	/* VSLEEP_SEL 0.7 V */
	for (i = 0; i < 16; i++) {
		writel(adr[i], pwrap + PWRAP_DVFS_ADR0 + 8 * i);
		writel(dat[i], pwrap + PWRAP_DVFS_ADR0 + 8 * i + 4);
	}
	pr_info(TAG "dvs=%u: stock SUSPEND pwrap table; pmic 060a %04x 060c %04x 044a %04x 045c %04x 06a8 %04x\n",
		dvs, sv.pm[0], sv.pm[1], sv.pm[2], sv.pm[3], sv.pm[4]);
	return 0;
}

static int spm_prepare(void)
{
	struct device_node *np;
	struct of_phandle_args oa = { .args_count = 3, .args = { 0, SPM_IRQ_SPI, IRQ_TYPE_LEVEL_LOW } };
	u32 i, md_settle;
	int ret;

	if (gic_read_pmr() <= SPM_IRQ_PRIO + 0x20) {
		pr_err(TAG "ICC_PMR_EL1 = %02x — refusing\n", gic_read_pmr());
		return -EIO;
	}
	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-sysirq");
	if (!np)
		return -ENODEV;
	oa.np = np;
	spm_irq = irq_create_of_mapping(&oa);
	of_node_put(np);
	if (!spm_irq)
		return -ENXIO;
	/* NO_SUSPEND: stays enabled through suspend_device_irqs() */
	ret = request_irq(spm_irq, spm_isr, IRQF_NO_AUTOEN | IRQF_NO_SUSPEND, "mt6797-spm", NULL);
	if (ret) {
		irq_dispose_mapping(spm_irq);
		spm_irq = 0;
		return ret;
	}

	sv.apsb = rd(AP_STANDBY_CON);
	sv.irqm = rd(SPM_IRQ_MASK);
	sv.tval = rd(PCM_TIMER_VAL);
	sv.wdtv = rd(PCM_WDT_VAL_OFF);
	for (i = 0; i < 3; i++)
		sv.bsi[i] = rd(SPM_BSI_D0_SR + 4 * i);
	sv.wmask = rd(SPM_WAKEUP_EVENT_MASK);
	sv.cwe = rd(SPM_CPU_WAKEUP_EVENT);
	for (i = 0; i < 10; i++)
		sv.wfi[i] = rd(MP0_CPU0_WFI_EN + 4 * i);
	sv.flag = rd(SPM_SW_FLAG);
	sv.rsv0 = rd(SPM_SW_RSV_0);
	sv.dbg = rd(SPM_SW_DEBUG);
	sv.src = rd(SPM_SRC_MASK);
	sv.src2 = rd(SPM_SRC2_MASK);
	sv.clkcon = rd(SPM_CLK_CON);
	sv.settle = rd(SPM_CLK_SETTLE);
	sv.mp1 = rd(SPM_MAS_PAUSE_MASK_B);
	sv.mp2 = rd(SPM_MAS_PAUSE2_MASK_B);
	sv.ini = rd(PCM_REG_DATA_INI);
	for (i = 0; i < 16; i++)
		sv.vec[i] = rd(PCM_EVENT_VECTOR0 + 4 * i);
	for (i = 0; i < 32; i++)
		sv.pw[i] = readl(pwrap + PWRAP_DVFS_ADR0 + 4 * i);
	dvs_active = 0;
	if (dvs && dvs_table() == 0)
		dvs_active = dvs;
	else for (i = 0; i < 16; i++) {	/* SPM PMIC commands -> read-only HWCID */
		writel(0x0200, pwrap + PWRAP_DVFS_ADR0 + 8 * i);
		writel(0, pwrap + PWRAP_DVFS_ADR0 + 8 * i + 4);
	}
	sv.prio = readb(gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	writeb(SPM_IRQ_PRIO, gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	wr(SPM_IRQ_STA, rd(SPM_IRQ_STA) & 0x3ff0c);
	if (src_mask != ~0U)
		wr(SPM_SRC_MASK, src_mask);
	wr(SPM_CLK_CON, rd(SPM_CLK_CON) | CLK_CON_VENDOR_INIT | SPM_LOCK_INFRA_DCM);
	wr(SPM_CLK_CON, rd(SPM_CLK_CON) | SYS_SETTLE_SEL);
	wr(SPM_CLK_SETTLE, 0);
	md_settle = rd(SPM_CLK_SETTLE);
	if (md_settle < SPM_SYSCLK_SETTLE)
		wr(SPM_CLK_SETTLE, SPM_SYSCLK_SETTLE - md_settle);
	enable_irq(spm_irq);
	sv.active = true;
	return 0;
}

static void spm_finish(void)
{
	u32 i;

	if (!sv.active)
		return;
	disable_irq(spm_irq);
	free_irq(spm_irq, NULL);
	irq_dispose_mapping(spm_irq);
	spm_irq = 0;
	writeb(sv.prio, gicd + GICD_IPRIORITYR + 32 + SPM_IRQ_SPI);
	wr(PCM_PWR_IO_EN, 0);
	for (i = 0; i < 16; i++)
		wr(PCM_EVENT_VECTOR0 + 4 * i, sv.vec[i]);
	wr(PCM_REG_DATA_INI, sv.ini);
	wr(SPM_MAS_PAUSE_MASK_B, sv.mp1);
	wr(SPM_MAS_PAUSE2_MASK_B, sv.mp2);
	wr(SPM_SW_RSV_0, sv.rsv0);
	wr(SPM_SW_FLAG, sv.flag);
	wr(SPM_SW_DEBUG, sv.dbg);
	wr(SPM_SRC_MASK, sv.src);
	wr(SPM_SRC2_MASK, sv.src2);
	wr(SPM_CLK_SETTLE, sv.settle);
	wr(SPM_CLK_CON, sv.clkcon);
	for (i = 0; i < 10; i++)
		wr(MP0_CPU0_WFI_EN + 4 * i, sv.wfi[i]);
	wr(AP_STANDBY_CON, sv.apsb);
	wr(SPM_CPU_WAKEUP_EVENT, sv.cwe);
	wr(SPM_WAKEUP_EVENT_MASK, sv.wmask);
	wr(SPM_IRQ_MASK, sv.irqm);
	wr(PCM_TIMER_VAL, sv.tval);
	wr(PCM_WDT_VAL_OFF, sv.wdtv);
	for (i = 0; i < 3; i++)
		wr(SPM_BSI_D0_SR + 4 * i, sv.bsi[i]);
	for (i = 0; i < 32; i++)
		writel(sv.pw[i], pwrap + PWRAP_DVFS_ADR0 + 4 * i);
	if (dvs_active) {	/* PMIC back to the pre-suspend values (VSRAM before Vcore) */
		u32 now[5];

		for (i = 0; i < 5; i++)
			if (pmic_read(dvs_reg[i], &now[i]))
				now[i] = ~0U;
		pr_info(TAG "dvs: pmic after sleep 060a %04x 060c %04x 044a %04x 045c %04x 06a8 %04x\n",
			now[0], now[1], now[2], now[3], now[4]);
		pmic_write(0x06a8, sv.pm[4]);
		udelay(200);
		for (i = 0; i < 4; i++)
			pmic_write(dvs_reg[i], sv.pm[i]);
		udelay(200);
		dvs_active = 0;
	}
	sv.active = false;
}

static bool key_ints_on;
static u32 int_con0_saved;

static void key_ints_enable(void)
{
	key_ints_on = false;
	if (!wake_keys || pmic_read(MT6351_INT_CON0, &int_con0_saved))
		return;
	if (pmic_write(MT6351_INT_CON0, int_con0_saved | KEY_INTS) ||
	    pmic_write(MT6351_INT_STATUS0, KEY_INTS))	/* clear stale latches */
		return;
	key_ints_on = true;
}

static void key_ints_restore(void)
{
	if (!key_ints_on)
		return;
	pmic_write(MT6351_INT_STATUS0, KEY_INTS);
	pmic_write(MT6351_INT_CON0, int_con0_saved);
	key_ints_on = false;
}

/* --------------------------------------------------------- suspend ops */
static int gem_suspend_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}

static int gem_suspend_begin(suspend_state_t state)
{
	pin_domains();
	return 0;
}

static int gem_suspend_prepare(void)
{
	int ret;

	if (mode != 1)
		return 0;
	strscpy(last_wake, "none", sizeof(last_wake));
	if (disp_check && (readl(ovl + OVL_SRC_CON) & BIT(0))) {
		pr_warn(TAG "display still on (OVL0 SRC_CON %08x) - refusing\n",
			readl(ovl + OVL_SRC_CON));
		strscpy(last_wake, "display on", sizeof(last_wake));
		return -EBUSY;
	}
	{
		unsigned int enp = !!(readl(gpio + GPIO_DIN(LCD_ENP_GPIO)) & BIT(LCD_ENP_GPIO % 32));
		unsigned int enn = !!(readl(gpio + GPIO_DIN(LCD_ENN_GPIO)) & BIT(LCD_ENN_GPIO % 32));

		snprintf(bias_at_sleep, sizeof(bias_at_sleep), "ENP %u ENN %u", enp, enn);
		pr_info(TAG "LCD bias before sleep: %s\n", bias_at_sleep);
		if (bias_check && (enp || enn)) {
			pr_warn(TAG "LCD bias still on - refusing (geminipda_drm panel_suspend=1?)\n");
			strscpy(last_wake, "bias on", sizeof(last_wake));
			return -EBUSY;
		}
	}
	im_verify("prepare");
	cr_susp++;
	cr_n = 0;
	crumb(CR_PREPARE);
	if (poll_ms < 100 || poll_ms > 5000 || lid_poll_ms < 100 || lid_poll_ms > 600000 ||
	    (io_en != 0x80 && io_en != 0x81)) {
		pr_err(TAG "bad poll_ms/lid_poll_ms/io_en\n");
		return -EINVAL;
	}
	bucks_down();
	ret = spm_prepare();
	if (ret) {
		pr_err(TAG "SPM prepare failed: %d\n", ret);
		return ret;
	}
	key_ints_enable();
	return 0;
}

static const char *const why_txt[] = { "max_s", "key", "lid opened", "SPM wake reason", "cycle failed", "pmic read failed" };

static int gem_suspend_enter(suspend_state_t state)
{
	struct cyc r, first = {}, last = {};
	u64 frq = read_sysreg(cntfrq_el0), t0 = read_sysreg(cntvct_el0);
	u32 n = 0, top = 0, kint = 0, asleep_ms = 0, wdt_mode, cyc_ms = poll_ms, wmask = wake_unmask, glitch = 0;
	u32 loop_mode, req_mode = 0, req_irq = 0;
	bool lid0, armed = false;
	int why = 0, ret;

	pr_info(TAG "enter: cpu%d, %u cpus online, PWR_STATUS %08x\n",
		raw_smp_processor_id(), num_online_cpus(), rd(SPM_PWR_STATUS));

	if (mode == 0) {
		u32 ms;

		for (ms = 0; ms < probe_ms; ms += 1000) {
			wdt_feed();
			mdelay(min(1000U, probe_ms - ms));
		}
		wdt_feed();
		return 0;
	}
	if (num_online_cpus() != 1 || raw_smp_processor_id() != 0)
		return -EBUSY;

	lid0 = lid_closed();
	wdt_mode = readl(wdt + WDT_MODE) & 0xff;
	loop_mode = wdt_mode;
	if (wdt_pause && (wdt_mode & WDT_MODE_EN))
		loop_mode &= ~WDT_MODE_EN;
	else if (wdt_int_rst && (wdt_mode & WDT_MODE_EN))
		loop_mode &= ~WDT_MODE_EXTEN;
	if (pcm_wdt) {	/* stock wd_spmwdt_mode_config(WD_REQ_EN, WD_REQ_RST_MODE) */
		loop_mode &= ~WDT_MODE_EXTEN;	/* SoC-only reset keeps the breadcrumb */
		req_mode = readl(wdt + WDT_REQ_MODE);
		req_irq = readl(wdt + WDT_REQ_IRQ_EN);
		writel(WDT_REQ_IRQ_KEY | (req_irq & 0xffffff & ~WDT_REQ_SPM_SCPSYS), wdt + WDT_REQ_IRQ_EN);
		writel(WDT_REQ_MODE_KEY | (req_mode & 0xffffff) | WDT_REQ_SPM_SCPSYS, wdt + WDT_REQ_MODE);
	}
	if (loop_mode != wdt_mode) {
		wdt_feed();
		writel(WDT_MODE_KEY | loop_mode, wdt + WDT_MODE);
	}
	if (touch_rst)	/* single CPU, IRQs off: plain RMW is safe */
		writel(readl(gpio + GPIO_DOUT(CTP_RST_GPIO)) & ~BIT(CTP_RST_GPIO % 32),
		       gpio + GPIO_DOUT(CTP_RST_GPIO));
	pmic_misc_set();
	if (ldo_lp_save() && ldo_lp_mode == 2) {
		ldo_lp_set(true);
		ldo_lp_log();
	}
	/*
	 * Long cycles only with the WDT stopped: it fires after 31 s
	 * (wdt_pause=0 hang hunting keeps the poll_ms cycles).
	 */
	if (pcm_wdt && pcm_wdt_wake)
		wmask |= R12_PCM_WDT_WAKEUP_B;
	if (lid_eint && wake_lid && lid0 && (wdt_pause || !(wdt_mode & WDT_MODE_EN))) {
		lid_eint_arm();
		armed = true;
		cyc_ms = lid_poll_ms;
		wmask |= R12_EINT_EVENT_B;
	}
	for (;;) {
		wdt_feed();
		cr_n = n;
		crumb(CR_LOOP);
		if (im_load()) {
			why = 4;
			break;
		}
		if (ldo_lp_mode == 1) {
			ldo_lp_set(true);
			if (!n)
				ldo_lp_log();
		}
		spm_sleep_once(&r, cyc_ms, wmask);
		if (ldo_lp_mode == 1)
			ldo_lp_set(false);
		if (!n)
			first = r;
		last = r;
		n++;
		if (r.refused || !r.returned || r.pm_ret) {
			why = 4;
			break;
		}
		asleep_ms += (u32)div_u64((u64)r.timer_out * 1000, 32768);
		if ((r.r12 & ~(R12_PCM_TIMER | (armed ? R12_EINT_EVENT_B : 0))) ||
		    (r.wsta & R12_PCM_WDT_WAKEUP_B)) {
			why = 3;
			break;
		}
		if (armed && (r.r12 & R12_EINT_EVENT_B)) {
			if (!lid_closed()) {
				why = 2;
				break;
			}
			glitch++;	/* still shut: clear it, sleep on */
			writel(BIT(LID_EINT), eint + EINT_ACK);
		}
		if (wake_keys) {
			ret = pmic_read(MT6351_TOPSTATUS, &top);
			if (ret) {
				why = 5;
				break;
			}
			if ((top & KEYS_MASK) != KEYS_MASK) {
				why = 1;
				break;
			}
			if (key_ints_on && !pmic_read(MT6351_INT_STATUS0, &kint) && (kint & KEY_INTS)) {
				why = 1;
				break;
			}
		}
		if (wake_lid && lid0 && !lid_closed()) {
			why = 2;
			break;
		}
		if (max_s && asleep_ms >= max_s * 1000) {
			why = 0;
			break;
		}
		crumb(CR_CHECKED);
		mdelay(gap_ms);
	}

	if (armed)
		lid_eint_disarm();
	pmic_misc_restore();
	ldo_lp_set(false);
	if (ldo_lp_on)
		ldo_lp_log();
	ldo_lp_on = false;
	if (touch_rst)	/* back to run; resume re-inits the chip (driver rebind) */
		writel(readl(gpio + GPIO_DOUT(CTP_RST_GPIO)) | BIT(CTP_RST_GPIO % 32),
		       gpio + GPIO_DOUT(CTP_RST_GPIO));
	wdt_feed();
	if (loop_mode != wdt_mode)
		writel(WDT_MODE_KEY | wdt_mode, wdt + WDT_MODE);
	if (pcm_wdt) {	/* stock: WD_REQ_DIS after the sleep */
		writel(WDT_REQ_MODE_KEY | (req_mode & 0xffffff), wdt + WDT_REQ_MODE);
		writel(WDT_REQ_IRQ_KEY | (req_irq & 0xffffff), wdt + WDT_REQ_IRQ_EN);
	}
	crumb(CR_LOOP_EXIT);
	strscpy(last_wake, why_txt[why], sizeof(last_wake));
	pr_info(TAG "woke: %s after %u cycles of %u ms, ~%u s asleep / %llu s in enter; lid %s at entry%s (%u glitches); TOPSTATUS %04x INT_STATUS0 %04x%s; WDT_MODE %02x%s; PCM WDT %u s%s\n",
		why_txt[why], n, cyc_ms, asleep_ms / 1000,
		frq ? div64_u64(read_sysreg(cntvct_el0) - t0, frq) : 0,
		lid0 ? "closed" : "open", armed ? ", lid EINT" : "", glitch,
		top, kint, touch_rst ? "; touch held in reset" : "",
		readl(wdt + WDT_MODE) & 0xff, wdt_pause ? " (paused in loop)" : "",
		pcm_wdt, pcm_wdt && pcm_wdt_test ? " (TEST: fires at 2 s)" : "");
	pr_info(TAG "  first: %s psci %d r12 %08x r13 %08x dbg %08x | last: %s psci %d r12 %08x r13 %08x dbg %08x timer_out %u\n",
		first.refused ? "REFUSED" : first.returned ? "ret" : "NO-RET", first.psci_ret,
		first.r12, first.r13, first.dbg,
		last.refused ? "REFUSED" : last.returned ? "ret" : "NO-RET", last.psci_ret,
		last.r12, last.r13, last.dbg, last.timer_out);
	pr_info(TAG "  last raw: R15(PC) %08x FSM %08x PWR %08x/%08x WAKEUP_STA %08x TIMER_OUT %08x WDT_OUT %08x IRQ_STA %08x\n",
		last.r15, last.fsm, last.pwr, last.pwr2, last.wsta, last.tout, last.wout, last.irqsta);
	return 0;
}

static void gem_suspend_finish(void)
{
	crumb(CR_FINISH);
	bucks_up();
	key_ints_restore();
	spm_finish();
	im_verify("finish");
	crumb(CR_FINISH_DONE);
}

static void gem_suspend_end(void)
{
	unpin_domains();
	if (mode == 1)
		crumb(CR_END);
}

static const struct platform_suspend_ops gem_suspend_ops = {
	.valid   = gem_suspend_valid,
	.begin   = gem_suspend_begin,
	.prepare = gem_suspend_prepare,
	.enter   = gem_suspend_enter,
	.finish  = gem_suspend_finish,
	.end     = gem_suspend_end,
};

/* ------------------------------------------------------------- init/exit */
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
	if (ret)
		return ret;

	memcpy(page_address(page), prog, bytes);
	im_copy = kmemdup(prog, bytes, GFP_KERNEL);
	dma_len = bytes;
	dma = dma_map_page(dev, page, 0, bytes, DMA_BIDIRECTIONAL);	/* cleans D-cache */
	if (dma_mapping_error(dev, dma) || dma != page_to_phys(page)) {
		if (!dma_mapping_error(dev, dma))
			dma_unmap_page(dev, dma, bytes, DMA_BIDIRECTIONAL);
		__free_pages(page, page_order);
		kfree(im_copy);
		im_copy = NULL;
		page = NULL;
		return -EIO;
	}
	return 0;
}

static void unmap_all(void)
{
	if (spm)
		iounmap(spm);
	if (pwrap)
		iounmap(pwrap);
	if (gpio)
		iounmap(gpio);
	if (gicd)
		iounmap(gicd);
	if (wdt)
		iounmap(wdt);
	if (ovl)
		iounmap(ovl);
	if (eint)
		iounmap(eint);
	spm = pwrap = gpio = gicd = wdt = ovl = eint = NULL;
}

static int __init gem_suspend_init(void)
{
	const struct firmware *f = NULL;
	void __iomem *infra;
	bool mode4g;
	u32 top = 0;
	int ret;

	spm = ioremap(SPM_PHYS, 0x1000);
	pwrap = ioremap(PWRAP_PHYS, 0x1000);
	gpio = ioremap(GPIO_PHYS, 0x1000);
	gicd = ioremap(GICD_PHYS, 0x1000);
	wdt = ioremap(WDT_PHYS, 0x100);
	ovl = ioremap(OVL0_PHYS, 0x100);
	eint = ioremap(EINT_PHYS, 0x1000);
	infra = ioremap(INFRA_AO_PHYS, 0x1000);
	if (!spm || !pwrap || !gpio || !gicd || !wdt || !ovl || !eint || !infra) {
		ret = -ENOMEM;
		goto out_map;
	}
	{	/* breadcrumb left by the previous boot / load */
		u32 c = readl(wdt + WDT_NONRST2);

		if ((c >> 28) == 0xb)
			pr_warn(TAG "last breadcrumb %08x: step %s, suspend #%u, cycle %u\n",
				c, cr_txt[(c >> 24) & 0xf], (c >> 16) & 0xff, c & 0xffff);
		else
			pr_info(TAG "NONRST_REG2 %08x (no breadcrumb)\n", c);
		writel(0, wdt + WDT_NONRST2);
		pr_info(TAG "TOPRGU WDT_STA %08x (bit1 = SPM/PCM WDT reset) REQ_MODE %08x REQ_IRQ_EN %08x\n",
			readl(wdt + WDT_STA), readl(wdt + WDT_REQ_MODE), readl(wdt + WDT_REQ_IRQ_EN));
	}
	mode4g = readl(infra + INFRA_MISC) & BIT(13);
	iounmap(infra);
	infra = NULL;

	pdev = platform_device_register_simple("mt6797-suspend", -1, NULL, 0);
	if (IS_ERR(pdev)) {
		ret = PTR_ERR(pdev);
		goto out_map;
	}
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	if (ret)
		goto out_pdev;

	ret = request_firmware_direct(&f, fw, &pdev->dev);
	if (ret) {
		pr_err(TAG "request_firmware_direct(%s) = %d\n", fw, ret);
		goto out_pdev;
	}
	ret = -EINVAL;
	if (f->size < 2)
		goto out_fw;
	im_words = get_unaligned_le16(f->data);
	if (im_words < 16 || im_words > IM_MAX_WORDS || f->size < 2 + im_words * 4 + sizeof(tail) + 1)
		goto out_fw;
	memcpy(&tail, f->data + 2 + im_words * 4, sizeof(tail));
	if (tail.size != im_words || tail.sess > 2 || tail.replace > 1)
		goto out_fw;

	if ((rd(PCM_FSM_STA) & 0x7fffff) != FSM_DEF || rd(PCM_PWR_IO_EN) ||
	    !(rd(POWERON_CONFIG_EN) & 1)) {
		pr_err(TAG "PCM not in reset-default / PWR_IO_EN set / bus CG off — refusing\n");
		ret = -EBUSY;
		goto out_fw;
	}
	ret = alloc_buf(&pdev->dev, f->data + 2, im_words * 4);
	if (ret)
		goto out_fw;
	im_ptr = (u32)page_to_phys(page) + (mode4g ? 0x40000000 : 0);
	release_firmware(f);
	f = NULL;

	saved_con0 = rd(PCM_CON0);
	saved_con1 = rd(PCM_CON1);
	saved_ptr = rd(PCM_IM_PTR);
	saved_len = rd(PCM_IM_LEN);
	loaded = true;

	pmic_read(MT6351_TOPSTATUS, &top);
	suspend_set_ops(&gem_suspend_ops);
	pr_info(TAG "deep suspend registered (mode %u, poll %u ms, lid EINT %s %u ms, gap %u ms, max_s %u, flags %08x%s); fw %u words, IM_PTR %08x; TOPSTATUS %04x, lid %s\n",
		mode, poll_ms, lid_eint ? "on" : "off", lid_poll_ms, gap_ms, max_s, flags,
		srcclken_low ? " srcclken_low" : "", im_words, im_ptr, top, lid_closed() ? "closed" : "open");
	return 0;

out_fw:
	if (ret == -EINVAL)
		pr_err(TAG "%s: bad format\n", fw);
	release_firmware(f);
out_pdev:
	platform_device_unregister(pdev);
out_map:
	if (infra)
		iounmap(infra);
	unmap_all();
	return ret;
}

static void __exit gem_suspend_exit(void)
{
	suspend_set_ops(NULL);
	unpin_domains();
	if (loaded) {
		pcm_reset();
		wr(PCM_IM_PTR, saved_ptr);
		wr(PCM_IM_LEN, saved_len);
		wr(PCM_CON1, KEY | (saved_con1 & 0xffff));
		wr(PCM_CON0, KEY | (saved_con0 & 0xffff));
	}
	if (page) {
		dma_unmap_page(&pdev->dev, dma, dma_len, DMA_BIDIRECTIONAL);
		__free_pages(page, page_order);
		kfree(im_copy);
		im_copy = NULL;
	}
	platform_device_unregister(pdev);
	unmap_all();
	pr_info(TAG "unregistered\n");
}

module_init(gem_suspend_init);
module_exit(gem_suspend_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 system suspend via the SPM (Gemini PDA)");
