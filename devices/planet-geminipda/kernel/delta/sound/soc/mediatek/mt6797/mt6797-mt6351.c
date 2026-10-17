// SPDX-License-Identifier: GPL-2.0
//
// mt6797-mt6351.c  --  MT6797 MT6351 ALSA SoC machine driver
//
// Copyright (c) 2018 MediaTek Inc.
// Author: KaiChieh Chuang <kaichieh.chuang@mediatek.com>

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <sound/soc.h>

#include "mt6797-afe-common.h"

SND_SOC_DAILINK_DEFS(playback_1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_2,
	DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback_3,
	DAILINK_COMP_ARRAY(COMP_CPU("DL3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_2,
	DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_3,
	DAILINK_COMP_ARRAY(COMP_CPU("UL3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture_mono_1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL_MONO_1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(hostless_lpbk,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless LPBK DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(hostless_speech,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless Speech DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "mt6351-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(pcm1,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(pcm2,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link mt6797_mt6351_dai_links[] = {
	/* FE */
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback_1),
	},
	{
		.name = "Playback_2",
		.stream_name = "Playback_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback_2),
	},
	{
		.name = "Playback_3",
		.stream_name = "Playback_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback_3),
	},
	{
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture_1),
	},
	{
		.name = "Capture_2",
		.stream_name = "Capture_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture_2),
	},
	{
		.name = "Capture_3",
		.stream_name = "Capture_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture_3),
	},
	{
		.name = "Capture_Mono_1",
		.stream_name = "Capture_Mono_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture_mono_1),
	},
	{
		.name = "Hostless_LPBK",
		.stream_name = "Hostless_LPBK",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_lpbk),
	},
	{
		.name = "Hostless_Speech",
		.stream_name = "Hostless_Speech",
		.trigger = {SND_SOC_DPCM_TRIGGER_PRE,
			    SND_SOC_DPCM_TRIGGER_PRE},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_speech),
	},
	/* BE */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(primary_codec),
	},
	{
		.name = "PCM 1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(pcm1),
	},
	{
		.name = "PCM 2",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(pcm2),
	},
};

/*
 * [gemini 2026-10-16] Speaker amps under DAPM.
 *
 * The Gemini PDA speakers hang off the codec HPL/HPR drivers through
 * external amps enabled by SoC GPIO 243 + 244 (DT "speaker-amp-gpios").
 * The "Speaker" widget drives those GPIOs, so the amps are on only while
 * a playback stream is routed to the headphone drivers. SPK widgets get
 * POST_PMU / PRE_PMD events: amps on after the codec path is up, off
 * before it goes down (no pop). The "Speaker Switch" pin switch lets
 * userspace pick jack-only output. The jack itself has no detection, so
 * "Headphones" is always connected. Without the DT property the amps
 * are simply not driven.
 */
static int mt6797_mt6351_spk_amp_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol,
				       int event)
{
	struct gpio_descs *amp = snd_soc_card_get_drvdata(w->dapm->card);
	int on = SND_SOC_DAPM_EVENT_ON(event);
	unsigned int i;

	if (!amp)
		return 0;

	for (i = 0; i < amp->ndescs; i++)
		gpiod_set_value_cansleep(amp->desc[i], on);

	/* let the amp outputs settle before the codec drivers change */
	usleep_range(5000, 6000);
	return 0;
}

static const struct snd_soc_dapm_widget mt6797_mt6351_widgets[] = {
	SND_SOC_DAPM_HP("Headphones", NULL),
	SND_SOC_DAPM_SPK("Speaker", mt6797_mt6351_spk_amp_event),
};

static const struct snd_soc_dapm_route mt6797_mt6351_routes[] = {
	{ "Headphones", NULL, "Headphone L" },
	{ "Headphones", NULL, "Headphone R" },
	{ "Speaker", NULL, "Headphone L" },
	{ "Speaker", NULL, "Headphone R" },
};

static const struct snd_kcontrol_new mt6797_mt6351_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphones"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static struct snd_soc_card mt6797_mt6351_card = {
	.name = "mt6797-mt6351",
	.owner = THIS_MODULE,
	.dai_link = mt6797_mt6351_dai_links,
	.num_links = ARRAY_SIZE(mt6797_mt6351_dai_links),
	.controls = mt6797_mt6351_controls,
	.num_controls = ARRAY_SIZE(mt6797_mt6351_controls),
	.dapm_widgets = mt6797_mt6351_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt6797_mt6351_widgets),
	.dapm_routes = mt6797_mt6351_routes,
	.num_dapm_routes = ARRAY_SIZE(mt6797_mt6351_routes),
};

static int mt6797_mt6351_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt6797_mt6351_card;
	struct device_node *platform_node, *codec_node;
	struct snd_soc_dai_link *dai_link;
	struct gpio_descs *amp;
	int ret, i;

	card->dev = &pdev->dev;

	/* amps start off; DAPM turns them on with a playback stream */
	amp = devm_gpiod_get_array_optional(&pdev->dev, "speaker-amp",
					    GPIOD_OUT_LOW);
	if (IS_ERR(amp))
		return dev_err_probe(&pdev->dev, PTR_ERR(amp),
				     "speaker-amp-gpios\n");
	snd_soc_card_set_drvdata(card, amp);

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_err(&pdev->dev, "Property 'platform' missing or invalid\n");
		return -EINVAL;
	}
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->platforms->name)
			continue;
		dai_link->platforms->of_node = platform_node;
	}

	codec_node = of_parse_phandle(pdev->dev.of_node,
				      "mediatek,audio-codec", 0);
	if (!codec_node) {
		dev_err(&pdev->dev,
			"Property 'audio-codec' missing or invalid\n");
		ret = -EINVAL;
		goto put_platform_node;
	}
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->codecs->name)
			continue;
		dai_link->codecs->of_node = codec_node;
	}

	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_err(&pdev->dev, "%s snd_soc_register_card fail %d\n",
			__func__, ret);

	of_node_put(codec_node);
put_platform_node:
	of_node_put(platform_node);
	return ret;
}

#ifdef CONFIG_OF
static const struct of_device_id mt6797_mt6351_dt_match[] = {
	{.compatible = "mediatek,mt6797-mt6351-sound",},
	{}
};
MODULE_DEVICE_TABLE(of, mt6797_mt6351_dt_match);
#endif

static struct platform_driver mt6797_mt6351_driver = {
	.driver = {
		.name = "mt6797-mt6351",
#ifdef CONFIG_OF
		.of_match_table = mt6797_mt6351_dt_match,
#endif
	},
	.probe = mt6797_mt6351_dev_probe,
};

module_platform_driver(mt6797_mt6351_driver);

/* Module information */
MODULE_DESCRIPTION("MT6797 MT6351 ALSA SoC machine driver");
MODULE_AUTHOR("KaiChieh Chuang <kaichieh.chuang@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt6797 mt6351 soc card");

