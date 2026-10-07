// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek LK "devinfo" handoff as a read-only NVMEM device.
 *
 * The MediaTek LK bootloader (MT6797 and relatives) copies the efuse
 * "devinfo" words it read at boot into the device tree it hands to the
 * kernel, as /chosen/atag,devinfo (lk/platform/mt6797/atags.c,
 * target_atag_devinfo_data()). The property is an opaque little-endian
 * word array, not a DT cell array:
 *
 *   word 0          ATAG size in words (= payload + 3)
 *   word 1          tag 0x41000804
 *   word 2 .. n+1   payload: get_devinfo_with_index(0 .. n-1)
 *   word n+2        payload count n
 *
 * The vendor kernel reads its calibration (thermal, PTP/EEM, speed bin)
 * through get_devinfo_with_index(), i.e. from this copy, and never maps
 * the efuse block itself. This driver exposes the payload as an NVMEM
 * device: byte offset = 4 * devinfo index, so DT cells can name the
 * words they need (e.g. thermal calibration = indexes 31..33 =
 * reg <0x7c 0xc>, the efuse words at 0x10206180..0x10206188).
 *
 * Idea and the LK wire format as recovered by the gemini-pda-mainline
 * project (github.com/ixoo/gemini-pda-mainline,
 * experiments/2026-07-13-mt6797-thermal-recovery).
 */

#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

#define LK_DEVINFO_TAG		0x41000804
#define LK_DEVINFO_OVERHEAD	3	/* size + tag + trailing count */

struct mtk_lk_devinfo {
	u32 *words;
	unsigned int count;
};

static int mtk_lk_devinfo_read(void *context, unsigned int offset,
			       void *val, size_t bytes)
{
	struct mtk_lk_devinfo *di = context;
	size_t size = di->count * sizeof(u32);

	if (offset >= size || bytes > size - offset)
		return -EINVAL;

	memcpy(val, (u8 *)di->words + offset, bytes);

	return 0;
}

static int mtk_lk_devinfo_parse(struct device *dev, struct mtk_lk_devinfo *di)
{
	struct device_node *chosen;
	const u8 *prop;
	unsigned int words, count, i;
	int len, ret = 0;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;

	prop = of_get_property(chosen, "atag,devinfo", &len);
	if (!prop) {
		ret = dev_err_probe(dev, -ENODEV, "no /chosen/atag,devinfo\n");
		goto out;
	}

	words = len / sizeof(u32);
	if (len % sizeof(u32) || words <= LK_DEVINFO_OVERHEAD ||
	    get_unaligned_le32(prop) != words ||
	    get_unaligned_le32(prop + 4) != LK_DEVINFO_TAG) {
		dev_err(dev, "malformed atag,devinfo (%d bytes)\n", len);
		ret = -EINVAL;
		goto out;
	}

	count = words - LK_DEVINFO_OVERHEAD;
	if (get_unaligned_le32(prop + (words - 1) * sizeof(u32)) != count) {
		dev_err(dev, "atag,devinfo count mismatch\n");
		ret = -EINVAL;
		goto out;
	}

	di->words = devm_kcalloc(dev, count, sizeof(u32), GFP_KERNEL);
	if (!di->words) {
		ret = -ENOMEM;
		goto out;
	}

	/* Stored in CPU (little-endian) order; nvmem consumers read u32s */
	for (i = 0; i < count; i++)
		di->words[i] = get_unaligned_le32(prop + (i + 2) * sizeof(u32));
	di->count = count;

out:
	of_node_put(chosen);
	return ret;
}

static int mtk_lk_devinfo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nvmem_config config = {
		.dev = dev,
		.name = "mtk-lk-devinfo",
		.id = NVMEM_DEVID_NONE,
		.owner = THIS_MODULE,
		.add_legacy_fixed_of_cells = true,
		.read_only = true,
		.root_only = true,
		.word_size = 4,
		.stride = 4,
		.reg_read = mtk_lk_devinfo_read,
	};
	struct mtk_lk_devinfo *di;
	int ret;

	di = devm_kzalloc(dev, sizeof(*di), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	ret = mtk_lk_devinfo_parse(dev, di);
	if (ret)
		return ret;

	config.priv = di;
	config.size = di->count * sizeof(u32);

	return PTR_ERR_OR_ZERO(devm_nvmem_register(dev, &config));
}

static const struct of_device_id mtk_lk_devinfo_of_match[] = {
	{ .compatible = "mediatek,mt6797-lk-devinfo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_lk_devinfo_of_match);

static struct platform_driver mtk_lk_devinfo_driver = {
	.probe = mtk_lk_devinfo_probe,
	.driver = {
		.name = "mtk-lk-devinfo",
		.of_match_table = mtk_lk_devinfo_of_match,
	},
};
module_platform_driver(mtk_lk_devinfo_driver);

MODULE_DESCRIPTION("MediaTek LK devinfo (efuse copy) NVMEM provider");
MODULE_LICENSE("GPL");
