// SPDX-License-Identifier: GPL-2.0
/*
 * MT6351 PMIC real-time clock for the Gemini PDA.
 *
 * The MT6351 RTC uses the same register layout as the MT6323/MT6397
 * RTCs (vendor include/mt-plat/mt6797/include/mach/mt_rtc_hw.h, RTC_BASE
 * 0x4000: TC_SEC..TC_YEA at +0x0a..+0x16, WRTGR at +0x3c, BBPU.CBUSY
 * bit 6). Mainline rtc-mt6397 cannot be used here: it needs the
 * mt6397 MFD core as its parent, and this platform has no MT6351 MFD
 * (each MT6351 function is a child of the pwrap node that takes the
 * pwrap regmap directly, like mt6351-keys / mt6351-regulator).
 *
 * Scope: read and set the time only. No alarm and no IRQ (the PMIC
 * interrupt path is not wired on MT6797), and nothing outside the TC_*
 * time registers is ever written: RTC_BBPU, PDN1/PDN2 and the SPAR
 * registers carry LK's boot flags (factory reset, KPOC, power-on
 * alarm), and RTC_AL_* hold vendor bits too.
 *
 * Year encoding as the vendor driver (RTC_MIN_YEAR 1968): register =
 * year - 1968; month register is 1-based.
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/rtc.h>

#define MT6351_RTC_BBPU		0x4000
#define MT6351_RTC_BBPU_RELOAD	BIT(5)
#define MT6351_RTC_BBPU_CBUSY	BIT(6)
#define MT6351_RTC_BBPU_KEY	(0x43 << 8)
#define MT6351_RTC_TC_SEC	0x400a	/* SEC MIN HOU DOM DOW MTH YEA */
#define MT6351_RTC_WRTGR	0x403c

#define MT6351_RTC_MIN_YEAR	1968
#define MT6351_RTC_YEAR_OFFSET	(MT6351_RTC_MIN_YEAR - 1900)

enum { TC_SEC, TC_MIN, TC_HOU, TC_DOM, TC_DOW, TC_MTH, TC_YEA, TC_NUM };

static const u16 mt6351_tc_mask[TC_NUM] = {
	0x3f, 0x3f, 0x1f, 0x1f, 0x07, 0x0f, 0x7f,
};

struct mt6351_rtc {
	struct device *dev;
	struct regmap *regmap;
	struct rtc_device *rtc;
};

static int mt6351_rtc_wait_idle(struct mt6351_rtc *r)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < 100; i++) {
		ret = regmap_read(r->regmap, MT6351_RTC_BBPU, &val);
		if (ret)
			return ret;
		if (!(val & MT6351_RTC_BBPU_CBUSY))
			return 0;
		usleep_range(50, 100);
	}
	return -ETIMEDOUT;
}

static int mt6351_rtc_read_tc(struct mt6351_rtc *r, u16 *tc)
{
	unsigned int val;
	int i, ret;

	for (i = 0; i < TC_NUM; i++) {
		ret = regmap_read(r->regmap, MT6351_RTC_TC_SEC + 2 * i, &val);
		if (ret)
			return ret;
		tc[i] = val & mt6351_tc_mask[i];
	}
	return 0;
}

/* vendor hal_rtc_get_tick_time(): BBPU |= KEY | RELOAD, trigger. */
static int mt6351_rtc_reload(struct mt6351_rtc *r)
{
	unsigned int bbpu;
	int ret;

	ret = regmap_read(r->regmap, MT6351_RTC_BBPU, &bbpu);
	if (ret)
		return ret;
	bbpu = (bbpu & 0xff) & ~MT6351_RTC_BBPU_CBUSY;
	ret = regmap_write(r->regmap, MT6351_RTC_BBPU,
			   bbpu | MT6351_RTC_BBPU_KEY | MT6351_RTC_BBPU_RELOAD);
	if (ret)
		return ret;
	ret = regmap_write(r->regmap, MT6351_RTC_WRTGR, 1);
	if (ret)
		return ret;
	return mt6351_rtc_wait_idle(r);
}

static int mt6351_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6351_rtc *r = dev_get_drvdata(dev);
	u16 tc[TC_NUM], sec;
	unsigned int val;
	int ret;

	ret = mt6351_rtc_reload(r);
	if (ret)
		return ret;

	/* Re-read if the seconds rolled over while reading the rest. */
	do {
		ret = mt6351_rtc_read_tc(r, tc);
		if (ret)
			return ret;
		ret = regmap_read(r->regmap, MT6351_RTC_TC_SEC, &val);
		if (ret)
			return ret;
		sec = val & mt6351_tc_mask[TC_SEC];
	} while (sec < tc[TC_SEC]);

	tm->tm_sec = tc[TC_SEC];
	tm->tm_min = tc[TC_MIN];
	tm->tm_hour = tc[TC_HOU];
	tm->tm_mday = tc[TC_DOM];
	tm->tm_mon = tc[TC_MTH] - 1;
	tm->tm_year = tc[TC_YEA] + MT6351_RTC_YEAR_OFFSET;
	/* weekday/yday derived, not read (DOW is never written either) */
	rtc_time64_to_tm(rtc_tm_to_time64(tm), tm);

	return rtc_valid_tm(tm);
}

static int mt6351_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6351_rtc *r = dev_get_drvdata(dev);
	u16 tc[TC_NUM];
	int i, ret;

	if (tm->tm_year < MT6351_RTC_YEAR_OFFSET ||
	    tm->tm_year > MT6351_RTC_YEAR_OFFSET + 127)
		return -EINVAL;

	/* vendor hal_rtc_set_tick_time(): YEA MTH DOM HOU MIN SEC, trigger */
	tc[TC_YEA] = tm->tm_year - MT6351_RTC_YEAR_OFFSET;
	tc[TC_MTH] = tm->tm_mon + 1;
	tc[TC_DOM] = tm->tm_mday;
	tc[TC_HOU] = tm->tm_hour;
	tc[TC_MIN] = tm->tm_min;
	tc[TC_SEC] = tm->tm_sec;
	for (i = TC_YEA; i >= TC_SEC; i--) {
		if (i == TC_DOW)
			continue;
		ret = regmap_write(r->regmap, MT6351_RTC_TC_SEC + 2 * i, tc[i]);
		if (ret)
			return ret;
	}

	ret = regmap_write(r->regmap, MT6351_RTC_WRTGR, 1);
	if (ret)
		return ret;
	ret = mt6351_rtc_wait_idle(r);
	if (ret)
		dev_warn(dev, "write trigger did not complete: %d\n", ret);
	return ret;
}

static const struct rtc_class_ops mt6351_rtc_ops = {
	.read_time = mt6351_rtc_read_time,
	.set_time = mt6351_rtc_set_time,
};

static int mt6351_rtc_probe(struct platform_device *pdev)
{
	struct mt6351_rtc *r;
	struct rtc_time tm;
	int ret;

	r = devm_kzalloc(&pdev->dev, sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	r->dev = &pdev->dev;
	r->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!r->regmap) {
		dev_err(&pdev->dev, "no pwrap regmap on parent\n");
		return -ENODEV;
	}
	platform_set_drvdata(pdev, r);

	r->rtc = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(r->rtc))
		return PTR_ERR(r->rtc);
	r->rtc->ops = &mt6351_rtc_ops;
	r->rtc->range_min = mktime64(MT6351_RTC_MIN_YEAR, 1, 1, 0, 0, 0);
	r->rtc->range_max = mktime64(MT6351_RTC_MIN_YEAR + 127, 12, 31,
				     23, 59, 59);

	ret = mt6351_rtc_read_time(&pdev->dev, &tm);
	if (ret)
		dev_warn(&pdev->dev, "RTC time invalid (%d); set it with hwclock -w or wait for NTP\n",
			 ret);
	else
		dev_info(&pdev->dev, "MT6351 RTC: %ptRd %ptRt UTC\n", &tm, &tm);

	return devm_rtc_register_device(r->rtc);
}

static const struct of_device_id mt6351_rtc_of_match[] = {
	{ .compatible = "mediatek,mt6351-rtc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6351_rtc_of_match);

static struct platform_driver mt6351_rtc_driver = {
	.probe	= mt6351_rtc_probe,
	.driver	= {
		.name		= "mt6351-rtc",
		.of_match_table	= mt6351_rtc_of_match,
	},
};
module_platform_driver(mt6351_rtc_driver);

MODULE_AUTHOR("Gemini PDA Linux port");
MODULE_DESCRIPTION("MT6351 PMIC RTC (time only) for the Gemini PDA");
MODULE_LICENSE("GPL");
