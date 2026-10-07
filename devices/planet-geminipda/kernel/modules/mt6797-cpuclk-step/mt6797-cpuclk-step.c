// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-cpuclk-step.c — TEST module: change the L cluster (cpu4-7) clock
 * through the clk framework, in exactly the sequence mainline
 * drivers/cpufreq/mediatek-cpufreq.c uses (and the vendor's non-hybrid
 * adjust_armpll_dds()):
 *
 *   clk_set_parent(cpu_l_sel, mainpll)   cluster runs at 1092 MHz meanwhile
 *   clk_set_rate(armpll_l, target)       CON1 write + CHG, 20 us settle
 *   clk_set_parent(cpu_l_sel, armpll_l)
 *
 * docs/cpu-dvfs.md, "first frequency step" (2026-10-06).
 *
 * Voltage first: refuses unless VPROC1 (DA9214 BUCKA, shared by LL, L, CCI)
 * is already at least the vendor SB voltage for the target:
 *   1209 MHz  1.000 V   (SB OPP9)
 *   1274 MHz  1.000 V   (what LK runs it at; for going back)
 *   1352 MHz  1.040 V   (SB OPP8)  -> load mt6797-vproc-set uv=1040000 first
 * The intermediate (MAINPLL 1092 MHz) is below all of these, so it is safe at
 * any of those voltages.
 *
 * Usage (root):  insmod mt6797-cpuclk-step.ko mhz=1352
 *                rmmod mt6797_cpuclk_step     # clock STAYS where it is
 *                insmod mt6797-cpuclk-step.ko mhz=1274   # back to LK's rate
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <dt-bindings/clock/mt6797-mcumixedsys.h>

#define CLK_APMIXED_MAINPLL	1	/* dt-bindings/clock/mt6797-clk.h */

static int mhz = 1352;
module_param(mhz, int, 0444);
MODULE_PARM_DESC(mhz, "L cluster target in MHz: 1209, 1274 or 1352");

static const struct { int mhz; int uv; } l_opp[] = {
	{ 1209, 1000000 },
	{ 1274, 1000000 },
	{ 1352, 1040000 },
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

static int __init cpuclk_step_init(void)
{
	struct clk *sel, *pll, *mainpll, *cpu_l;
	struct regulator *vproc1;
	int i, need_uv = -1, vproc_uv, ret;
	unsigned long before, mid, after;

	for (i = 0; i < ARRAY_SIZE(l_opp); i++)
		if (l_opp[i].mhz == mhz)
			need_uv = l_opp[i].uv;
	if (need_uv < 0) {
		pr_err("cpuclk-step: mhz=%d not allowed (1209, 1274, 1352)\n", mhz);
		return -EINVAL;
	}

	vproc1 = regulator_get(NULL, "vproc1");
	if (IS_ERR(vproc1)) {
		pr_err("cpuclk-step: no vproc1 regulator: %ld\n", PTR_ERR(vproc1));
		return PTR_ERR(vproc1);
	}
	vproc_uv = regulator_get_voltage(vproc1);
	regulator_put(vproc1);
	if (vproc_uv < need_uv) {
		pr_err("cpuclk-step: VPROC1 %d uV < %d uV needed for %d MHz - refusing (load mt6797-vproc-set uv=%d first)\n",
		       vproc_uv, need_uv, mhz, need_uv);
		return -EPERM;
	}

	sel = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_L_SEL);
	pll = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_ARMPLL_L);
	cpu_l = get_clk("mediatek,mt6797-mcumixedsys", CLK_MCU_L);
	mainpll = get_clk("mediatek,mt6797-apmixedsys", CLK_APMIXED_MAINPLL);
	if (IS_ERR(sel) || IS_ERR(pll) || IS_ERR(cpu_l) || IS_ERR(mainpll)) {
		pr_err("cpuclk-step: clocks missing (sel %ld pll %ld cpu_l %ld mainpll %ld)\n",
		       PTR_ERR_OR_ZERO(sel), PTR_ERR_OR_ZERO(pll),
		       PTR_ERR_OR_ZERO(cpu_l), PTR_ERR_OR_ZERO(mainpll));
		ret = -ENODEV;
		goto put;
	}
	if (!clk_is_match(clk_get_parent(sel), pll)) {
		pr_err("cpuclk-step: cpu_l_sel is not on armpll_l - refusing\n");
		ret = -EBUSY;
		goto put;
	}

	before = clk_get_rate(cpu_l);
	ret = clk_set_parent(sel, mainpll);
	if (ret) {
		pr_err("cpuclk-step: switch to mainpll failed: %d\n", ret);
		goto put;
	}
	mid = clk_get_rate(cpu_l);
	ret = clk_set_rate(pll, (unsigned long)mhz * 1000000);
	if (ret)	/* still switch back: the PLL is unchanged */
		pr_err("cpuclk-step: armpll_l set_rate failed: %d\n", ret);
	i = clk_set_parent(sel, pll);
	if (i) {
		pr_err("cpuclk-step: switch BACK to armpll_l failed: %d (cluster stays on mainpll 1092 MHz)\n", i);
		ret = ret ?: i;
	}
	after = clk_get_rate(cpu_l);
	pr_info("cpuclk-step: cpu_l %lu -> %lu (mainpll) -> %lu Hz, VPROC1 %d uV, ret %d\n",
		before, mid, after, vproc_uv, ret);
put:
	if (!IS_ERR(sel))
		clk_put(sel);
	if (!IS_ERR(pll))
		clk_put(pll);
	if (!IS_ERR(cpu_l))
		clk_put(cpu_l);
	if (!IS_ERR(mainpll))
		clk_put(mainpll);
	return ret;
}

static void __exit cpuclk_step_exit(void)
{
}

module_init(cpuclk_step_init);
module_exit(cpuclk_step_exit);
MODULE_DESCRIPTION("Gemini PDA test: step the L-cluster clock via the clk framework");
MODULE_LICENSE("GPL");
