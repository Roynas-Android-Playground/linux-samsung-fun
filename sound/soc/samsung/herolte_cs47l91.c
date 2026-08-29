// SPDX-License-Identifier: GPL-2.0-only
/*
 * Samsung Galaxy S7 (herolte) audio machine driver.
 *
 * Two links: the AP's I2S0 to the CS47L91's AIF1, and a codec-to-codec link
 * from AIF4 to the MAX98506 speaker amplifier. Modem voice (AIF2), Bluetooth
 * SCO (AIF3) and the DSP offload paths are not wired up.
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "../codecs/moon.h"
#include "i2s.h"

/* XCLKOUT, muxed to the external oscillator */
#define HEROLTE_MCLK_RATE	24000000U

struct herolte_priv {
	struct snd_soc_component *component;
	unsigned int sysclk_rate;
};

static int herolte_start_sysclk(struct snd_soc_card *card)
{
	struct herolte_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *component = priv->component;
	int ret;

	ret = snd_soc_component_set_pll(component, MOON_FLL1_REFCLK,
					ARIZONA_FLL_SRC_MCLK1,
					HEROLTE_MCLK_RATE, priv->sysclk_rate);
	if (ret < 0) {
		dev_err(component->dev, "failed to set FLL1 source: %d\n", ret);
		return ret;
	}

	ret = snd_soc_component_set_pll(component, MOON_FLL1,
					ARIZONA_FLL_SRC_MCLK1,
					HEROLTE_MCLK_RATE, priv->sysclk_rate);
	if (ret < 0) {
		dev_err(component->dev, "failed to start FLL1: %d\n", ret);
		return ret;
	}

	ret = snd_soc_component_set_sysclk(component, ARIZONA_CLK_SYSCLK,
					   ARIZONA_CLK_SRC_FLL1,
					   priv->sysclk_rate, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(component->dev, "failed to set SYSCLK source: %d\n", ret);
		return ret;
	}

	return 0;
}

static int herolte_stop_sysclk(struct snd_soc_card *card)
{
	struct herolte_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_component *component = priv->component;
	int ret;

	ret = snd_soc_component_set_pll(component, MOON_FLL1, 0, 0, 0);
	if (ret < 0) {
		dev_err(component->dev, "failed to stop FLL1: %d\n", ret);
		return ret;
	}

	ret = snd_soc_component_set_sysclk(component, ARIZONA_CLK_SYSCLK,
					   ARIZONA_CLK_SRC_FLL1, 0, 0);
	if (ret < 0) {
		dev_err(component->dev, "failed to stop SYSCLK: %d\n", ret);
		return ret;
	}

	return 0;
}

static int herolte_aif1_hw_params(struct snd_pcm_substream *substream,
				  struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = snd_soc_substream_to_rtd(substream);
	struct snd_soc_component *component = snd_soc_rtd_to_codec(rtd, 0)->component;
	struct herolte_priv *priv = snd_soc_card_get_drvdata(rtd->card);

	switch (params_rate(params)) {
	case 4000:
	case 8000:
	case 12000:
	case 16000:
	case 24000:
	case 32000:
	case 48000:
	case 96000:
	case 192000:
		priv->sysclk_rate = 147456000U;
		break;
	case 11025:
	case 22050:
	case 44100:
	case 88200:
	case 176400:
		priv->sysclk_rate = 135475200U;
		break;
	default:
		dev_err(component->dev, "unsupported sample rate: %d\n",
			params_rate(params));
		return -EINVAL;
	}

	return herolte_start_sysclk(rtd->card);
}

static const struct snd_soc_ops herolte_aif1_ops = {
	.hw_params = herolte_aif1_hw_params,
};

static const struct snd_soc_pcm_stream herolte_spk_params = {
	.formats	= SNDRV_PCM_FMTBIT_S16_LE,
	.rate_min	= 48000,
	.rate_max	= 48000,
	.channels_min	= 2,
	.channels_max	= 2,
};

static const struct snd_soc_dapm_widget herolte_dapm_widgets[] = {
	SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("SPK", NULL),
	SND_SOC_DAPM_SPK("RCV", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Sub Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

SND_SOC_DAILINK_DEFS(aif1,
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "moon-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(spk,
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "moon-aif4")),
	DAILINK_COMP_ARRAY(COMP_CODEC(NULL, "max98506-aif1")));

static struct snd_soc_dai_link herolte_dai_links[] = {
	{
		.name		= "CS47L91 AIF1",
		.stream_name	= "HiFi Primary",
		.ops		= &herolte_aif1_ops,
		.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBP_CFP,
		SND_SOC_DAILINK_REG(aif1),
	}, {
		.name		= "CS47L91 SPK",
		.stream_name	= "Speaker",
		.dai_fmt	= SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
				  SND_SOC_DAIFMT_CBP_CFP,
		.c2c_params	= &herolte_spk_params,
		.num_c2c_params	= 1,
		.ignore_suspend	= 1,
		SND_SOC_DAILINK_REG(spk),
	},
};

static int herolte_late_probe(struct snd_soc_card *card)
{
	struct herolte_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;
	int ret;

	rtd = snd_soc_get_pcm_runtime(card, &herolte_dai_links[0]);
	codec_dai = snd_soc_rtd_to_codec(rtd, 0);
	priv->component = codec_dai->component;

	ret = snd_soc_component_set_sysclk(priv->component, ARIZONA_CLK_OPCLK, 0,
					   HEROLTE_MCLK_RATE, SND_SOC_CLOCK_OUT);
	if (ret < 0)
		dev_err(codec_dai->dev, "failed to set OPCLK: %d\n", ret);

	return ret;
}

static int herolte_set_bias_level(struct snd_soc_card *card,
				  struct snd_soc_dapm_context *dapm,
				  enum snd_soc_bias_level level)
{
	struct snd_soc_dapm_context *card_dapm = snd_soc_card_to_dapm(card);
	struct herolte_priv *priv = snd_soc_card_get_drvdata(card);

	if (!priv->component ||
	    snd_soc_dapm_to_dev(dapm) != priv->component->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		if (snd_soc_dapm_get_bias_level(card_dapm) == SND_SOC_BIAS_OFF)
			herolte_start_sysclk(card);
		break;
	case SND_SOC_BIAS_OFF:
		herolte_stop_sysclk(card);
		break;
	default:
		break;
	}

	return 0;
}

static struct snd_soc_card herolte_card = {
	.owner			= THIS_MODULE,
	.dai_link		= herolte_dai_links,
	.num_links		= ARRAY_SIZE(herolte_dai_links),
	.dapm_widgets		= herolte_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(herolte_dapm_widgets),
	.late_probe		= herolte_late_probe,
	.set_bias_level		= herolte_set_bias_level,
};

static int herolte_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card = &herolte_card;
	struct device_node *cpu, *codec, *amp;
	struct herolte_priv *priv;
	struct of_phandle_args args;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	card->dev = dev;
	snd_soc_card_set_drvdata(card, priv);

	ret = snd_soc_of_parse_card_name(card, "model");
	if (ret < 0)
		return dev_err_probe(dev, ret, "card name is not specified\n");

	ret = snd_soc_of_parse_audio_routing(card, "audio-routing");
	if (ret < 0)
		return dev_err_probe(dev, ret, "audio routing is invalid\n");

	ret = of_parse_phandle_with_args(dev->of_node, "i2s-controller",
					 "#sound-dai-cells", 0, &args);
	if (ret)
		return dev_err_probe(dev, ret, "i2s-controller is invalid\n");
	cpu = args.np;

	codec = of_parse_phandle(dev->of_node, "audio-codec", 0);
	amp = of_parse_phandle(dev->of_node, "speaker-codec", 0);
	if (!codec || !amp) {
		ret = dev_err_probe(dev, -EINVAL,
				    "audio-codec/speaker-codec is invalid\n");
		goto err_put;
	}

	herolte_dai_links[0].cpus->of_node = cpu;
	herolte_dai_links[0].platforms->of_node = cpu;
	herolte_dai_links[0].codecs->of_node = codec;
	herolte_dai_links[1].cpus->of_node = codec;
	herolte_dai_links[1].codecs->of_node = amp;

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		dev_err_probe(dev, ret, "failed to register card\n");

err_put:
	of_node_put(cpu);
	of_node_put(codec);
	of_node_put(amp);
	return ret;
}

static const struct of_device_id herolte_of_match[] = {
	{ .compatible = "samsung,herolte-audio" },
	{ },
};
MODULE_DEVICE_TABLE(of, herolte_of_match);

static struct platform_driver herolte_audio_driver = {
	.driver = {
		.name		= "herolte-audio",
		.of_match_table	= herolte_of_match,
		.pm		= &snd_soc_pm_ops,
	},
	.probe = herolte_audio_probe,
};
module_platform_driver(herolte_audio_driver);

MODULE_DESCRIPTION("ASoC machine driver for Samsung Galaxy S7 (herolte)");
MODULE_LICENSE("GPL");
