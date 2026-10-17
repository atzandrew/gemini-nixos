// SPDX-License-Identifier: GPL-2.0
/*
 * mt6797-suspend — system suspend ("deep", /sys/power/mem_sleep) for the
 * MT6797 Gemini PDA, as a loadable module (suspend_set_ops is exported).
 *
 * mode=0 (probe): .enter just busy-waits probe_ms and returns. Tests that
 *   every driver's suspend/resume callbacks and the secondary-CPU
 *   offline/online path survive a real suspend, with no SPM involved.
 *
 * Power domains: genpd powers a domain off in suspend_noirq once all of
 * its devices have suspended. "mm" must never go off here (LK programmed
 * the display pipeline and nothing re-initialises it), and audio/conn are
 * kept for now. .begin sets GENPD_FLAG_ALWAYS_ON on the domains of the
 * devices in keep_pd[] and .end clears what it set. (Kernel fix later:
 * MTK_SCPD_ALWAYS_ON for mm in mtk-scpsys.)
 *
 * Unload restores "no platform suspend" (s2idle only).
 */
#include <linux/module.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/smp.h>
#include <linux/cpumask.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>

static uint mode;
module_param(mode, uint, 0644);
MODULE_PARM_DESC(mode, "0 = probe (enter busy-waits probe_ms, no SPM)");

static uint probe_ms = 5000;
module_param(probe_ms, uint, 0644);
MODULE_PARM_DESC(probe_ms, "mode 0: time spent in .enter (ms)");

#define SPM_PHYS		0x10006000
#define SPM_PWR_STATUS		0x180
#define SPM_PWR_STATUS_2ND	0x184
static void __iomem *spm;

static const char * const keep_pd[] = {
	"geminipda-drm",		/* mm */
	"11220000.audio-controller",	/* audio */
	"18070000.consys",		/* conn */
};
static struct generic_pm_domain *pinned[ARRAY_SIZE(keep_pd)];

static void pin_domains(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(keep_pd); i++) {
		struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL, keep_pd[i]);
		struct generic_pm_domain *pd;

		pinned[i] = NULL;
		if (!dev) {
			pr_warn("mt6797-suspend: %s not found\n", keep_pd[i]);
			continue;
		}
		if (dev->pm_domain) {
			pd = pd_to_genpd(dev->pm_domain);
			if (!(pd->flags & GENPD_FLAG_ALWAYS_ON)) {
				pd->flags |= GENPD_FLAG_ALWAYS_ON;
				pinned[i] = pd;
			}
			pr_info("mt6797-suspend: domain %s (%s) pinned on\n", pd->name, keep_pd[i]);
		}
		put_device(dev);
	}
}

static void unpin_domains(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(pinned); i++) {
		if (pinned[i])
			pinned[i]->flags &= ~GENPD_FLAG_ALWAYS_ON;
		pinned[i] = NULL;
	}
}

static int gem_suspend_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_MEM;
}

static int gem_suspend_begin(suspend_state_t state)
{
	pr_info("mt6797-suspend: begin (mode %u)\n", mode);
	pin_domains();
	return 0;
}

static int gem_suspend_enter(suspend_state_t state)
{
	pr_info("mt6797-suspend: enter on cpu%d, %u cpus online, irqs %s\n",
		raw_smp_processor_id(), num_online_cpus(),
		irqs_disabled() ? "off" : "ON");
	pr_info("mt6797-suspend: PWR_STATUS %08x / %08x\n",
		readl(spm + SPM_PWR_STATUS), readl(spm + SPM_PWR_STATUS_2ND));

	switch (mode) {
	case 0:
		mdelay(probe_ms);
		break;
	default:
		return -EINVAL;
	}
	pr_info("mt6797-suspend: leaving enter\n");
	return 0;
}

static void gem_suspend_end(void)
{
	unpin_domains();
	pr_info("mt6797-suspend: end\n");
}

static const struct platform_suspend_ops gem_suspend_ops = {
	.valid = gem_suspend_valid,
	.begin = gem_suspend_begin,
	.enter = gem_suspend_enter,
	.end   = gem_suspend_end,
};

static int __init gem_suspend_init(void)
{
	spm = ioremap(SPM_PHYS, 0x1000);
	if (!spm)
		return -ENOMEM;
	suspend_set_ops(&gem_suspend_ops);
	pr_info("mt6797-suspend: deep suspend registered (mode %u)\n", mode);
	return 0;
}

static void __exit gem_suspend_exit(void)
{
	suspend_set_ops(NULL);
	unpin_domains();
	iounmap(spm);
	pr_info("mt6797-suspend: unregistered\n");
}

module_init(gem_suspend_init);
module_exit(gem_suspend_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MT6797 system suspend (Gemini PDA)");
