// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-a72clk-step.c — TEST module: change the A72 cluster (cpu8-9) clock
 * through the clk framework, in the sequence mediatek-cpufreq uses and the
 * vendor's non-hybrid adjust_armpll_dds() for MT_CPU_DVFS_B:
 *
 *   clk_set_parent(cpu_b_sel, mainpll)   A72s run at 1092 MHz meanwhile
 *   clk_set_rate(armpll_b, target)       ATF SMC 0xC20003B8 (output MHz)
 *   clk_set_parent(cpu_b_sel, armpll_b)
 *
 * docs/cpu-dvfs.md, "A72 step 1" (2026-10-07). Needs the boot image with the
 * A72 clocks in clk-mt6797-mcu (a72clk1) and both A72s online.
 *
 * Frequency only — VPROC2 stays where it is (LK/cl2-power: 1.000 V). The
 * module refuses unless VPROC2 >= the vendor SB-0119 voltage for the target:
 *    750 MHz  0.880 V  (what cl2-power/LK leave; for going back)
 *    845 MHz  0.880 V  (OPP13)
 *   1001 MHz  0.900 V  (OPP12)
 *   1131 MHz  0.930 V  (OPP11)
 *   1378 MHz  0.980 V  (OPP9)
 *   1495 MHz  1.000 V  (OPP8)   <- highest point at today's 1.000 V
 * The intermediate (MAINPLL 1092 MHz) is below 1131 @ 0.93 V, so it is safe
 * at 1.000 V.
 *
 * Usage (root):  insmod mt6797-a72clk-step.ko mhz=1001
 *                rmmod mt6797_a72clk_step      # clock STAYS where it is
 *                ./cpumhz                      # measure (cpu8/9 column)
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/cpumask.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <dt-bindings/clock/mt6797-mcumixedsys.h>

#define CLK_APMIXED_MAINPLL	1	/* dt-bindings/clock/mt6797-clk.h */

static int mhz = 1001;
module_param(mhz, int, 0444);
MODULE_PARM_DESC(mhz, "A72 target in MHz: 750, 845, 1001, 1131, 1378 or 1495");

static const struct { int mhz; int uv; } b_opp[] = {
	{  750,  880000 },
	{  845,  880000 },
	{ 1001,  900000 },
	{ 1131,  930000 },
	{ 1378,  980000 },
	{ 1495, 1000000 },
};

static struct clk *get_clk(const char *compat, int id)
{
	struct of_phandle_args args = { .args_count = 1, .args = { id } };
	struct clk *clk;

	args.np = of_find_compatible_node(NULL, NULL, compat);
	if (!args.np)
		return ERR_PTR(-ENODEV);
	clk = of_clk_get_from_provider(&args);
	of_node_put(args.np);
	return clk;
}

static int __init a72clk_step_init(void)
{
	struct clk *sel, *pll, *mainpll, *cpu_b;
	struct regulator *vproc2;
	int i, need_uv = -1, vproc_uv, ret;
	unsigned long before, mid, after, pll_rate, pll_before, ceiling;

	for (i = 0; i < ARRAY_SIZE(b_opp); i++)
		if (b_opp[i].mhz == mhz)
			need_uv = b_opp[i].uv;
	if (need_uv < 0) {
		pr_err("a72clk-step: mhz=%d not allowed (750, 845, 1001, 1131, 1378, 1495)\n", mhz);
		return -EINVAL;
	}
	if (!cpu_online(8) || !cpu_online(9)) {
		pr_err("a72clk-step: cpu8/cpu9 not both online - refusing\n");
		return -EAGAIN;
	}

	vproc2 = regulator_get(NULL, "vproc2");
	if (IS_ERR(vproc2)) {
		pr_err("a72clk-step: no vproc2 regulator: %ld\n", PTR_ERR(vproc2));
		return PTR_ERR(vproc2);
	}
	vproc_uv = regulator_get_voltage(vproc2);
	regulator_put(vproc2);
	if (vproc_uv < need_uv) {
		pr_err("a72clk-step: VPROC2 %d uV < %d uV needed for %d MHz - refusing\n",
		       vproc_uv, need_uv, mhz);
		return -EPERM;
	}

	sel = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_B_SEL);
	pll = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_ARMPLL_B);
	cpu_b = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_B);
	mainpll = get_clk("mediatek,mt6797-apmixedsys", CLK_APMIXED_MAINPLL);
	if (IS_ERR(sel) || IS_ERR(pll) || IS_ERR(cpu_b) || IS_ERR(mainpll)) {
		pr_err("a72clk-step: clocks missing (sel %ld pll %ld cpu_b %ld mainpll %ld) - old boot image?\n",
		       PTR_ERR_OR_ZERO(sel), PTR_ERR_OR_ZERO(pll),
		       PTR_ERR_OR_ZERO(cpu_b), PTR_ERR_OR_ZERO(mainpll));
		ret = -ENODEV;
		goto put;
	}
	/*
	 * No parent check here: on the a72clk1 boot image the clk core cached
	 * cpu_b_sel's parent while the A72 cluster was still off (MUXSEL B
	 * field 0 = clk26m), so the cached parent is stale. The clock driver
	 * itself refuses to retune armpll_b while the hardware mux is on it.
	 */
	pr_info("a72clk-step: cpu_b_sel cached parent %s\n",
		__clk_get_name(clk_get_parent(sel)) ?: "?");

	before = clk_get_rate(cpu_b);
	pll_before = clk_get_rate(pll);
	/* the A72s may only go back to a rate they ran at or that we asked for */
	ceiling = max_t(unsigned long, pll_before, (unsigned long)mhz * 1000000) + 2000000;
	ret = clk_set_parent(sel, mainpll);
	if (ret) {
		pr_err("a72clk-step: switch to mainpll failed: %d\n", ret);
		goto put;
	}
	mid = clk_get_rate(cpu_b);
	ret = clk_set_rate(pll, (unsigned long)mhz * 1000000);
	if (ret)	/* still switch back */
		pr_err("a72clk-step: armpll_b set_rate failed: %d\n", ret);
	pll_rate = clk_get_rate(pll);
	if (pll_rate < 250000000 || pll_rate > ceiling) {
		/* never put the A72s on a PLL we did not ask for */
		pr_err("a72clk-step: armpll_b reads %lu Hz - staying on mainpll (1092 MHz)\n",
		       pll_rate);
		ret = ret ?: -EIO;
		after = clk_get_rate(cpu_b);
		goto report;
	}
	i = clk_set_parent(sel, pll);
	if (i) {
		pr_err("a72clk-step: switch BACK to armpll_b failed: %d (A72s stay on mainpll 1092 MHz)\n", i);
		ret = ret ?: i;
	}
	after = clk_get_rate(cpu_b);
report:
	pr_info("a72clk-step: cpu_b %lu -> %lu (mainpll) -> %lu Hz, armpll_b %lu Hz, VPROC2 %d uV, ret %d\n",
		before, mid, after, pll_rate, vproc_uv, ret);
put:
	if (!IS_ERR(sel))
		clk_put(sel);
	if (!IS_ERR(pll))
		clk_put(pll);
	if (!IS_ERR(cpu_b))
		clk_put(cpu_b);
	if (!IS_ERR(mainpll))
		clk_put(mainpll);
	return ret;
}

static void __exit a72clk_step_exit(void)
{
}

module_init(a72clk_step_init);
module_exit(a72clk_step_exit);
MODULE_DESCRIPTION("Gemini PDA test: step the A72 clock via the clk framework");
MODULE_LICENSE("GPL");
