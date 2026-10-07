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
 * v2 (2026-10-06, with the vsram_l regulator driver): VSRAM_L follows
 * VPROC1 through its own regulator, vendor set_cur_volt_extbuck() order:
 *   VSRAM_L target = clamp(VPROC1 + 100 mV, 1.00 V, 1.20 V)
 *   going up:   VSRAM_L first, then VPROC1
 *   going down: VPROC1 first, then VSRAM_L
 * so VSRAM >= VPROC and VSRAM - VPROC <= 300 mV hold at every step (one step
 * suffices inside the 1.00-1.15 V test window). This is the same rule
 * mediatek-cpufreq's sram-supply tracking will apply later.
 *
 * Usage (root):  insmod mt6797-vproc-set.ko uv=1040000   # 1.04 V / 1.14 V
 *                rmmod mt6797_vproc_set                  # voltages STAY
 *                insmod mt6797-vproc-set.ko uv=1000000   # LK's 1.00 / 1.10 V
 * Logs "vproc-set:" with before/after of both rails.
 */

#include <linux/kernel.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/regulator/consumer.h>

static int uv = 1040000;
module_param(uv, int, 0444);
MODULE_PARM_DESC(uv, "VPROC1 target in microvolts (1000000..1150000)");

#define UV_MIN		1000000
#define UV_MAX		1150000
#define SRAM_DIFF	100000
#define SRAM_MAX_DIFF	300000
#define SRAM_MIN	1000000
#define SRAM_MAX	1200000

static struct regulator *vproc1, *vsram;

static bool rule_ok(int proc, int sram)
{
	return sram >= proc && sram - proc <= SRAM_MAX_DIFF;
}

static int __init vproc_set_init(void)
{
	int proc0, sram0, proc1, sram1, sram_t, ret;

	if (uv < UV_MIN || uv > UV_MAX) {
		pr_err("vproc-set: uv=%d outside the %d-%d uV test window\n", uv, UV_MIN, UV_MAX);
		return -EINVAL;
	}
	sram_t = clamp(uv + SRAM_DIFF, SRAM_MIN, SRAM_MAX);

	vproc1 = regulator_get(NULL, "vproc1");
	if (IS_ERR(vproc1)) {
		pr_err("vproc-set: no vproc1 regulator: %ld\n", PTR_ERR(vproc1));
		return PTR_ERR(vproc1);
	}
	vsram = regulator_get(NULL, "vsram_l");
	if (IS_ERR(vsram)) {
		pr_err("vproc-set: no vsram_l regulator: %ld\n", PTR_ERR(vsram));
		regulator_put(vproc1);
		return PTR_ERR(vsram);
	}

	proc0 = regulator_get_voltage(vproc1);
	sram0 = regulator_get_voltage(vsram);
	if (proc0 < 0 || sram0 < 0 || !rule_ok(proc0, sram0)) {
		pr_err("vproc-set: start state VPROC1 %d / VSRAM_L %d uV not sane - refusing\n",
		       proc0, sram0);
		ret = -EINVAL;
		goto put;
	}

	if (uv >= proc0) {	/* up: SRAM first */
		ret = regulator_set_voltage(vsram, sram_t, SRAM_MAX);
		if (!ret)
			ret = regulator_set_voltage(vproc1, uv, uv);
	} else {		/* down: VPROC first */
		ret = regulator_set_voltage(vproc1, uv, uv);
		if (!ret)
			ret = regulator_set_voltage(vsram, sram_t, SRAM_MAX);
	}
	proc1 = regulator_get_voltage(vproc1);
	sram1 = regulator_get_voltage(vsram);
	pr_info("vproc-set: VPROC1 %d -> %d uV, VSRAM_L %d -> %d uV (target %d/%d, ret %d)%s\n",
		proc0, proc1, sram0, sram1, uv, sram_t, ret,
		rule_ok(proc1, sram1) ? "" : " RULE BROKEN");
	if (!ret)
		return 0;
put:
	regulator_put(vsram);
	regulator_put(vproc1);
	return ret;
}

static void __exit vproc_set_exit(void)
{
	pr_info("vproc-set: unloading; VPROC1 %d / VSRAM_L %d uV stay\n",
		regulator_get_voltage(vproc1), regulator_get_voltage(vsram));
	regulator_put(vsram);
	regulator_put(vproc1);
}

module_init(vproc_set_init);
module_exit(vproc_set_exit);
MODULE_DESCRIPTION("Gemini PDA test: set VPROC1 + VSRAM_L via their regulators");
MODULE_LICENSE("GPL");
