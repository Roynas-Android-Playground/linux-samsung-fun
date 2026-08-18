// SPDX-License-Identifier: GPL-2.0-only
/* Minimal ASoC component for CS47L90/CS47L91 (Moon). */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/soc.h>

#include "moon.h"

static int moon_dai_startup(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	return -EOPNOTSUPP;
}

static const struct snd_soc_dai_ops moon_dai_ops = {
	.startup = moon_dai_startup,
};

#define MOON_DAI(_id, _name, _channels) { \
	.id = _id, \
	.name = _name, \
	.ops = &moon_dai_ops, \
	.playback = { \
		.stream_name = _name " Playback", \
		.channels_min = 1, \
		.channels_max = _channels, \
		.rates = MOON_RATES, \
		.formats = MOON_FORMATS, \
	}, \
	.capture = { \
		.stream_name = _name " Capture", \
		.channels_min = 1, \
		.channels_max = _channels, \
		.rates = MOON_RATES, \
		.formats = MOON_FORMATS, \
	}, \
}

static struct snd_soc_dai_driver moon_dais[] = {
	MOON_DAI(1, "moon-aif1", 8),
	MOON_DAI(2, "moon-aif2", 8),
	MOON_DAI(3, "moon-aif3", 2),
	MOON_DAI(4, "moon-aif4", 2),
};

static const struct snd_soc_component_driver moon_component = {
	.name = "moon-codec",
};

static int moon_codec_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev, &moon_component,
					       moon_dais, ARRAY_SIZE(moon_dais));
}

static const struct platform_device_id moon_codec_ids[] = {
	{ "moon-codec", 0 },
	{ }
};
MODULE_DEVICE_TABLE(platform, moon_codec_ids);

static struct platform_driver moon_codec_driver = {
	.probe = moon_codec_probe,
	.id_table = moon_codec_ids,
	.driver = {
		.name = "moon-codec",
	},
};
module_platform_driver(moon_codec_driver);

MODULE_DESCRIPTION("Minimal CS47L90/CS47L91 Moon ASoC component");
MODULE_AUTHOR("Nous Research");
MODULE_LICENSE("GPL");
