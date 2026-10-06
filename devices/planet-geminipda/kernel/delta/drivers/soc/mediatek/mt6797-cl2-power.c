// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-cl2-power.c — power on the MT6797 A72 cluster (cluster 2,
 * cpu8/cpu9) and bring its CPUs online. Gemini PDA (2026-10-06).
 *
 * On MT6797 the A72 cluster runs from an EXTERNAL buck (DA9214 BUCKB =
 * VPROC2) that is off at boot, and ATF's PSCI CPU_ON does not power it.
 * The kernel boots with maxcpus=8, so cpu8/9 are present but not started;
 * this driver powers the cluster once its regulator exists (i.e. after the
 * DA9214 on i2c6 has probed — normal deferred probing) and onlines them.
 *
 * Sequence = vendor gemian/gemini-linux-kernel-3.18
 * arch/arm64/kernel/psci.c cpu_power_on_buck(cpu, hotplug=1):
 *   1. SPM 0x10006218 |= bit0
 *   2. dummy read 0x102224a0 (MCUCFG2: A72 iDVFS PLL CON0)
 *   3. assert TOPRGU SWSYSRST bit 11 (PWRAP_SPI_CTL_RST: PMIC-wrapper SPI
 *      controller held in reset)                       -> "pwrap-spi" reset
 *   4. enable VPROC2 (DA9214 BUCKB), 1 ms              -> "vproc" supply
 *   5. SPM 0x10006290 &= ~3 (EXT_BUCK_ISO clear)
 *   6. deassert the reset
 *   7. 240 us, A72 SRAM LDO 1.1 V via SMC 0xC20003BF, 240 us
 * then PSCI CPU_ON through the normal hotplug path (add_cpu()).
 * Doing this from userspace with the rail switched on OUTSIDE step 3 and a
 * tens-of-ms latch window (cl2-up.sh) wedged the board when boot was busy;
 * this order with a ~1.4 ms window passed settled, loaded and at-boot
 * tests (docs/cpu-dvfs.md, test module mt6797-cl2-on, 2026-10-06).
 *
 * The SPM / MCUCFG2 words are mapped without claiming the regions (they
 * belong to scpsys / mcucfg); only these two SPM bits are touched.
 *
 * Not handled (yet): re-powering the cluster after BOTH A72s were taken
 * offline (ATF may tear the cluster down on the last CPU off). Keep at
 * least one A72 online, or reboot.
 *
 * DT:
 *   cl2-power {
 *       compatible = "mediatek,mt6797-cl2-power";
 *       reg = <0 0x10006000 0 0x1000>, <0 0x10222000 0 0x1000>;
 *       reg-names = "spm", "mcucfg2";
 *       vproc-supply = <&vproc2>;
 *       resets = <&watchdog 11>;
 *       reset-names = "pwrap-spi";
 *       cpus = <&cpu8>, <&cpu9>;
 *   };
 */

#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>

#define SPM_CL2_218		0x218
#define SPM_EXT_BUCK_ISO	0x290
#define MCUCFG2_IDVFS_PLL_CON0	0x4a0
#define MTK_SIP_IDVFS_SRAMLDO_SET 0xC20003BFUL
#define CL2_SRAM_UV_X100	110000	/* 1.1 V, vendor value */
#define CL2_MAX_CPUS		2

struct cl2_power {
	struct device *dev;
	void __iomem *spm;
	void __iomem *mcucfg2;
	struct regulator *vproc;
	struct reset_control *pwrap_spi;
	int cpus[CL2_MAX_CPUS];
	int ncpus;
};

static void __iomem *cl2_map(struct platform_device *pdev, const char *name)
{
	struct resource *res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);

	if (!res)
		return IOMEM_ERR_PTR(-EINVAL);
	/* shared with scpsys / mcucfg: map, don't request */
	return devm_ioremap(&pdev->dev, res->start, resource_size(res)) ?:
	       IOMEM_ERR_PTR(-ENOMEM);
}

static int cl2_power_on(struct cl2_power *cl2)
{
	struct arm_smccc_res smc;
	unsigned long flags;
	s64 t0, t_latch, t_unlatch;
	int ret, ret2;

	t0 = ktime_to_us(ktime_get());

	local_irq_save(flags);
	writel(readl(cl2->spm + SPM_CL2_218) | BIT(0), cl2->spm + SPM_CL2_218);
	(void)readl(cl2->mcucfg2 + MCUCFG2_IDVFS_PLL_CON0);
	local_irq_restore(flags);

	ret = reset_control_assert(cl2->pwrap_spi);
	if (ret) {
		dev_err(cl2->dev, "pwrap-spi reset assert failed: %d\n", ret);
		return ret;
	}
	t_latch = ktime_to_us(ktime_get());

	ret = regulator_enable(cl2->vproc);
	udelay(1000);

	local_irq_save(flags);
	writel(readl(cl2->spm + SPM_EXT_BUCK_ISO) & ~0x3u, cl2->spm + SPM_EXT_BUCK_ISO);
	local_irq_restore(flags);

	ret2 = reset_control_deassert(cl2->pwrap_spi);
	t_unlatch = ktime_to_us(ktime_get());

	dev_info(cl2->dev, "VPROC2 %s (%d uV), latch window %lld us\n",
		 ret ? "enable FAILED" : "on", regulator_get_voltage(cl2->vproc),
		 t_unlatch - t_latch);
	if (ret)
		return ret;
	if (ret2) {
		dev_err(cl2->dev, "pwrap-spi reset deassert failed: %d\n", ret2);
		return ret2;
	}

	udelay(240);
	arm_smccc_smc(MTK_SIP_IDVFS_SRAMLDO_SET, CL2_SRAM_UV_X100, 0, 0, 0, 0, 0, 0, &smc);
	udelay(240);
	if (smc.a0) {
		dev_err(cl2->dev, "A72 SRAM LDO SMC failed: 0x%lx\n", smc.a0);
		return -EIO;
	}

	dev_info(cl2->dev, "cluster powered in %lld us (SPM218=0x%08x SPM290=0x%08x)\n",
		 ktime_to_us(ktime_get()) - t0, readl(cl2->spm + SPM_CL2_218),
		 readl(cl2->spm + SPM_EXT_BUCK_ISO));
	return 0;
}

static int cl2_power_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cl2_power *cl2;
	int i, ret;

	cl2 = devm_kzalloc(dev, sizeof(*cl2), GFP_KERNEL);
	if (!cl2)
		return -ENOMEM;
	cl2->dev = dev;

	cl2->spm = cl2_map(pdev, "spm");
	if (IS_ERR(cl2->spm))
		return dev_err_probe(dev, PTR_ERR(cl2->spm), "spm\n");
	cl2->mcucfg2 = cl2_map(pdev, "mcucfg2");
	if (IS_ERR(cl2->mcucfg2))
		return dev_err_probe(dev, PTR_ERR(cl2->mcucfg2), "mcucfg2\n");

	cl2->vproc = devm_regulator_get(dev, "vproc");
	if (IS_ERR(cl2->vproc))
		return dev_err_probe(dev, PTR_ERR(cl2->vproc), "vproc supply\n");

	cl2->pwrap_spi = devm_reset_control_get_exclusive(dev, "pwrap-spi");
	if (IS_ERR(cl2->pwrap_spi))
		return dev_err_probe(dev, PTR_ERR(cl2->pwrap_spi), "pwrap-spi reset\n");

	for (i = 0; i < CL2_MAX_CPUS; i++) {
		struct device_node *np = of_parse_phandle(dev->of_node, "cpus", i);
		int cpu;

		if (!np)
			break;
		cpu = of_cpu_node_to_id(np);
		of_node_put(np);
		if (cpu < 0)
			return dev_err_probe(dev, cpu, "cpus[%d]\n", i);
		cl2->cpus[cl2->ncpus++] = cpu;
	}
	if (!cl2->ncpus)
		return dev_err_probe(dev, -EINVAL, "no cpus\n");

	for (i = 0; i < cl2->ncpus; i++) {
		if (cpu_online(cl2->cpus[i])) {
			dev_warn(dev, "cpu%d already online - leaving the cluster alone\n",
				 cl2->cpus[i]);
			return 0;
		}
	}

	ret = cl2_power_on(cl2);
	if (ret) {
		/* keep our resources (the rail may be on): no retry, no put */
		dev_err(dev, "power-on failed: %d - cpu8/9 stay offline\n", ret);
		return 0;
	}

	for (i = 0; i < cl2->ncpus; i++) {
		s64 t = ktime_to_us(ktime_get());

		ret = add_cpu(cl2->cpus[i]);
		dev_info(dev, "cpu%d online: %d (%lld us)\n", cl2->cpus[i], ret,
			 ktime_to_us(ktime_get()) - t);
		if (ret)
			break;
	}
	/* keep the rail on whatever happened: the cluster is powered now */
	platform_set_drvdata(pdev, cl2);
	return 0;
}

static const struct of_device_id cl2_power_of_match[] = {
	{ .compatible = "mediatek,mt6797-cl2-power" },
	{ }
};
MODULE_DEVICE_TABLE(of, cl2_power_of_match);

static struct platform_driver cl2_power_driver = {
	.probe = cl2_power_probe,
	.driver = {
		.name = "mt6797-cl2-power",
		.of_match_table = cl2_power_of_match,
		.suppress_bind_attrs = true,	/* never unbind: the rail must stay on */
	},
};
module_platform_driver(cl2_power_driver);

MODULE_DESCRIPTION("MT6797 A72 cluster power-on (Gemini PDA)");
MODULE_LICENSE("GPL");
