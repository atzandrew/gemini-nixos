// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-cpuldo-regulator.c — MT6797 VSRAM_L: the eight CPU SRAM LDOs of the
 * A53 clusters (LL, L) and the CCI, controlled from INFRACFG_AO.
 * Gemini PDA, 2026-10-06. docs/cpu-dvfs.md.
 *
 * Registers (vendor gemian/gemini-linux-kernel-3.18
 * drivers/misc/mediatek/base/power/mt6797/mt_cpufreq.c,
 * set_cur_volt_sram_l / get_cur_volt_sram_l):
 *   CPULDO_CTRL_0 0xF98  [7:0]  enable, one bit per LDO; must be 0xff
 *   CPULDO_CTRL_1 0xF9C  vosel of LDO0..3 in [3:0], [11:8], [19:16], [27:24]
 *   CPULDO_CTRL_2 0xFA0  vosel of LDO4..7, same layout
 * vosel: 0 = 1.05 V, 1 = 0.60 V, 2 = 0.70 V, n >= 3 = 0.90 V + (n - 3) * 25 mV.
 * The vendor always writes the same vosel to all eight LDOs as a whole word
 * (vosel replicated into every byte) to both CTRL_1 and CTRL_2; LK leaves
 * 0x0b0b0b0b / 0x0b0b0b0b (1.10 V), so that is also what we write.
 *
 * Only selectors 3..15 (0.90..1.20 V) are offered: 0..2 are the odd low /
 * legacy codes the vendor never uses for DVFS (its range is 1.00..1.20 V,
 * MIN/MAX_VSRAM_VOLT); DT constraints narrow it further.
 *
 * Settle (vendor PMIC_VOLT_UP/DOWN_SETTLE_TIME + PMIC_CMD_DELAY_TIME,
 * MIN_PMIC_SETTLE_TIME): up 12.5 mV/us, down 3.125 mV/us, plus 5 us,
 * at least 25 us.
 *
 * Tracking against VPROC1 (VSRAM >= VPROC, VSRAM - VPROC <= 300 mV) is NOT
 * done here: in mainline that is the consumer's job (mediatek-cpufreq's
 * "sram-supply" voltage tracking, min/max_volt_shift).
 *
 * INFRACFG_AO is a syscon (clk-mt6797 owns it); access goes through its
 * regmap: mediatek,infracfg = <&infrasys>.
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define CPULDO_CTRL_0		0xf98
#define CPULDO_CTRL_1		0xf9c
#define CPULDO_CTRL_2		0xfa0
#define CPULDO_EN_ALL		0xff
#define CPULDO_VOSEL_MASK	0xf
#define CPULDO_SEL_MIN		3
#define CPULDO_SEL_MAX		15

static int cpuldo_is_enabled(struct regulator_dev *rdev)
{
	unsigned int v;
	int ret = regmap_read(rdev->regmap, CPULDO_CTRL_0, &v);

	if (ret)
		return ret;
	return (v & CPULDO_EN_ALL) == CPULDO_EN_ALL;
}

static int cpuldo_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int c1, c2;
	int ret;

	ret = regmap_read(rdev->regmap, CPULDO_CTRL_1, &c1);
	if (!ret)
		ret = regmap_read(rdev->regmap, CPULDO_CTRL_2, &c2);
	if (ret)
		return ret;
	if (c1 != c2 || c1 != (c1 & CPULDO_VOSEL_MASK) * 0x01010101u) {
		/* the vendor BUG()s here; we just refuse to guess */
		dev_err(&rdev->dev, "LDOs disagree: CTRL_1 0x%08x CTRL_2 0x%08x\n", c1, c2);
		return -EINVAL;
	}
	c1 &= CPULDO_VOSEL_MASK;
	if (c1 < CPULDO_SEL_MIN) {
		dev_err(&rdev->dev, "unsupported vosel %u\n", c1);
		return -EINVAL;
	}
	return c1;
}

static int cpuldo_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	u32 word = (sel & CPULDO_VOSEL_MASK) * 0x01010101u;
	int ret;

	if (sel < CPULDO_SEL_MIN || sel > CPULDO_SEL_MAX)
		return -EINVAL;
	/* vendor: "Make sure 8 LDO is enable" */
	ret = regmap_update_bits(rdev->regmap, CPULDO_CTRL_0, CPULDO_EN_ALL, CPULDO_EN_ALL);
	if (!ret)
		ret = regmap_write(rdev->regmap, CPULDO_CTRL_1, word);
	if (!ret)
		ret = regmap_write(rdev->regmap, CPULDO_CTRL_2, word);
	return ret;
}

static int cpuldo_set_voltage_time_sel(struct regulator_dev *rdev,
				       unsigned int old_sel, unsigned int new_sel)
{
	int old_uv = regulator_list_voltage_linear(rdev, old_sel);
	int new_uv = regulator_list_voltage_linear(rdev, new_sel);
	int t;

	if (old_uv < 0 || new_uv < 0)
		return 25;
	if (new_uv >= old_uv)
		t = DIV_ROUND_UP(new_uv - old_uv, 12500) + 5;
	else
		t = (old_uv - new_uv) / 3125 + 5;
	return max(t, 25);
}

static const struct regulator_ops cpuldo_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = cpuldo_get_voltage_sel,
	.set_voltage_sel = cpuldo_set_voltage_sel,
	.set_voltage_time_sel = cpuldo_set_voltage_time_sel,
	.is_enabled = cpuldo_is_enabled,
};

static const struct regulator_desc cpuldo_desc = {
	.name = "vsram_l",
	.of_match = NULL,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &cpuldo_ops,
	.n_voltages = CPULDO_SEL_MAX + 1,
	.linear_min_sel = CPULDO_SEL_MIN,
	.min_uV = 900000,
	.uV_step = 25000,
};

static int mt6797_cpuldo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config cfg = { .dev = dev, .of_node = dev->of_node };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int c0, c1, c2;

	regmap = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,infracfg");
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap), "mediatek,infracfg\n");

	regmap_read(regmap, CPULDO_CTRL_0, &c0);
	regmap_read(regmap, CPULDO_CTRL_1, &c1);
	regmap_read(regmap, CPULDO_CTRL_2, &c2);
	dev_info(dev, "CPULDO_CTRL_0..2 = 0x%08x 0x%08x 0x%08x\n", c0, c1, c2);

	cfg.regmap = regmap;
	cfg.init_data = of_get_regulator_init_data(dev, dev->of_node, &cpuldo_desc);
	if (!cfg.init_data)
		return -ENOMEM;

	rdev = devm_regulator_register(dev, &cpuldo_desc, &cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "register\n");
	return 0;
}

static const struct of_device_id mt6797_cpuldo_of_match[] = {
	{ .compatible = "mediatek,mt6797-cpuldo" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6797_cpuldo_of_match);

static struct platform_driver mt6797_cpuldo_driver = {
	.probe = mt6797_cpuldo_probe,
	.driver = {
		.name = "mt6797-cpuldo",
		.of_match_table = mt6797_cpuldo_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6797_cpuldo_driver);

MODULE_DESCRIPTION("MT6797 CPU SRAM LDO (VSRAM_L) regulator");
MODULE_LICENSE("GPL");
