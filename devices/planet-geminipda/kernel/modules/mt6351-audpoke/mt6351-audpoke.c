// SPDX-License-Identifier: GPL-2.0
/*
 * mt6351-audpoke.c — DEBUG ONLY: read/write MT6351 *audio* registers on the
 * Gemini PDA, for the 2026-10-16 headphone idle-buzz experiment
 * (notes/audio-dapm-amp.md). Not part of any image.
 *
 * Borrows the pwrap PMIC regmap like mt6351-probe. Writes are refused
 * outside the audio blocks, so no regulator / charger / RTC register can
 * be touched by mistake:
 *   0x023A-0x0242  TOP_CKPDN_CON0 (+SET/CLR)    audio clock gates
 *   0x029A-0x029E  TOP_CLKSQ (+SET/CLR)
 *   0x0800-0x080A  ZCD_CON0-5
 *   0x0CF2-0x0D12  AUDDEC/AUDENC_ANA_CON*
 *   0x2000-0x20FE  audio digital (AFE) block
 *
 * debugfs /sys/kernel/debug/mt6351_audpoke/reg:
 *   echo "ADDR VAL [MASK]" > reg     update_bits (MASK default 0xffff)
 *   echo "ADDR" > reg; cat reg       read one register
 */
#include <linux/debugfs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>

static struct platform_device *pwrap_pdev;
static struct regmap *pmic;
static struct dentry *dbg_dir;
static unsigned int last_addr = 0x0cf2;

static bool audio_reg(unsigned int a)
{
	return (a >= 0x023a && a <= 0x0242) || (a >= 0x029a && a <= 0x029e) ||
	       (a >= 0x0800 && a <= 0x080a) || (a >= 0x0cf2 && a <= 0x0d12) ||
	       (a >= 0x2000 && a <= 0x20fe);
}

static ssize_t reg_write(struct file *f, const char __user *ubuf,
			 size_t len, loff_t *ppos)
{
	char buf[64];
	unsigned int a, v, m = 0xffff;
	int n, ret;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = 0;
	n = sscanf(buf, "%x %x %x", &a, &v, &m);
	if (n < 1 || (a & 1) || !audio_reg(a))
		return -EINVAL;
	last_addr = a;
	if (n >= 2) {
		ret = regmap_update_bits(pmic, a, m & 0xffff, v & 0xffff);
		if (ret)
			return ret;
		pr_info("mt6351_audpoke: %04x <- %04x (mask %04x)\n", a, v, m);
	}
	return len;
}

static ssize_t reg_read(struct file *f, char __user *ubuf, size_t len,
			loff_t *ppos)
{
	char buf[32];
	unsigned int v = 0;
	int ret, n;

	ret = regmap_read(pmic, last_addr, &v);
	if (ret)
		return ret;
	n = scnprintf(buf, sizeof(buf), "%04x: %04x\n", last_addr, v);
	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations reg_fops = {
	.owner = THIS_MODULE,
	.read = reg_read,
	.write = reg_write,
	.llseek = default_llseek,
};

static int __init audpoke_init(void)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "mediatek,mt6797-pwrap");
	if (!np)
		return -ENODEV;
	pwrap_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pwrap_pdev)
		return -ENODEV;
	pmic = dev_get_regmap(&pwrap_pdev->dev, NULL);
	if (!pmic) {
		put_device(&pwrap_pdev->dev);
		return -ENODEV;
	}
	dbg_dir = debugfs_create_dir("mt6351_audpoke", NULL);
	debugfs_create_file("reg", 0600, dbg_dir, NULL, &reg_fops);
	pr_info("mt6351_audpoke: loaded (audio registers only)\n");
	return 0;
}

static void __exit audpoke_exit(void)
{
	debugfs_remove_recursive(dbg_dir);
	put_device(&pwrap_pdev->dev);
}

module_init(audpoke_init);
module_exit(audpoke_exit);
MODULE_DESCRIPTION("MT6351 audio register poke (debug)");
MODULE_LICENSE("GPL");
