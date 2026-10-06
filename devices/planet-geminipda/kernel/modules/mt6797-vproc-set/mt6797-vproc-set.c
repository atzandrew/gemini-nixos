// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-vproc-set.c — TEST module: set VPROC1 (DA9214 BUCKA = A53 LL + L
 * clusters + CCI) through the regulator framework. docs/cpu-dvfs.md,
 * "first voltage change" (2026-10-06).
 *
 * Why: LK leaves VPROC1 at 1.000 V while running L (cpu4-7) at 1274 MHz and
 * CCI at 630 MHz; the vendor SB sign-off table wants 1.04 V above 1209 /
 * 611 MHz. 1.04 V adds margin at today's clocks and is the voltage for the
 * next L step (1352 MHz). No clock is changed here.
 *
 * Safety rules enforced (vendor mt_cpufreq.c set_cur_volt_extbuck):
 *  - target within [1000, 1100] mV (test window; vendor table 770-1200);
 *  - VSRAM_L (CPULDO, INFRACFG_AO 0x10001F9C) must stay >= VPROC1 and
 *    <= VPROC1 + 300 mV; LK leaves VSRAM_L at 1100 mV, so up to 1100 mV is
 *    fine without touching the SRAM rail (that gets its own driver later);
 *  - the regulator core aggregates consumer requests; nothing else consumes
 *    vproc1 today (always-on, no cpufreq yet).
 *
 * Usage (root):  insmod mt6797-vproc-set.ko uv=1040000   # set 1.04 V
 *                rmmod mt6797_vproc_set                  # voltage STAYS
 *                insmod mt6797-vproc-set.ko uv=1000000   # back to LK's 1.00 V
 * The module keeps its regulator handle while loaded; rmmod drops the
 * request but the DA9214 keeps the last value (the core doesn't revert).
 * Logs "vproc-set:" with before/after (regulator readback = DA9214 0xD7).
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/regulator/consumer.h>

static int uv = 1040000;
module_param(uv, int, 0444);
MODULE_PARM_DESC(uv, "VPROC1 target in microvolts (1000000..1100000)");

#define INFRACFG_AO_PHYS	0x10001000UL
#define CPULDO_CTRL_0		0xf98
#define CPULDO_CTRL_1		0xf9c

static struct regulator *vproc1;

static int vsram_l_uv(void)
{
	void __iomem *b = ioremap(INFRACFG_AO_PHYS, 0x1000);
	u32 en, v;
	int sel;

	if (!b)
		return -ENOMEM;
	en = readl(b + CPULDO_CTRL_0) & 0xff;
	v = readl(b + CPULDO_CTRL_1);
	iounmap(b);
	if (en != 0xff)
		return -ENODEV;
	sel = v & 0xf;
	switch (sel) {	/* vendor get_cur_volt_sram_l() */
	case 0: return 1050000;
	case 1: return 600000;
	case 2: return 700000;
	default: return 900000 + (sel - 3) * 25000;
	}
}

static int __init vproc_set_init(void)
{
	int before, after, vsram, ret;

	if (uv < 1000000 || uv > 1100000) {
		pr_err("vproc-set: uv=%d outside the 1.00-1.10 V test window\n", uv);
		return -EINVAL;
	}
	vsram = vsram_l_uv();
	if (vsram < 0) {
		pr_err("vproc-set: cannot read VSRAM_L (%d) - refusing\n", vsram);
		return vsram;
	}
	if (uv > vsram || vsram - uv > 300000) {
		pr_err("vproc-set: VSRAM_L %d uV vs target %d uV breaks the vendor rule - refusing\n",
		       vsram, uv);
		return -EINVAL;
	}

	vproc1 = regulator_get(NULL, "vproc1");
	if (IS_ERR(vproc1)) {
		ret = PTR_ERR(vproc1);
		pr_err("vproc-set: no vproc1 regulator: %d\n", ret);
		return ret;
	}

	before = regulator_get_voltage(vproc1);
	ret = regulator_set_voltage(vproc1, uv, uv);
	after = regulator_get_voltage(vproc1);
	pr_info("vproc-set: VPROC1 %d -> %d uV (target %d, ret %d), VSRAM_L %d uV\n",
		before, after, uv, ret, vsram);
	if (ret) {
		regulator_put(vproc1);
		return ret;
	}
	return 0;
}

static void __exit vproc_set_exit(void)
{
	pr_info("vproc-set: unloading; VPROC1 stays at %d uV\n",
		regulator_get_voltage(vproc1));
	regulator_put(vproc1);
}

module_init(vproc_set_init);
module_exit(vproc_set_exit);
MODULE_DESCRIPTION("Gemini PDA test: set VPROC1 via the DA9214 regulator");
MODULE_LICENSE("GPL");
