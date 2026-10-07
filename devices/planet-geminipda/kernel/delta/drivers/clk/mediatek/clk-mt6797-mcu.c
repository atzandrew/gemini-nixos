// SPDX-License-Identifier: GPL-2.0
/*
 * clk-mt6797-mcu.c — MT6797 MCUMIXEDSYS CPU clocks (A53 clusters + CCI).
 * Gemini PDA, 2026-10-06. docs/cpu-dvfs.md.
 *   v1: read-only.
 *   v2: write support in the mainline MediaTek-cpufreq shape: the cpu_X_sel
 *       mux can be reparented (to "mainpll" = the intermediate clock), and
 *       armpll_X accepts set_rate ONLY while its cluster is not running from
 *       it. The caller does: mux -> mainpll, set armpll rate, mux -> armpll,
 *       which is the vendor's non-hybrid adjust_armpll_dds() order and what
 *       drivers/cpufreq/mediatek-cpufreq.c does with "cpu"/"intermediate".
 *       Voltage is NOT this driver's business (OPP / cpufreq / the caller);
 *       the only guard here is a per-cluster ceiling = the SB speed-bin max.
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
 *
 * MUX SWITCH (vendor _cpu_clock_switch): TOPCKGEN CLK_MISC_CFG_0[5:4] = 3
 * before any cluster runs from MAINPLL/UNIVPLL, = 0 when back on ARMPLL.
 * Those bits are shared by all four clusters (B = A72 too), so unlike the
 * vendor (which only ever had one cluster off-PLL at a time) they are cleared
 * only when NO cluster field still selects MAINPLL/UNIVPLL. TOPCKGEN belongs
 * to clk-mt6797, so that one word is mapped, not requested ("misc-cfg").
 *
 * PLL WRITE (vendor adjust_armpll_dds): CON1 = (CON1 & ~(26:24 | 20:0)) |
 * posdiv << 24 | DDS | CHG(bit31), then 20 us PLL settle before anyone may
 * switch back. DDS = (VCO << 14) / 26 MHz. posdiv is /1 down to 1092 MHz
 * and /2 below (exactly the vendor SB/FY A53 tables: LL 1118 /1, 1014 /2;
 * L 1092 /1, 962 /2). The vendor never uses /4, so neither do we: the
 * lowest PLL rate is 546 MHz; lower OPPs need CKDIV (/2, /4), not done yet. Writing posdiv and DDS in one go is fine
 * because the cluster is not on the PLL while it changes (the vendor's
 * separate adjust_posdiv() also switches to MAINPLL around its write).
 * CKDIV (the cpu_X divider) stays read-only: every OPP >= 559 MHz is /1.
 *
 * A72 CLUSTER (B = cpu8-9, added 2026-10-07): the same mux/divider pair in
 * MCUMIXEDSYS (MUXSEL[1:0], CKDIV[4:0]), but its PLL is the "iDVFS" PLL in
 * MCUCFG2 (CON0 0x102224a0: bit0 enable, 14:12 posdiv; CON1 0x102224a4:
 * PCW, VCO = 26 MHz * PCW / 2^24), which the kernel may only program
 * through ATF. Vendor non-hybrid path (mt_cpufreq.c adjust_armpll_dds() for
 * MT_CPU_DVFS_B, mt_idvfs.c BigiDVFSPllSetFreq()): mux -> MAINPLL, SiP SMC
 * 0xC20003B8 (BIGIDVFSPLLSETFREQ, arg = output MHz; ATF picks PCW and
 * posdiv), 20 us, mux -> ARMPLL. Read back through the secure-read SMC
 * 0xC200035F. The vendor only touches it while an A72 is online, so this
 * driver does the same (cpus = every "arm,cortex-a72" CPU node); while the
 * cluster is down the PLL reports its last known rate.
 */

#include <linux/arm-smccc.h>
#include <linux/clk-provider.h>
#include <linux/cpumask.h>
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

#define CON1_CHG		BIT(31)
#define CON1_POSDIV		GENMASK(26, 24)
#define CON1_DDS		GENMASK(20, 0)
#define MUX_ARMPLL		1
#define MUX_MAINPLL		2
#define MUX_UNIVPLL		3
#define MISC_CFG_MASK		GENMASK(5, 4)
#define PLL_SETTLE_US		20	/* vendor PLL_SETTLE_TIME */
#define VCO_MIN_HZ		1092000000UL	/* lowest vendor /1 rate */
#define PLL_MIN_HZ		(VCO_MIN_HZ / 2)

#define CSPM_POWERON_CONFIG_EN	0x000
#define CSPM_SEMA3_M0		0x440
#define CSPM_CG_KEY_EN		0x0b160001
#define SEMA_TRIES		200	/* x 10 us = 2 ms (vendor SEMA_GET_TIMEOUT) */

/* A72 iDVFS PLL (vendor mt_idvfs.h / mt_idvfs.c) */
#define SIP_IDVFS_READ		0xC200035FUL
#define SIP_IDVFS_PLL_SETFREQ	0xC20003B8UL
#define B_PLL_CON0		0x102224a0UL
#define B_PLL_CON1		0x102224a4UL
#define B_PLL_MIN_HZ		250000000UL	/* BigiDVFSPllSetFreq() range */
#define B_PLL_MAX_HZ		2587000000UL	/* SB/TT 0119 OPP0 */
#define B_MUX_SHIFT		0
#define B_DIV_SHIFT		0

struct mcu_clk_ctx {
	struct cpumask b_cpus;		/* the A72s; empty = no B clocks */
	void __iomem *base;
	void __iomem *cspm;
	void __iomem *misc_cfg;		/* TOPCKGEN CLK_MISC_CFG_0, one word */
	spinlock_t lock;
};

/* SB speed-bin maximum per cluster (vendor *_SB OPP0), Hz */
static const unsigned long pll_max_hz[3] = { 1547000000, 2002000000, 988000000 };

static const char * const pll_names[3] = { "armpll_ll", "armpll_l", "armpll_cci" };
static const char * const sel_names[3] = { "cpu_ll_sel", "cpu_l_sel", "cpu_cci_sel" };
static const char * const out_names[3] = { "cpu_ll", "cpu_l", "cpu_cci" };
static const u8 mux_shift[3] = { 2, 4, 6 };
static const u8 div_shift[3] = { 5, 10, 15 };

static int mcu_sema_get(struct mcu_clk_ctx *c)
{
	int i;

	for (i = 0; i < SEMA_TRIES; i++) {
		writel(0x1, c->cspm + CSPM_SEMA3_M0);
		if (readl(c->cspm + CSPM_SEMA3_M0) & 0x1)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static void mcu_sema_put(struct mcu_clk_ctx *c)
{
	if (readl(c->cspm + CSPM_SEMA3_M0) & 0x1)
		writel(0x1, c->cspm + CSPM_SEMA3_M0);
}

/* vendor mt6797_0x1001AXXX_reg_read(), without the BUG_ON on timeout */
static int mcu_read(struct mcu_clk_ctx *c, u32 off, u32 *val)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&c->lock, flags);
	ret = mcu_sema_get(c);
	if (!ret) {
		ndelay(200);	/* vendor: "DE workaround, for first read after sequential write" */
		*val = readl(c->base + off);
		mcu_sema_put(c);
	}
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret)
		pr_warn_ratelimited("clk-mt6797-mcu: HW semaphore timeout reading 0x%03x\n", off);
	return ret;
}

/*
 * Read-modify-write under ONE semaphore hold (the vendor's
 * cpufreq_write_mask_armpll() takes it twice). Caller holds c->lock.
 * Returns the value written in *out (if non-NULL).
 */
static int mcu_rmw_locked(struct mcu_clk_ctx *c, u32 off, u32 mask, u32 val, u32 *out)
{
	u32 v;
	int ret = mcu_sema_get(c);

	if (ret) {
		pr_err("clk-mt6797-mcu: HW semaphore timeout writing 0x%03x\n", off);
		return ret;
	}
	ndelay(200);
	v = (readl(c->base + off) & ~mask) | (val & mask);
	writel(v, c->base + off);
	ndelay(200);	/* vendor mt6797_0x1001AXXX_reg_write() */
	mcu_sema_put(c);
	if (out)
		*out = v;
	return 0;
}

static bool muxsel_off_pll(u32 muxsel)
{
	int shift;

	for (shift = 0; shift <= 6; shift += 2) {	/* B, LL, L, CCI */
		u32 f = (muxsel >> shift) & 0x3;

		if (f == MUX_MAINPLL || f == MUX_UNIVPLL)
			return true;
	}
	return false;
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

static unsigned int mcu_pll_posdiv_shift(unsigned long rate)
{
	return rate >= VCO_MIN_HZ ? 0 : 1;	/* /1 or /2 */
}

static u32 mcu_pll_dds(unsigned long rate, unsigned int shift, unsigned long parent)
{
	return (u32)div_u64((u64)rate << (14 + shift), parent) & CON1_DDS;
}

static unsigned long mcu_pll_dds_rate(u32 dds, unsigned int shift, unsigned long parent)
{
	return (unsigned long)((((u64)parent * dds) >> 14) >> shift);
}

static int mcu_pll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	struct mcu_pll *p = to_mcu_pll(hw);
	unsigned long rate = clamp(req->rate, PLL_MIN_HZ, pll_max_hz[p->idx]);
	unsigned int shift = mcu_pll_posdiv_shift(rate);

	if (!req->best_parent_rate)
		return -EINVAL;
	req->rate = mcu_pll_dds_rate(mcu_pll_dds(rate, shift, req->best_parent_rate),
				     shift, req->best_parent_rate);
	return 0;
}

static int mcu_pll_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct mcu_pll *p = to_mcu_pll(hw);
	struct mcu_clk_ctx *c = p->c;
	unsigned int shift;
	unsigned long flags;
	u32 muxsel, con1 = 0, dds;
	int ret;

	if (rate > pll_max_hz[p->idx] || rate < PLL_MIN_HZ || !parent_rate)
		return -EINVAL;
	shift = mcu_pll_posdiv_shift(rate);
	dds = mcu_pll_dds(rate, shift, parent_rate);

	spin_lock_irqsave(&c->lock, flags);
	/* refuse to retune a PLL its cluster is running from */
	ret = mcu_sema_get(c);
	if (!ret) {
		ndelay(200);
		muxsel = readl(c->base + ARMPLLDIV_MUXSEL);
		mcu_sema_put(c);
		if (((muxsel >> mux_shift[p->idx]) & 0x3) == MUX_ARMPLL)
			ret = -EBUSY;
	}
	if (!ret)
		ret = mcu_rmw_locked(c, ARMCAXPLL_CON1(p->idx),
				     CON1_CHG | CON1_POSDIV | CON1_DDS,
				     CON1_CHG | (shift << 24) | dds, &con1);
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret) {
		pr_err("clk-mt6797-mcu: %s -> %lu Hz refused: %d\n",
		       clk_hw_get_name(hw), rate, ret);
		return ret;
	}
	udelay(PLL_SETTLE_US);	/* before anyone switches the cluster back */
	pr_debug("clk-mt6797-mcu: %s -> %lu Hz (CON1 0x%08x, /%u)\n",
		clk_hw_get_name(hw), rate, con1, 1u << shift);
	return 0;
}

static const struct clk_ops mcu_pll_ops = {
	.recalc_rate = mcu_pll_recalc_rate,
	.determine_rate = mcu_pll_determine_rate,
	.set_rate = mcu_pll_set_rate,
};

/* ---- A72 iDVFS PLL (through ATF) ---------------------------------------- */

struct mcu_bpll {
	struct clk_hw hw;
	struct mcu_clk_ctx *c;
	unsigned long last_rate;
};
#define to_mcu_bpll(_hw) container_of(_hw, struct mcu_bpll, hw)

static bool mcu_b_cluster_up(struct mcu_clk_ctx *c)
{
	return cpumask_intersects(&c->b_cpus, cpu_online_mask);
}

static unsigned long mcu_sip_read(unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SIP_IDVFS_READ, addr, 0, 0, 0, 0, 0, 0, &res);
	return res.a0;
}

static unsigned long mcu_bpll_read_rate(void)
{
	u32 con0 = mcu_sip_read(B_PLL_CON0);
	u32 pcw = mcu_sip_read(B_PLL_CON1) & GENMASK(30, 0);
	u64 vco = (26000000ULL * pcw) >> 24;

	if (!(con0 & BIT(0)))
		return 0;
	return (unsigned long)(vco >> ((con0 >> 12) & 0x7));
}

static unsigned long mcu_bpll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct mcu_bpll *b = to_mcu_bpll(hw);

	if (mcu_b_cluster_up(b->c))
		b->last_rate = mcu_bpll_read_rate();
	return b->last_rate;
}

static int mcu_bpll_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	/* ATF takes whole MHz */
	req->rate = rounddown(clamp(req->rate, B_PLL_MIN_HZ, B_PLL_MAX_HZ), 1000000);
	return 0;
}

static int mcu_bpll_set_rate(struct clk_hw *hw, unsigned long rate, unsigned long parent_rate)
{
	struct mcu_bpll *b = to_mcu_bpll(hw);
	struct mcu_clk_ctx *c = b->c;
	struct arm_smccc_res res;
	unsigned long flags, got;
	u32 muxsel;
	int ret;

	if (rate > B_PLL_MAX_HZ || rate < B_PLL_MIN_HZ)
		return -EINVAL;

	/*
	 * No cpus_read_lock() here: cpufreq calls this with the policy rwsem
	 * held, and CPU hotplug takes them in the other order. cpufreq only
	 * retunes an online policy, which is what this check is for.
	 */
	if (!mcu_b_cluster_up(c)) {
		ret = -EAGAIN;
		goto out;
	}

	/* refuse to retune the PLL the A72s are running from */
	spin_lock_irqsave(&c->lock, flags);
	ret = mcu_sema_get(c);
	if (!ret) {
		ndelay(200);
		muxsel = readl(c->base + ARMPLLDIV_MUXSEL);
		mcu_sema_put(c);
		if (((muxsel >> B_MUX_SHIFT) & 0x3) == MUX_ARMPLL)
			ret = -EBUSY;
	}
	spin_unlock_irqrestore(&c->lock, flags);
	if (ret)
		goto out;

	arm_smccc_smc(SIP_IDVFS_PLL_SETFREQ, rate / 1000000, 0, 0, 0, 0, 0, 0, &res);
	udelay(PLL_SETTLE_US);
	got = mcu_bpll_read_rate();
	b->last_rate = got;
	/* PCW resolution: 26 MHz / 2^24 per step, so allow 1 MHz of slack */
	if ((long)res.a0 < 0 || abs((long)got - (long)rate) > 1000000) {
		pr_err("clk-mt6797-mcu: armpll_b -> %lu Hz: SMC 0x%lx, PLL now %lu Hz\n",
		       rate, res.a0, got);
		ret = -EIO;
	}
out:
	if (ret)
		pr_err("clk-mt6797-mcu: armpll_b -> %lu Hz refused: %d\n", rate, ret);
	else
		pr_debug("clk-mt6797-mcu: armpll_b -> %lu Hz\n", got);
	return ret;
}

static const struct clk_ops mcu_bpll_ops = {
	.recalc_rate = mcu_bpll_recalc_rate,
	.determine_rate = mcu_bpll_determine_rate,
	.set_rate = mcu_bpll_set_rate,
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

static int mcu_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct mcu_mux *m = to_mcu_mux(hw);
	struct mcu_clk_ctx *c = m->c;
	bool off_pll = index == MUX_MAINPLL || index == MUX_UNIVPLL;
	unsigned long flags;
	u32 muxsel;
	int ret;

	if (index > MUX_UNIVPLL)
		return -EINVAL;

	spin_lock_irqsave(&c->lock, flags);
	if (off_pll)	/* vendor: CLK_MISC_CFG_0[5:4] = 3 BEFORE the switch */
		writel(readl(c->misc_cfg) | MISC_CFG_MASK, c->misc_cfg);
	ret = mcu_rmw_locked(c, ARMPLLDIV_MUXSEL, 0x3 << m->shift, index << m->shift, &muxsel);
	if (!ret && !muxsel_off_pll(muxsel))	/* = 0 AFTER, once nobody needs it */
		writel(readl(c->misc_cfg) & ~MISC_CFG_MASK, c->misc_cfg);
	spin_unlock_irqrestore(&c->lock, flags);

	if (ret)
		return ret;
	pr_debug("clk-mt6797-mcu: %s -> %s (MUXSEL 0x%08x)\n", clk_hw_get_name(hw),
		clk_hw_get_name(clk_hw_get_parent_by_index(hw, index)) ?: "?", muxsel);
	return 0;
}

static const struct clk_ops mcu_mux_ops = {
	.get_parent = mcu_mux_get_parent,
	.set_parent = mcu_mux_set_parent,
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


static int mcu_register(struct device *dev, struct clk_hw *hw, const char *name,
			const struct clk_ops *ops, const char * const *parents, u8 nparents,
			unsigned long flags)
{
	struct clk_init_data init = {
		.name = name,
		.ops = ops,
		.parent_names = parents,
		.num_parents = nparents,
		.flags = CLK_GET_RATE_NOCACHE | flags,
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
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "misc-cfg");
	if (!res)
		return dev_err_probe(dev, -EINVAL, "no misc-cfg (TOPCKGEN CLK_MISC_CFG_0)\n");
	c->misc_cfg = devm_ioremap(dev, res->start, 4);	/* topckgen's: not requested */
	if (!c->misc_cfg)
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

		ret = mcu_register(dev, &p->hw, pll_names[i], &mcu_pll_ops, clk26m, 1, 0);
		if (ret)
			return ret;

		mux_parents[i][0] = "clk26m";
		mux_parents[i][1] = pll_names[i];
		mux_parents[i][2] = "mainpll";
		mux_parents[i][3] = "univpll";
		ret = mcu_register(dev, &m->hw, sel_names[i], &mcu_mux_ops, mux_parents[i], 4,
				   CLK_SET_RATE_NO_REPARENT);
		if (ret)
			return ret;

		ret = mcu_register(dev, &d->hw, out_names[i], &mcu_div_ops, &sel_names[i], 1, 0);
		if (ret)
			return ret;

		data->hws[CLK_MCU_ARMPLL_LL + i] = &p->hw;
		data->hws[CLK_MCU_LL_SEL + i] = &m->hw;
		data->hws[CLK_MCU_LL + i] = &d->hw;
	}

	/* A72 cluster: the CPUs it clocks are every "arm,cortex-a72" CPU node */
	for_each_possible_cpu(i) {
		struct device_node *np = of_get_cpu_node(i, NULL);

		if (np && of_device_is_compatible(np, "arm,cortex-a72"))
			cpumask_set_cpu(i, &c->b_cpus);
		of_node_put(np);
	}
	if (!cpumask_empty(&c->b_cpus)) {
		static const char * const clk26m[] = { "clk26m" };
		static const char * const b_parents[] = {
			"clk26m", "armpll_b", "mainpll", "univpll" };
		static const char * const b_sel[] = { "cpu_b_sel" };
		struct mcu_bpll *b = devm_kzalloc(dev, sizeof(*b), GFP_KERNEL);
		struct mcu_mux *m = devm_kzalloc(dev, sizeof(*m), GFP_KERNEL);
		struct mcu_div *d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);

		if (!b || !m || !d)
			return -ENOMEM;
		b->c = m->c = d->c = c;
		m->shift = B_MUX_SHIFT;
		d->shift = B_DIV_SHIFT;

		ret = mcu_register(dev, &b->hw, "armpll_b", &mcu_bpll_ops, clk26m, 1, 0);
		if (!ret)
			ret = mcu_register(dev, &m->hw, "cpu_b_sel", &mcu_mux_ops, b_parents, 4,
					   CLK_SET_RATE_NO_REPARENT);
		if (!ret)
			ret = mcu_register(dev, &d->hw, "cpu_b", &mcu_div_ops, b_sel, 1, 0);
		if (ret)
			return ret;
		data->hws[CLK_MCU_ARMPLL_B] = &b->hw;
		data->hws[CLK_MCU_B_SEL] = &m->hw;
		data->hws[CLK_MCU_B] = &d->hw;
	} else {
		for (i = CLK_MCU_ARMPLL_B; i <= CLK_MCU_B; i++)
			data->hws[i] = ERR_PTR(-ENOENT);
	}

	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;
	if (!mcu_read(c, ARMPLLDIV_MUXSEL, &v))
		dev_info(dev, "MUXSEL 0x%08x CLK_MISC_CFG_0 0x%08x\n", v, readl(c->misc_cfg));
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
