// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6797 / MT6351 power-reset driver for the Gemini PDA.
 *
 * This registers two system power transitions that mainline Linux lacks
 * on this SoC:
 *
 *   - restart:  the TOPRGU watchdog external reset, exactly the
 *               sequence LK uses for every one of its reboots
 *               (gemini-lk lk/platform/mt6797/mtk_wdt.c:mtk_wdt_reset()).
 *               The mainline mtk_wdt restart handler only pokes
 *               WDT_SWRST without first restoring WDT_MODE; on this
 *               unit that leaves the SoC in a dead "limbo" (PMIC on,
 *               CPUs stopped, LK never re-runs; observed on glass
 *               2026-09-10). Replicating LK's full sequence fixes it.
 *
 *   - poweroff: the MT6351 RTC BBPU "pull PWRBB low" write, exactly
 *               the sequence LK (mt_rtc.c:rtc_bbpu_power_down()) and
 *               the vendor Android kernel (mt_power_off()) use. If USB
 *               power is attached the PMIC cannot drop the rails, so
 *               fall back to LK's off-mode-charging path (WDT reset
 *               without the "bypass power key" bit).
 *
 * 2026-10-05 (G1): the RTC writes moved to a POWER_OFF_PREPARE sys-off
 * handler (interrupts still on; the pwrap regmap may sleep while polling
 * its FSM, which the final IRQs-off handler cannot safely do), plus the
 * vendor's 32k-export stop, runtime knobs (module params f32k_off,
 * clear_alarm, fallback_ms) and diagnostics in sysfs: rtc_regs (dump)
 * and bbpu_test (run the power-down write on a live system and log the
 * RTC_BBPU readback). See docs/power-states.md.
 *
 * Both paths are bring-up knowledge from the legacy GeminiPDA project;
 * see docs/power-states.md in the gemini-nixos repo for the full
 * source-traced receipts and the on-glass test plan.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

/* ---- TOPRGU / watchdog registers (0x10007000) ---- */
#define MTK_WDT_MODE			0x00
#define MTK_WDT_RESTART			0x08
#define MTK_WDT_SWRST			0x14

#define MTK_WDT_MODE_ENABLE		BIT(0)
#define MTK_WDT_MODE_EXTEN		BIT(2)
#define MTK_WDT_MODE_IRQ		BIT(3)
#define MTK_WDT_MODE_AUTO_RESTART	BIT(4)
#define MTK_WDT_MODE_DUAL_MODE		BIT(6)
#define MTK_WDT_MODE_KEY		0x22000000U
#define MTK_WDT_RESTART_KEY		0x1971
#define MTK_WDT_SWRST_KEY		0x1209

/* ---- MT6351 RTC space, reached over the pwrap 16-bit regmap ---- */
#define MT6351_RTC_BBPU			0x4000
#define MT6351_RTC_IRQ_EN		0x4004
#define MT6351_RTC_PDN1			0x402c
#define MT6351_RTC_CON			0x403e
#define MT6351_CHR_CON0			0x0f78
#define MT6351_RTC_AL_SEC		0x4018
#define MT6351_RTC_PROT			0x4036
#define MT6351_RTC_WRTGR		0x403c

#define MT6351_RTC_BBPU_PWREN		BIT(0)
#define MT6351_RTC_BBPU_BBPU		BIT(2)	/* 1: power on, 0: power down */
#define MT6351_RTC_BBPU_AUTO		BIT(3)
#define MT6351_RTC_BBPU_CBUSY		BIT(6)
#define MT6351_RTC_BBPU_AUTO_PDN_SEL	BIT(6)
#define MT6351_RTC_BBPU_2SEC_EN		BIT(8)
#define MT6351_RTC_BBPU_KEY		(0x43 << 8)

#define MT6351_RTC_IRQ_EN_AL		BIT(0)
#define MT6351_RTC_CON_F32KOB		BIT(5)	/* 1: RTC_GPIO 32k export off */
#define MT6351_RTC_GPIO_USER_MASK	(0x1f << 8)	/* RTC_PDN1 bits 8-12 */
#define MT6351_CHR_CON0_CHRDET		BIT(5)

#define MT6351_RTC_PROT_UNLOCK1		0x586a
#define MT6351_RTC_PROT_UNLOCK2		0x9136

struct mt6797_power {
	struct device *dev;
	void __iomem *wdt_base;
	struct regmap *pmic;
	struct notifier_block restart_nb;
};

/* register_platform_power_off() takes a bare callback: singleton. */
static struct mt6797_power *mt6797_power_priv;

/*
 * LK's mtk_wdt_reset(): reload first, then set the hardware-reboot mode
 * (with the key) and finally trigger the external reset. bypass=true is
 * LK's mode 1 ("bypass power key"): the PMIC power-cycles the SoC and it
 * self-boots. bypass=false is mode 0, used for the off-mode-charging
 * fallback (LK re-runs, waits on the power key).
 */
static void mt6797_wdt_reset(void __iomem *base, bool bypass)
{
	u32 mode;

	writel(MTK_WDT_RESTART_KEY, base + MTK_WDT_RESTART);

	mode = readl(base + MTK_WDT_MODE);
	mode &= ~(MTK_WDT_MODE_AUTO_RESTART | MTK_WDT_MODE_IRQ |
		  MTK_WDT_MODE_ENABLE | MTK_WDT_MODE_DUAL_MODE);
	mode |= MTK_WDT_MODE_KEY | MTK_WDT_MODE_EXTEN;
	if (bypass)
		mode |= MTK_WDT_MODE_AUTO_RESTART;
	writel(mode, base + MTK_WDT_MODE);

	udelay(100);
	writel(MTK_WDT_SWRST_KEY, base + MTK_WDT_SWRST);
}

static int mt6797_restart(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	struct mt6797_power *p = container_of(nb, struct mt6797_power,
					      restart_nb);

	mt6797_wdt_reset(p->wdt_base, true);

	/* The reset should have fired; keep it alive if the SoC is slow. */
	while (1) {
		writel(MTK_WDT_SWRST_KEY, p->wdt_base + MTK_WDT_SWRST);
		mdelay(5);
	}

	return NOTIFY_DONE;
}

/* LK's rtc_write_trigger(): start the write and wait for !CBUSY. */
static void mt6351_rtc_trigger(struct regmap *pmic)
{
	unsigned int val, i;

	regmap_write(pmic, MT6351_RTC_WRTGR, 1);
	for (i = 0; i < 100; i++) {
		if (regmap_read(pmic, MT6351_RTC_BBPU, &val))
			break;
		if (!(val & MT6351_RTC_BBPU_CBUSY))
			break;
		udelay(10);
	}
}

/*
 * Poweroff tuning, writable at runtime under
 * /sys/module/mt6797_power/parameters/ (G1 investigation, 2026-10-05).
 *
 *   f32k_off     vendor hal_rtc_bbpu_pwdn(): stop the RTC_GPIO 32 kHz
 *                export (RTC_CON.F32KOB) before pulling PWRBB low, when
 *                no RTC_GPIO user bit is set in RTC_PDN1.
 *   clear_alarm  disable the RTC alarm interrupt before the BBPU write.
 *                BBPU is written with PWREN ("BBPU=1 when alarm occurs"),
 *                so a pending/enabled alarm can power the PMIC straight
 *                back on.
 *   fallback_ms  if still running this long after the BBPU write (USB
 *                power holds the rails up), do LK's mode-0 WDT reset into
 *                off-mode charging. 0 = never: spin instead (diagnostic:
 *                separates "BBPU did not cut" from LK's own behaviour).
 */
static bool f32k_off = true;
module_param(f32k_off, bool, 0644);
MODULE_PARM_DESC(f32k_off, "stop the RTC 32k export before power-down (vendor)");

static bool clear_alarm;
module_param(clear_alarm, bool, 0644);
MODULE_PARM_DESC(clear_alarm, "disable the RTC alarm IRQ before power-down");

static unsigned int fallback_ms = 1000;
module_param(fallback_ms, uint, 0644);
MODULE_PARM_DESC(fallback_ms, "ms to wait before the off-mode-charging WDT reset (0 = spin)");

/* set once the BBPU write has been done from the PREPARE stage */
static bool mt6797_bbpu_done;

/*
 * The vendor/LK power-down sequence (hal_rtc_bbpu_pwdn + rtc_bbpu_pwrdown).
 * Returns the RTC_BBPU readback after the write. Uses the pwrap regmap,
 * whose transactions may sleep (readx_poll_timeout): call it only from
 * process context with interrupts enabled.
 */
static unsigned int mt6351_bbpu_power_down(struct mt6797_power *p,
					   unsigned int *con_before)
{
	unsigned int val = 0, pdn1 = 0;

	/* rtc_disable_2sec_reboot(): clear the 2-second reboot latch. */
	if (!regmap_read(p->pmic, MT6351_RTC_AL_SEC, &val)) {
		val &= ~(MT6351_RTC_BBPU_2SEC_EN |
			 MT6351_RTC_BBPU_AUTO_PDN_SEL);
		regmap_write(p->pmic, MT6351_RTC_AL_SEC, val);
		mt6351_rtc_trigger(p->pmic);
	}

	if (con_before)
		regmap_read(p->pmic, MT6351_RTC_CON, con_before);

	if (f32k_off && !regmap_read(p->pmic, MT6351_RTC_PDN1, &pdn1) &&
	    !(pdn1 & MT6351_RTC_GPIO_USER_MASK) &&
	    !regmap_read(p->pmic, MT6351_RTC_CON, &val)) {
		regmap_write(p->pmic, MT6351_RTC_CON,
			     val | MT6351_RTC_CON_F32KOB);
		mt6351_rtc_trigger(p->pmic);
	}

	if (clear_alarm && !regmap_read(p->pmic, MT6351_RTC_IRQ_EN, &val)) {
		regmap_write(p->pmic, MT6351_RTC_IRQ_EN,
			     val & ~MT6351_RTC_IRQ_EN_AL);
		mt6351_rtc_trigger(p->pmic);
	}

	/* Unlock the RTC write interface. */
	regmap_write(p->pmic, MT6351_RTC_PROT, MT6351_RTC_PROT_UNLOCK1);
	mt6351_rtc_trigger(p->pmic);
	regmap_write(p->pmic, MT6351_RTC_PROT, MT6351_RTC_PROT_UNLOCK2);
	mt6351_rtc_trigger(p->pmic);

	/* Pull PWRBB low: KEY | AUTO | PWREN = 0x4309 (BBPU bit 2 = 0). */
	regmap_write(p->pmic, MT6351_RTC_BBPU,
		     MT6351_RTC_BBPU_KEY | MT6351_RTC_BBPU_AUTO |
		     MT6351_RTC_BBPU_PWREN);
	mt6351_rtc_trigger(p->pmic);

	val = 0xffff;
	regmap_read(p->pmic, MT6351_RTC_BBPU, &val);
	return val;
}

/*
 * SYS_OFF_MODE_POWER_OFF_PREPARE: runs from kernel_power_off() after
 * device shutdown but BEFORE machine_power_off() disables interrupts and
 * stops the other CPUs. The pwrap regmap can sleep while polling its
 * FSM, so this is the only safe place for the RTC writes. (Until
 * 2026-10-05 they ran in the final, IRQs-off handler.)
 */
static int mt6797_power_off_prepare(struct sys_off_data *data)
{
	struct mt6797_power *p = data->cb_data;
	unsigned int bbpu;

	pr_emerg("mt6797-power: power-down: BBPU write (f32k_off=%d clear_alarm=%d fallback_ms=%u)\n",
		 f32k_off, clear_alarm, fallback_ms);
	bbpu = mt6351_bbpu_power_down(p, NULL);
	mt6797_bbpu_done = true;
	pr_emerg("mt6797-power: RTC_BBPU after write 0x%04x (bit2 %s)\n",
		 bbpu, (bbpu & MT6351_RTC_BBPU_BBPU) ? "STILL SET" : "clear");
	return NOTIFY_DONE;
}

static void mt6797_power_off(void)
{
	struct mt6797_power *p = mt6797_power_priv;

	if (WARN_ON(!p))
		return;

	/* Only if PREPARE did not run (it always should on poweroff). */
	if (!mt6797_bbpu_done)
		mt6351_bbpu_power_down(p, NULL);

	/*
	 * If USB power holds the rails up we are still executing; hand over
	 * to LK's off-mode charging (WDT reset without bypassing the power
	 * key). On battery the BBPU write should already have cut the AP.
	 */
	if (fallback_ms) {
		mdelay(fallback_ms);
		pr_emerg("mt6797-power: still alive %u ms after BBPU, WDT mode-0 reset\n",
			 fallback_ms);
		mt6797_wdt_reset(p->wdt_base, false);
		while (1) {
			writel(MTK_WDT_SWRST_KEY,
			       p->wdt_base + MTK_WDT_SWRST);
			mdelay(5);
		}
	}

	pr_emerg("mt6797-power: fallback disabled, spinning\n");
	while (1)
		cpu_relax();
}

/* ---- diagnostics: /sys/devices/platform/.../power@10007000/ ---- */

static const struct {
	const char *name;
	unsigned int reg;
} mt6351_rtc_dump_regs[] = {
	{ "BBPU", 0x4000 }, { "IRQ_STA", 0x4002 }, { "IRQ_EN", 0x4004 },
	{ "CII_EN", 0x4006 }, { "AL_MASK", 0x4008 },
	{ "TC_SEC", 0x400a }, { "TC_MIN", 0x400c }, { "TC_HOU", 0x400e },
	{ "TC_DOM", 0x4010 }, { "TC_DOW", 0x4012 }, { "TC_MTH", 0x4014 },
	{ "TC_YEA", 0x4016 },
	{ "AL_SEC", 0x4018 }, { "AL_MIN", 0x401a }, { "AL_HOU", 0x401c },
	{ "AL_DOM", 0x401e }, { "AL_DOW", 0x4020 }, { "AL_MTH", 0x4022 },
	{ "AL_YEA", 0x4024 }, { "OSC32CON", 0x4026 },
	{ "POWERKEY1", 0x4028 }, { "POWERKEY2", 0x402a },
	{ "PDN1", 0x402c }, { "PDN2", 0x402e }, { "SPAR0", 0x4030 },
	{ "SPAR1", 0x4032 }, { "PROT", 0x4036 }, { "DIFF", 0x4038 },
	{ "CALI", 0x403a }, { "CON", 0x403e },
	{ "CHR_CON0", 0x0f78 }, { "TOPSTATUS", 0x0220 },
};

static ssize_t rtc_regs_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct mt6797_power *p = dev_get_drvdata(dev);
	unsigned int i, val;
	ssize_t n = 0;

	for (i = 0; i < ARRAY_SIZE(mt6351_rtc_dump_regs); i++) {
		if (regmap_read(p->pmic, mt6351_rtc_dump_regs[i].reg, &val))
			n += sysfs_emit_at(buf, n, "%-9s 0x%04x  read error\n",
					   mt6351_rtc_dump_regs[i].name,
					   mt6351_rtc_dump_regs[i].reg);
		else
			n += sysfs_emit_at(buf, n, "%-9s 0x%04x  0x%04x\n",
					   mt6351_rtc_dump_regs[i].name,
					   mt6351_rtc_dump_regs[i].reg, val);
	}
	return n;
}
static DEVICE_ATTR_ADMIN_RO(rtc_regs);

/*
 * Write "1": do the real power-down register sequence on the RUNNING
 * system and log what happens. If the rails really drop, the unit just
 * switches off (sync first!). If it is still alive after 2 s, BBPU is
 * restored to power-on (KEY|AUTO|BBPU|PWREN, LK's rtc_bbpu_power_on())
 * and RTC_CON put back, and the readbacks are in dmesg.
 */
static ssize_t bbpu_test_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mt6797_power *p = dev_get_drvdata(dev);
	unsigned int bbpu, prev, con_before = 0, chr = 0, i;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	regmap_read(p->pmic, MT6351_CHR_CON0, &chr);
	dev_emerg(dev, "bbpu_test: start (charger %s, f32k_off=%d clear_alarm=%d)\n",
		  (chr & MT6351_CHR_CON0_CHRDET) ? "PRESENT" : "absent",
		  f32k_off, clear_alarm);
	bbpu = mt6351_bbpu_power_down(p, &con_before);
	dev_emerg(dev, "bbpu_test: t=0 RTC_BBPU 0x%04x (bit2 %s)\n", bbpu,
		  (bbpu & MT6351_RTC_BBPU_BBPU) ? "set" : "clear");

	prev = bbpu;
	for (i = 1; i <= 100; i++) {
		msleep(20);
		if (regmap_read(p->pmic, MT6351_RTC_BBPU, &bbpu))
			continue;
		if (bbpu != prev) {
			dev_emerg(dev, "bbpu_test: t=%ums RTC_BBPU 0x%04x\n",
				  i * 20, bbpu);
			prev = bbpu;
		}
	}

	dev_emerg(dev, "bbpu_test: STILL ALIVE after 2 s, RTC_BBPU 0x%04x; restoring power-on state\n",
		  bbpu);
	regmap_write(p->pmic, MT6351_RTC_BBPU,
		     MT6351_RTC_BBPU_KEY | MT6351_RTC_BBPU_AUTO |
		     MT6351_RTC_BBPU_BBPU | MT6351_RTC_BBPU_PWREN);
	mt6351_rtc_trigger(p->pmic);
	regmap_write(p->pmic, MT6351_RTC_CON, con_before);
	mt6351_rtc_trigger(p->pmic);
	regmap_read(p->pmic, MT6351_RTC_BBPU, &bbpu);
	regmap_read(p->pmic, MT6351_RTC_CON, &con_before);
	dev_emerg(dev, "bbpu_test: restored RTC_BBPU 0x%04x RTC_CON 0x%04x\n",
		  bbpu, con_before);
	return count;
}
static DEVICE_ATTR_WO(bbpu_test);

static struct attribute *mt6797_power_attrs[] = {
	&dev_attr_rtc_regs.attr,
	&dev_attr_bbpu_test.attr,
	NULL
};
ATTRIBUTE_GROUPS(mt6797_power);

static int mt6797_power_probe(struct platform_device *pdev)
{
	struct mt6797_power *p;
	struct platform_device *pmic_pdev;
	struct device_node *np;
	struct resource *res;
	int ret;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;

	/*
	 * Do NOT use devm_platform_ioremap_resource(): the TOPRGU/WDT block
	 * at 0x10007000 is shared with the mainline mtk_wdt driver (bound
	 * through the watchdog@10007000 node). devm_ioremap_resource()
	 * calls devm_request_mem_region(), so whichever driver probes first
	 * would make the other fail with -EBUSY — and if mtk_wdt loses, the
	 * LK-armed watchdog is never kicked and the SoC resets a few
	 * seconds into every boot (observed on glass 2026-09-10). Map the
	 * resource without claiming it.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	p->wdt_base = devm_ioremap(&pdev->dev, res->start,
				   resource_size(res));
	if (!p->wdt_base)
		return -ENOMEM;

	np = of_parse_phandle(pdev->dev.of_node, "mediatek,pmic", 0);
	if (!np) {
		dev_err(&pdev->dev, "missing mediatek,pmic phandle\n");
		return -EINVAL;
	}
	pmic_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pmic_pdev)
		return -EPROBE_DEFER;

	p->pmic = dev_get_regmap(&pmic_pdev->dev, NULL);
	platform_device_put(pmic_pdev);
	if (!p->pmic)
		return -EPROBE_DEFER;

	/*
	 * Sanity-check that the RTC address window is reachable. This is
	 * informative only: never let a failed probe take the restart
	 * handler down with it.
	 */
	{
		unsigned int val;

		ret = regmap_read(p->pmic, MT6351_RTC_BBPU, &val);
		if (ret)
			dev_warn(&pdev->dev,
				 "cannot read MT6351 RTC space: %d (poweroff may not work)\n",
				 ret);
		else
			dev_info(&pdev->dev,
				 "MT6351 RTC_BBPU readback 0x%04x\n", val);
	}

	platform_set_drvdata(pdev, p);
	mt6797_power_priv = p;

	p->restart_nb.notifier_call = mt6797_restart;
	/* Above the mainline mtk_wdt handler's 128 (which is broken here). */
	p->restart_nb.priority = 200;
	ret = register_restart_handler(&p->restart_nb);
	if (ret) {
		dev_err(&pdev->dev, "cannot register restart handler: %d\n",
			ret);
		return ret;
	}

	/*
	 * RTC writes happen here, while interrupts are still on.
	 * NOT SYS_OFF_PRIO_PLATFORM: that priority uses ONE static handler
	 * slot (kernel/reboot.c alloc_sys_off_handler), so taking it here
	 * made register_platform_power_off() below fail with -EBUSY and the
	 * whole driver (restart + poweroff) unbind (on glass 2026-10-05).
	 */
	ret = devm_register_sys_off_handler(&pdev->dev,
					    SYS_OFF_MODE_POWER_OFF_PREPARE,
					    SYS_OFF_PRIO_DEFAULT,
					    mt6797_power_off_prepare, p);
	if (ret)
		dev_warn(&pdev->dev,
			 "cannot register poweroff-prepare handler: %d (BBPU write falls back to the IRQs-off path)\n",
			 ret);

	ret = register_platform_power_off(mt6797_power_off);
	if (ret) {
		dev_err(&pdev->dev, "cannot register poweroff handler: %d\n",
			ret);
		unregister_restart_handler(&p->restart_nb);
		return ret;
	}

	dev_info(&pdev->dev,
		 "MT6797 restart + MT6351 poweroff handlers registered\n");
	return 0;
}

static int mt6797_power_remove(struct platform_device *pdev)
{
	struct mt6797_power *p = platform_get_drvdata(pdev);

	unregister_platform_power_off(mt6797_power_off);
	unregister_restart_handler(&p->restart_nb);
	mt6797_power_priv = NULL;

	return 0;
}

static const struct of_device_id mt6797_power_of_match[] = {
	{ .compatible = "mediatek,mt6797-power" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mt6797_power_of_match);

static struct platform_driver mt6797_power_driver = {
	.probe	= mt6797_power_probe,
	.remove	= mt6797_power_remove,
	.driver	= {
		.name		= "mt6797-power",
		.of_match_table	= mt6797_power_of_match,
		.dev_groups	= mt6797_power_groups,
	},
};
module_platform_driver(mt6797_power_driver);

MODULE_AUTHOR("Gemini PDA Linux port");
MODULE_DESCRIPTION("MT6797 restart + MT6351 poweroff (Gemini PDA)");
MODULE_LICENSE("GPL");
