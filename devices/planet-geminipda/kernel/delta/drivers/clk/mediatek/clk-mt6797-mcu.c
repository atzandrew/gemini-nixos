// SPDX-License-Identifier: GPL-2.0
/*
 * clk-mt6797-mcu.c — MT6797 MCUMIXEDSYS CPU clocks (A53 clusters + CCI).
 * Gemini PDA, 2026-10-06. Version 1: READ-ONLY (rates/parents are read
 * from hardware; no set ops). docs/cpu-dvfs.md.
 *
 * Per cluster (LL = cpu0-3, L = cpu4-7, CCI = MCSI bus):
 *
 *   clk26m -> armpll_X (ARMCAXPLLn: CON1 = CHG bit31 | posdiv 26:24 | DDS 20:0,
 *                       VCO = 26 MHz * DDS / 2^14, out = VCO >> posdiv)
 *   {clk26m, armpll_X, mainpll, univpll} -> cpu_X_sel (ARMPLLDIV_MUXSEL)
 *   cpu_X_sel -> cpu_X (ARMPLLDIV_CKDIV code: 8 /1, 9 3/4, 10 /2, 11 /4,
 *                       17 4/5, 18 3/5, 19 2/5, anything else /1)
 *
 * Source: vendor gemian/gemini-linux-kernel-3.18
 * drivers/misc/mediatek/base/power/mt6797/mt_cpufreq.c (_cpu_freq_calc,
 * _cpu_clock_switch, adjust_clkdiv) and mt_clkmgr.h.
 *
 * ACCESS PROTOCOL: the vendor's "Everest 0x1001AXXX bus access issue" —
 * ATF, the CPU-DVFS sequencer (CSPM) and the kernel must not access
 * MCUMIXEDSYS concurrently. Every access here is done like the vendor's
 * mt6797_0x1001AXXX_lock(): IRQs off + spinlock, take HW semaphore 3 /
 * master 0 (CSPM 0x11015440: write 1, read back bit0; 2 ms timeout), 200 ns
 * before each access, release by writing 1 again. The CSPM block needs its
 * bus CG enabled for the semaphore (POWERON_CONFIG_EN bit0, key
 * 0x0b160001 — the vendor writes this at core_initcall); LK leaves it on.
 *
 * The CSPM block is mapped, not requested (shared with nothing in Linux
 * today, but owned by the vendor DVFS processor).
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/mt6797-mcumixedsys.h>

#define ARMCAXPLL_CON1(n)	(0x204 + 0x10 * (n))
#define ARMPLLDIV_MUXSEL	0x270
#define ARMPLLDIV_CKDIV		0x274

#define CSPM_POWERON_CONFIG_EN	0x000
#define CSPM_SEMA3_M0		0x440
#define CSPM_CG_KEY_EN		0x0b160001
#define SEMA_TRIES		200	/* x 10 us = 2 ms (vendor SEMA_GET_TIMEOUT) */

struct mcu_clk_ctx {
	void __iomem *base;
	void __iomem *cspm;
	spinlock_t lock;
};

static struct mcu_clk_ctx *mcu_ctx;

/* vendor mt6797_0x1001AXXX_reg_read(), without the BUG_ON on timeout */
static int mcu_read(struct mcu_clk_ctx *c, u32 off, u32 *val)
{
	unsigned long flags;
	int i, ret = -ETIMEDOUT;

	spin_lock_irqsave(&c->lock, flags);
	for (i = 0; i < SEMA_TRIES; i++) {
		writel(0x1, c->cspm + CSPM_SEMA3_M0);
		if (readl(c->cspm + CSPM_SEMA3_M0) & 0x1) {
			ret = 0;
			break;
		}
		udelay(10);
	}
	if (!ret) {
		ndelay(200);	/* vendor: "DE workaround, for first read after sequential write" */
		*val = readl(c->base + off);
		if (readl(c->cspm + CSPM_SEMA3_M0) & 0x1)
			writel(0x1, c->cspm + CSPM_SEMA3_M0);
	}
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret)
		pr_warn_ratelimited("clk-mt6797-mcu: HW semaphore timeout reading 0x%03x\n", off);
	return ret;
}

/* ---- PLL ---------------------------------------------------------------- */

struct mcu_pll {
	struct clk_hw hw;
	struct mcu_clk_ctx *c;
	int idx;
};
#define to_mcu_pll(_hw) container_of(_hw, struct mcu_pll, hw)

static unsigned long mcu_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct mcu_pll *p = to_mcu_pll(hw);
	u32 con1;
	u64 vco;

	if (mcu_read(p->c, ARMCAXPLL_CON1(p->idx), &con1))
		return 0;
	vco = ((u64)parent_rate * (con1 & GENMASK(20, 0))) >> 14;
	return (unsigned long)(vco >> ((con1 >> 24) & 0x7));
}

static const struct clk_ops mcu_pll_ops = {
	.recalc_rate = mcu_pll_recalc_rate,
};

/* ---- mux (source select) ------------------------------------------------ */

struct mcu_mux {
	struct clk_hw hw;
	struct mcu_clk_ctx *c;
	u8 shift;
};
#define to_mcu_mux(_hw) container_of(_hw, struct mcu_mux, hw)

static u8 mcu_mux_get_parent(struct clk_hw *hw)
{
	struct mcu_mux *m = to_mcu_mux(hw);
	u32 v;

	if (mcu_read(m->c, ARMPLLDIV_MUXSEL, &v))
		return 1;	/* assume armpll */
	return (v >> m->shift) & 0x3;
}

static const struct clk_ops mcu_mux_ops = {
	.get_parent = mcu_mux_get_parent,
	.determine_rate = __clk_mux_determine_rate,
};

/* ---- divider ------------------------------------------------------------ */

struct mcu_div {
	struct clk_hw hw;
	struct mcu_clk_ctx *c;
	u8 shift;
};
#define to_mcu_div(_hw) container_of(_hw, struct mcu_div, hw)

static unsigned long mcu_div_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct mcu_div *d = to_mcu_div(hw);
	u32 v, code;
	unsigned int num = 1, den = 1;

	if (mcu_read(d->c, ARMPLLDIV_CKDIV, &v))
		return parent_rate;
	code = (v >> d->shift) & 0x1f;
	switch (code) {	/* vendor _cpu_freq_calc() */
	case 9:  num = 3; den = 4; break;
	case 10: num = 1; den = 2; break;
	case 11: num = 1; den = 4; break;
	case 17: num = 4; den = 5; break;
	case 18: num = 3; den = 5; break;
	case 19: num = 2; den = 5; break;
	default: break;	/* 8 = /1; LK leaves 0 = /1 */
	}
	return (unsigned long)div_u64((u64)parent_rate * num, den);
}

static const struct clk_ops mcu_div_ops = {
	.recalc_rate = mcu_div_recalc_rate,
};

/* ---- registration ------------------------------------------------------- */

static const char * const pll_names[3] = { "armpll_ll", "armpll_l", "armpll_cci" };
static const char * const sel_names[3] = { "cpu_ll_sel", "cpu_l_sel", "cpu_cci_sel" };
static const char * const out_names[3] = { "cpu_ll", "cpu_l", "cpu_cci" };
static const u8 mux_shift[3] = { 2, 4, 6 };
static const u8 div_shift[3] = { 5, 10, 15 };

static int mcu_register(struct device *dev, struct clk_hw *hw, const char *name,
			const struct clk_ops *ops, const char * const *parents, u8 nparents)
{
	struct clk_init_data init = {
		.name = name,
		.ops = ops,
		.parent_names = parents,
		.num_parents = nparents,
		.flags = CLK_GET_RATE_NOCACHE,
	};

	hw->init = &init;
	return devm_clk_hw_register(dev, hw);
}

static int mt6797_mcu_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct mcu_clk_ctx *c;
	struct resource *res;
	const char *mux_parents[3][4];
	int i, ret;
	u32 v;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	data = devm_kzalloc(dev, struct_size(data, hws, CLK_MCU_NR_CLK), GFP_KERNEL);
	if (!c || !data)
		return -ENOMEM;
	spin_lock_init(&c->lock);

	c->base = devm_platform_ioremap_resource_byname(pdev, "mcumixed");
	if (IS_ERR(c->base))
		return PTR_ERR(c->base);
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "cspm");
	if (!res)
		return -EINVAL;
	c->cspm = devm_ioremap(dev, res->start, resource_size(res));	/* not requested */
	if (!c->cspm)
		return -ENOMEM;

	v = readl(c->cspm + CSPM_POWERON_CONFIG_EN);
	if (!(v & 1)) {
		writel(CSPM_CG_KEY_EN, c->cspm + CSPM_POWERON_CONFIG_EN);	/* vendor core_initcall */
		dev_info(dev, "CSPM bus CG was off (0x%08x), enabled\n", v);
	}

	data->num = CLK_MCU_NR_CLK;
	for (i = 0; i < 3; i++) {
		struct mcu_pll *p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
		struct mcu_mux *m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
		struct mcu_div *d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
		static const char * const clk26m[] = { "clk26m" };

		if (!p || !m || !d)
			return -ENOMEM;
		p->c = m->c = d->c = c;
		p->idx = i;
		m->shift = mux_shift[i];
		d->shift = div_shift[i];

		ret = mcu_register(dev, &p->hw, pll_names[i], &mcu_pll_ops, clk26m, 1);
		if (ret)
			return ret;

		mux_parents[i][0] = "clk26m";
		mux_parents[i][1] = pll_names[i];
		mux_parents[i][2] = "mainpll";
		mux_parents[i][3] = "univpll";
		ret = mcu_register(dev, &m->hw, sel_names[i], &mcu_mux_ops, mux_parents[i], 4);
		if (ret)
			return ret;

		ret = mcu_register(dev, &d->hw, out_names[i], &mcu_div_ops, &sel_names[i], 1);
		if (ret)
			return ret;

		data->hws[CLK_MCU_ARMPLL_LL + i] = &p->hw;
		data->hws[CLK_MCU_LL_SEL + i] = &m->hw;
		data->hws[CLK_MCU_LL + i] = &d->hw;
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;
	mcu_ctx = c;

	for (i = 0; i < 3; i++)
		dev_info(dev, "%s: %lu Hz (%s %lu Hz)\n", out_names[i],
			 clk_hw_get_rate(data->hws[CLK_MCU_LL + i]), pll_names[i],
			 clk_hw_get_rate(data->hws[CLK_MCU_ARMPLL_LL + i]));
	return 0;
}

static const struct of_device_id mt6797_mcu_clk_of_match[] = {
	{ .compatible = "mediatek,mt6797-mcumixedsys" },
	{ }
};

static struct platform_driver mt6797_mcu_clk_driver = {
	.probe = mt6797_mcu_clk_probe,
	.driver = {
		.name = "clk-mt6797-mcu",
		.of_match_table = mt6797_mcu_clk_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6797_mcu_clk_driver);
