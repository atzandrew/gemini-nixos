// SPDX-License-Identifier: GPL-2.0
/*
 * da9214-test — awake power test for VPROC2 (DA9214 BUCKB, the A72 rail)
 * while the A72 cluster is torn down (2026-10-17, notes/vproc-research.md).
 *
 * Question: is the ~43 mA that VPROC2 costs leakage of the off cluster at
 * 1.0 V (falls steeply with voltage) or the buck's own quiescent current
 * (datasheet: 17 mA per phase in PWM, BUCKB runs 2 phases)?
 *
 * On load: applies the given BUCKB settings; on unload: restores them.
 * Only BUCKB registers are touched (0xD9 VBUCKB_A, 0xD3 PHASE_SEL_B,
 * 0xD2 BUCKB_MODE), through the da9211 driver's regmap (page register and
 * cache stay right). Refuses unless cpu8/9 are offline and ATF has torn
 * the cluster down (EXT_BUCK_ISO 0x10006290 bit1 set, 0x10006218 bit0
 * clear), i.e. nothing runs from the rail.
 *
 *   insmod da9214-test.ko vproc2_mv=600     # 0.30..1.00 V, 10 mV steps
 *   insmod da9214-test.ko phases=1          # BUCKB 1 phase instead of 2
 *   insmod da9214-test.ko mode=1            # BUCKB_MODE: 1 auto 1-phase, 3 auto
 *   insmod da9214-test.ko noi2c=1           # load/unload only, no I2C at all
 *   insmod da9214-test.ko nospm=1           # empty: no SPM read, no I2C (implies noi2c)
 *   rmmod da9214-test                       # restore
 */
#include <linux/module.h>
#include <linux/cpumask.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/regmap.h>

#define TAG "da9214-test: "

#define DA9214_BUCKB_CONF	0xd2	/* [1:0] BUCKB_MODE */
#define DA9214_BUCK_CONF	0xd3	/* [2] PHASE_SEL_B: 0 = 1 phase, 1 = 2 */
#define DA9214_VBUCKB_A		0xd9	/* [7] SL_A, [6:0] 300 mV + 10 mV * n */

#define SPM_BASE		0x10006000
#define SPM_CL2_218		0x218
#define SPM_EXT_BUCK_ISO	0x290

static uint vproc2_mv;
module_param(vproc2_mv, uint, 0444);
MODULE_PARM_DESC(vproc2_mv, "VBUCKB_A in mV (300..1000, 0 = leave)");
static uint phases;
module_param(phases, uint, 0444);
MODULE_PARM_DESC(phases, "BUCKB phases (1 or 2, 0 = leave)");
static uint mode;
module_param(mode, uint, 0444);
MODULE_PARM_DESC(mode, "BUCKB_MODE (1 = auto 1-phase, 2 = PWM, 3 = auto, 0 = leave)");
/* Hang hunt control: same load/unload and SPM check, but no regmap access. */
static bool noi2c;
module_param(noi2c, bool, 0444);
MODULE_PARM_DESC(noi2c, "load/unload only, never touch the DA9214 (hang hunt)");
static bool nospm;
module_param(nospm, bool, 0444);
MODULE_PARM_DESC(nospm, "empty load: no SPM ioremap/read and no I2C (hang hunt)");

static struct regmap *map;
static unsigned int saved_d2, saved_d3, saved_d9;

static int __init da9214_test_init(void)
{
	struct device *dev;
	void __iomem *spm;
	u32 iso, con;
	int ret;

	if ((vproc2_mv && (vproc2_mv < 300 || vproc2_mv > 1000)) || phases > 2 || mode > 3 || mode == 2)
		return -EINVAL;	/* mode 2 (forced PWM) is never what we want */
	if (nospm) {
		noi2c = true;
		pr_info(TAG "nospm: loaded, nothing touched\n");
		return 0;
	}

	spm = ioremap(SPM_BASE, 0x1000);
	if (!spm)
		return -ENOMEM;
	iso = readl(spm + SPM_EXT_BUCK_ISO);
	con = readl(spm + SPM_CL2_218);
	iounmap(spm);
	if (cpu_online(8) || cpu_online(9) || !(iso & BIT(1)) || (con & BIT(0))) {
		pr_err(TAG "A72 cluster not torn down (cpu8/9 %d%d, ISO %08x, 218 %08x)\n",
		       cpu_online(8), cpu_online(9), iso, con);
		return -EBUSY;
	}

	if (noi2c) {
		pr_info(TAG "noi2c: loaded, DA9214 not touched\n");
		return 0;
	}

	dev = bus_find_device_by_name(&i2c_bus_type, NULL, "2-0068");
	if (!dev)
		return -ENODEV;
	map = dev_get_regmap(dev, NULL);
	put_device(dev);
	if (!map)
		return -ENODEV;

	ret = regmap_read(map, DA9214_BUCKB_CONF, &saved_d2) ?:
	      regmap_read(map, DA9214_BUCK_CONF, &saved_d3) ?:
	      regmap_read(map, DA9214_VBUCKB_A, &saved_d9);
	if (ret)
		return ret;
	pr_info(TAG "saved D2 %02x D3 %02x D9 %02x\n", saved_d2, saved_d3, saved_d9);

	if (vproc2_mv)
		ret = regmap_update_bits(map, DA9214_VBUCKB_A, 0x7f, (vproc2_mv - 300) / 10);
	if (!ret && phases)
		ret = regmap_update_bits(map, DA9214_BUCK_CONF, BIT(2), phases == 2 ? BIT(2) : 0);
	if (!ret && mode)
		ret = regmap_update_bits(map, DA9214_BUCKB_CONF, 0x3, mode);
	if (ret) {
		pr_err(TAG "write failed %d, restoring\n", ret);
		regmap_write(map, DA9214_BUCKB_CONF, saved_d2);
		regmap_write(map, DA9214_BUCK_CONF, saved_d3);
		regmap_write(map, DA9214_VBUCKB_A, saved_d9);
		return ret;
	}
	pr_info(TAG "applied vproc2_mv=%u phases=%u mode=%u\n", vproc2_mv, phases, mode);
	return 0;
}

static void __exit da9214_test_exit(void)
{
	int ret;

	if (noi2c) {
		pr_info(TAG "noi2c: unloaded, DA9214 not touched\n");
		return;
	}
	ret = regmap_write(map, DA9214_BUCKB_CONF, saved_d2) |
		  regmap_write(map, DA9214_BUCK_CONF, saved_d3) |
		  regmap_write(map, DA9214_VBUCKB_A, saved_d9);

	pr_info(TAG "restored D2 %02x D3 %02x D9 %02x (%d)\n", saved_d2, saved_d3, saved_d9, ret);
}

module_init(da9214_test_init);
module_exit(da9214_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 Gemini: VPROC2 (DA9214 BUCKB) awake power test");
