// SPDX-License-Identifier: GPL-2.0
/*
 * SUPERSEDED 2026-10-06 by the built-in driver
 * kernel/delta/drivers/soc/mediatek/mt6797-cl2-power.c (same sequence,
 * regulator + reset APIs). Kept as the test record; it refuses to run when
 * the A72s are already online (and 0x68 is owned by da9211 now).
 *
 * mt6797-cl2-on.c — bring the A72 cluster (cpu8/cpu9) online from the
 * kernel, in the VENDOR order, with tight timing. Test vehicle for
 * docs/cpu-dvfs.md "A72 bring-up" (option B), replacing cl2-up.sh.
 *
 * Why: cl2-up.sh (userspace: devmem + i2cset + sramldo-smc + sysfs
 * online) differs from the vendor kernel in two ways that can matter:
 *  1. ORDER. Vendor gemian/gemini-linux-kernel-3.18 arch/arm64/kernel/
 *     psci.c cpu_power_on_buck() asserts the SWSYSRST latch bit 11
 *     (MTK_WDT_SWSYS_RST_PWRAP_SPI_CTL_RST: PMIC-wrapper SPI controller
 *     held in reset) BEFORE switching the DA9214 BUCKB rail on, and only
 *     releases it after the 1 ms settle + EXT_BUCK_ISO clear. cl2-up.sh
 *     turns BUCKB on first, outside the latch.
 *  2. WINDOW. cl2-up.sh holds the latch across several devmem process
 *     launches (tens of ms) while the kernel keeps using the MT6351 over
 *     pwrap (regulators, audio, keys, gauge, RTC).
 * Hypothesis: a pwrap transaction collides with the rail switch-on or the
 * long latch window -> pwrap stuck -> RCU stalls / eMMC timeouts (the
 * 2026-09-07 "wedge while boot is busy").
 *
 * Sequence (vendor cpu_power_on_buck(cpu, hotplug=1) + cpu_psci_cpu_boot):
 *   1. SPM 0x10006218 |= bit0
 *   2. dummy read 0x102224a0 (A72 iDVFS PLL CON0)
 *   3. SWSYSRST (0x10007018): key 0x88000000 | bit11   -> latch
 *   4. DA9214 (i2c6 @0x68): reg 0x00 [3:0] = 0 (page 0); reg 0x5E bit0 = 1 (BUCKB on)
 *   5. udelay(1000)
 *   6. SPM 0x10006290 &= ~3  (EXT_BUCK_ISO clear)
 *   7. SWSYSRST: key | (val & ~bit11)                   -> unlatch
 *   8. udelay(240); SMC 0xC20003BF(110000) (A72 SRAM LDO 1.1 V); udelay(240)
 *   9. add_cpu(8) (PSCI CPU_ON, SMC32 id from our DT), then add_cpu(9)
 * Steps 1-3 and 6-7 run with IRQs off; step 4 (I2C, sleeps) between them,
 * exactly as the vendor does (its window also contains the I2C write).
 * Not done (vendor does it after CPU_ON, not needed to boot): MP2 sync DCM
 * enable and iDVFS/OCP init.
 *
 * Safety net: the kernel's own mtk_wdt (31 s, pinged by the watchdog
 * core) — the module does not touch the WDT by default (see `wdt`).
 *
 * Usage (as root, A72s OFFLINE, gemini-a72-up NOT run this boot):
 *   insmod mt6797-cl2-on.ko                 # full sequence + online cpu8, cpu9
 *   insmod mt6797-cl2-on.ko dry=1           # only log the current state
 *   insmod mt6797-cl2-on.ko cpus=1          # online cpu8 only
 * Everything is logged with "cl2-on:" + microsecond timings; the module
 * stays loaded doing nothing (rmmod any time; it does not power down).
 */

#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>

static bool dry;
module_param(dry, bool, 0444);
MODULE_PARM_DESC(dry, "only log the current state, change nothing");

static int cpus = 2;
module_param(cpus, int, 0444);
MODULE_PARM_DESC(cpus, "A72 cores to online after power-on: 0, 1 (cpu8) or 2 (cpu8+cpu9)");

/*
 * Default 0 = leave the watchdog alone (2026-10-06). The mainline mtk_wdt
 * driver owns it (CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED: the kernel pings it,
 * LK leaves MODE 0x5D / LENGTH 0xF800 = 31 s), so a hard freeze already
 * resets the board within 31 s. The cl2-up.sh-style arm below writes
 * LENGTH = n << 5, i.e. n/64 s (not n seconds), and the disarm switches
 * the WDT off for the rest of the boot — the old "reboot trap". Kept only
 * for comparison runs.
 */
static int wdt;
module_param(wdt, int, 0444);
MODULE_PARM_DESC(wdt, "cl2-up.sh-style WDT arm/disarm (LENGTH units of 1/64 s; 0 = do not touch, recommended)");

#define SPM_PHYS		0x10006000UL
#define SPM_CL2_218		0x218	/* vendor: |= bit0 before buck on */
#define SPM_EXT_BUCK_ISO	0x290	/* bits1:0 = isolation; cleared after buck on */
#define WDT_PHYS		0x10007000UL
#define WDT_MODE		0x000
#define WDT_LENGTH		0x004
#define WDT_SWSYSRST		0x018
#define WDT_MODE_KEY		0x22000000
#define SWSYSRST_KEY		0x88000000
#define SWSYS_PWRAP_SPI_RST	0x00000800
#define IDVFS_PHYS		0x10222000UL
#define IDVFS_ARMPLL_CON0	0x4a0
#define DA9214_ADDR		0x68
#define DA9214_PAGE_CON		0x00
#define DA9214_BUCKB_CONT	0x5e
#define DA9214_VBUCKB_A		0xd9
#define MTK_SIP_SRAMLDO_SET	0xC20003BFUL
#define A72_SRAM_UV_X100	110000	/* 1.1 V, vendor value */

static void __iomem *spm, *wdtb, *idvfs;

#define T() ktime_to_us(ktime_get())

static int da_read(struct i2c_client *c, u8 reg)
{
	int v = i2c_smbus_read_byte_data(c, reg);

	if (v < 0)
		pr_err("cl2-on: DA9214 read 0x%02x failed %d\n", reg, v);
	return v;
}

static void log_state(struct i2c_client *c, const char *when)
{
	int b = c ? da_read(c, DA9214_BUCKB_CONT) : -1;
	int v = c ? da_read(c, DA9214_VBUCKB_A) : -1;

	pr_info("cl2-on: [%s] online=%*pbl SPM218=0x%08x SPM290=0x%08x SWSYSRST=0x%08x WDT_MODE=0x%08x BUCKB_CONT=0x%02x VBUCKB=%d mV\n",
		when, cpumask_pr_args(cpu_online_mask),
		readl(spm + SPM_CL2_218), readl(spm + SPM_EXT_BUCK_ISO),
		readl(wdtb + WDT_SWSYSRST), readl(wdtb + WDT_MODE),
		b, v < 0 ? -1 : 300 + (v & 0x7f) * 10);
}

static struct i2c_client *da9214_client(void)
{
	struct device_node *np = of_find_node_by_path("/i2c@1100e000");
	struct i2c_adapter *adap;
	struct i2c_client *c;

	if (!np) {
		pr_err("cl2-on: no /i2c@1100e000 node\n");
		return NULL;
	}
	adap = of_find_i2c_adapter_by_node(np);
	of_node_put(np);
	if (!adap) {
		pr_err("cl2-on: i2c6 adapter not probed\n");
		return NULL;
	}
	c = i2c_new_dummy_device(adap, DA9214_ADDR);
	i2c_put_adapter(adap);
	if (IS_ERR(c)) {
		pr_err("cl2-on: dummy client 0x68: %ld (address in use?)\n", PTR_ERR(c));
		return NULL;
	}
	return c;
}

static void wdt_arm(void)
{
	if (wdt <= 0)
		return;
	writel((wdt << 5) | 0x8, wdtb + WDT_LENGTH);	/* same as cl2-up.sh wdt_arm */
	pr_info("cl2-on: WDT LENGTH field %d (= %d/64 s)\n", wdt, wdt);
}

static void wdt_disarm(void)
{
	if (wdt <= 0)
		return;
	writel(WDT_MODE_KEY, wdtb + WDT_MODE);		/* same as cl2-up.sh wdt_disarm */
	pr_info("cl2-on: WDT disarmed (MODE=0x%08x)\n", readl(wdtb + WDT_MODE));
}

static int power_on_cluster(struct i2c_client *c)
{
	struct arm_smccc_res res;
	unsigned long flags;
	u32 v;
	s64 t0, t_latch, t_buck, t_unlatch;
	int r, page;

	t0 = T();
	/* 1-3: SPM bit, dummy read, latch PWRAP SPI ctl reset (IRQs off) */
	local_irq_save(flags);
	writel(readl(spm + SPM_CL2_218) | BIT(0), spm + SPM_CL2_218);
	(void)readl(idvfs + IDVFS_ARMPLL_CON0);
	v = readl(wdtb + WDT_SWSYSRST);
	writel(v | SWSYSRST_KEY | SWSYS_PWRAP_SPI_RST, wdtb + WDT_SWSYSRST);
	local_irq_restore(flags);
	t_latch = T();

	/* 4: DA9214 page 0 + BUCKB enable (vendor da9214_config_interface) */
	page = i2c_smbus_read_byte_data(c, DA9214_PAGE_CON);
	if (page >= 0 && (page & 0xf))
		r = i2c_smbus_write_byte_data(c, DA9214_PAGE_CON, page & ~0xf);
	r = i2c_smbus_read_byte_data(c, DA9214_BUCKB_CONT);
	if (r >= 0)
		r = i2c_smbus_write_byte_data(c, DA9214_BUCKB_CONT, r | BIT(0));
	t_buck = T();
	/* 5 */
	udelay(1000);

	/* 6-7: EXT_BUCK_ISO clear, unlatch (IRQs off) */
	local_irq_save(flags);
	writel(readl(spm + SPM_EXT_BUCK_ISO) & ~0x3u, spm + SPM_EXT_BUCK_ISO);
	v = readl(wdtb + WDT_SWSYSRST);
	writel((v | SWSYSRST_KEY) & ~SWSYS_PWRAP_SPI_RST, wdtb + WDT_SWSYSRST);
	local_irq_restore(flags);
	t_unlatch = T();

	pr_info("cl2-on: latch window %lld us (I2C %lld us), page=0x%02x BUCKB write %s\n",
		t_unlatch - t_latch, t_buck - t_latch, page < 0 ? 0xff : page,
		r < 0 ? "FAILED" : "ok");
	if (r < 0)
		return r;

	/* 8: A72 SRAM LDO */
	udelay(240);
	arm_smccc_smc(MTK_SIP_SRAMLDO_SET, A72_SRAM_UV_X100, 0, 0, 0, 0, 0, 0, &res);
	udelay(240);
	pr_info("cl2-on: SRAM LDO SMC(%u) -> 0x%lx; power-on done in %lld us\n",
		A72_SRAM_UV_X100, res.a0, T() - t0);
	return res.a0 ? -EIO : 0;
}

static int __init cl2_on_init(void)
{
	struct i2c_client *c;
	int ret = 0;
	s64 t;

	spm = ioremap(SPM_PHYS, 0x1000);
	wdtb = ioremap(WDT_PHYS, 0x1000);
	idvfs = ioremap(IDVFS_PHYS, 0x1000);
	if (!spm || !wdtb || !idvfs) {
		ret = -ENOMEM;
		goto out_unmap;
	}

	c = da9214_client();
	pr_info("cl2-on: dry=%d cpus=%d wdt=%d\n", dry, cpus, wdt);
	log_state(c, "before");
	if (dry || !c)
		goto out_client;

	if (cpu_online(8) || cpu_online(9)) {
		pr_warn("cl2-on: A72 already online - nothing to do\n");
		goto out_client;
	}

	wdt_arm();
	ret = power_on_cluster(c);
	log_state(c, "rail on");
	if (ret) {
		wdt_disarm();
		goto out_client;
	}

	if (cpus >= 1) {
		t = T();
		ret = add_cpu(8);
		pr_info("cl2-on: add_cpu(8) = %d in %lld us\n", ret, T() - t);
	}
	if (!ret && cpus >= 2) {
		t = T();
		ret = add_cpu(9);
		pr_info("cl2-on: add_cpu(9) = %d in %lld us\n", ret, T() - t);
	}
	wdt_disarm();
	log_state(c, "after");

out_client:
	if (c)
		i2c_unregister_device(c);
out_unmap:
	if (idvfs)
		iounmap(idvfs);
	if (wdtb)
		iounmap(wdtb);
	if (spm)
		iounmap(spm);
	spm = wdtb = idvfs = NULL;
	/* stay loaded even on failure so the log context is obvious; rmmod is a no-op */
	return 0;
}

static void __exit cl2_on_exit(void)
{
}

module_init(cl2_on_init);
module_exit(cl2_on_exit);
MODULE_DESCRIPTION("MT6797 A72 cluster power-on in vendor order (Gemini PDA test module)");
MODULE_LICENSE("GPL");
