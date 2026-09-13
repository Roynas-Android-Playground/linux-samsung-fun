// SPDX-License-Identifier: GPL-2.0-only
/*
 * Playback-only MAX98506 smart amplifier support.
 *
 * The initial driver intentionally omits VMON/IMON, Maxim DSM, interrupts,
 * receiver pairing and user-adjustable gain or boost policy.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "max98506.h"

struct max98506_priv {
	struct regmap *regmap;
};

static const struct reg_sequence max98506_playback_config[] = {
	{ MAX98506_IRQ_ENABLE0, 0x00 },
	{ MAX98506_IRQ_ENABLE1, 0x00 },
	{ MAX98506_IRQ_ENABLE2, 0x00 },
	{ MAX98506_DAI_CLK_MODE1, MAX98506_DAI_CLK_SOURCE_BCLK | 0x06 },
	{ MAX98506_DAI_CLK_MODE2, MAX98506_DAI_SR_48000 |
					 MAX98506_DAI_BSEL_32 },
	{ MAX98506_FORMAT, MAX98506_DAI_CHANSZ_16 | MAX98506_DAI_DLY_I2S },
	{ MAX98506_TDM_SLOT_SELECT, MAX98506_TDM_STEREO_INPUT },
	{ MAX98506_DOUT_CFG_VMON, 0x00 },
	{ MAX98506_DOUT_CFG_IMON, 0x00 },
	{ MAX98506_DOUT_HIZ_CFG1, 0xff },
	{ MAX98506_DOUT_HIZ_CFG2, 0xff },
	{ MAX98506_DOUT_HIZ_CFG3, 0xff },
	{ MAX98506_DOUT_HIZ_CFG4, 0xf0 },
	{ MAX98506_FILTERS, 0xd9 },
	{ MAX98506_GAIN, MAX98506_DAC_IN_DIV2_SUM | MAX98506_GAIN_MIN },
	{ MAX98506_GAIN_RAMPING, MAX98506_RAMP_ZCD_ENABLE },
	{ MAX98506_SPK_AMP, 0x02 },
	{ MAX98506_ALC_CONFIGURATION, 0x12 },
	{ MAX98506_BOOST_CONVERTER, 0x01 },
	{ MAX98506_CONFIGURATION, MAX98506_CONFIGURATION_6V5 },
	{ MAX98506_BOOST_LIMITER, MAX98506_BOOST_ILIM_MIN },
};

static int max98506_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) !=
	    SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S)
		return -EINVAL;
	if ((fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF)
		return -EINVAL;

	return 0;
}

static int max98506_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *dai)
{
	struct max98506_priv *max98506 =
		snd_soc_component_get_drvdata(dai->component);

	if (params_rate(params) != 48000 || params_channels(params) != 2 ||
	    params_format(params) != SNDRV_PCM_FORMAT_S16_LE)
		return -EINVAL;

	return regmap_multi_reg_write(max98506->regmap, max98506_playback_config,
				      ARRAY_SIZE(max98506_playback_config));
}

static const struct snd_soc_dai_ops max98506_dai_ops = {
	.set_fmt = max98506_set_fmt,
	.hw_params = max98506_hw_params,
};

static struct snd_soc_dai_driver max98506_dai = {
	.name = "max98506-aif1",
	.playback = {
		.stream_name = "HiFi Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &max98506_dai_ops,
};

static int max98506_amp_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct max98506_priv *max98506 = snd_soc_component_get_drvdata(component);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = regmap_write(max98506->regmap, MAX98506_BLOCK_ENABLE,
				   MAX98506_BLOCK_ENABLE_PLAYBACK);
		if (ret)
			return ret;

		ret = regmap_write(max98506->regmap, MAX98506_GLOBAL_ENABLE,
				   MAX98506_GLOBAL_ENABLE_ON);
		if (ret)
			regmap_write(max98506->regmap, MAX98506_BLOCK_ENABLE, 0);
		return ret;
	case SND_SOC_DAPM_POST_PMD:
		ret = regmap_write(max98506->regmap, MAX98506_GLOBAL_ENABLE, 0);
		if (regmap_write(max98506->regmap, MAX98506_BLOCK_ENABLE, 0) && !ret)
			ret = -EIO;
		return ret;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dapm_widget max98506_dapm_widgets[] = {
	SND_SOC_DAPM_AIF_IN("AIFIN", "HiFi Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_DAC_E("Speaker Amp", NULL, SND_SOC_NOPM, 0, 0,
			   max98506_amp_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("SPKOUT"),
};

static const struct snd_soc_dapm_route max98506_routes[] = {
	{ "Speaker Amp", NULL, "AIFIN" },
	{ "SPKOUT", NULL, "Speaker Amp" },
};

static const struct snd_soc_component_driver max98506_component = {
	.dapm_widgets = max98506_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(max98506_dapm_widgets),
	.dapm_routes = max98506_routes,
	.num_dapm_routes = ARRAY_SIZE(max98506_routes),
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config max98506_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX98506_VERSION,
	.cache_type = REGCACHE_NONE,
};

static void max98506_disable(void *data)
{
	struct max98506_priv *max98506 = data;

	regmap_write(max98506->regmap, MAX98506_GLOBAL_ENABLE, 0);
	regmap_write(max98506->regmap, MAX98506_BLOCK_ENABLE, 0);
}

static int max98506_i2c_probe(struct i2c_client *i2c)
{
	struct max98506_priv *max98506;
	unsigned int version;
	int ret;

	max98506 = devm_kzalloc(&i2c->dev, sizeof(*max98506), GFP_KERNEL);
	if (!max98506)
		return -ENOMEM;

	max98506->regmap = devm_regmap_init_i2c(i2c, &max98506_regmap_config);
	if (IS_ERR(max98506->regmap))
		return dev_err_probe(&i2c->dev, PTR_ERR(max98506->regmap),
				     "failed to initialize regmap\n");

	ret = regmap_read(max98506->regmap, MAX98506_VERSION, &version);
	if (ret)
		return dev_err_probe(&i2c->dev, ret, "failed to read revision\n");
	if (version != 0x40 && version != 0x50 && version != 0x80)
		return dev_err_probe(&i2c->dev, -ENODEV,
				     "unsupported revision %#x\n", version);

	/* Probe must never leave an inherited amplifier enabled. */
	ret = regmap_write(max98506->regmap, MAX98506_GLOBAL_ENABLE, 0);
	if (ret)
		return ret;
	ret = regmap_write(max98506->regmap, MAX98506_BLOCK_ENABLE, 0);
	if (ret)
		return ret;

	ret = devm_add_action_or_reset(&i2c->dev, max98506_disable, max98506);
	if (ret)
		return ret;

	i2c_set_clientdata(i2c, max98506);
	ret = devm_snd_soc_register_component(&i2c->dev, &max98506_component,
					      &max98506_dai, 1);
	if (ret)
		return dev_err_probe(&i2c->dev, ret,
				     "failed to register component\n");

	dev_info(&i2c->dev, "MAX98506 revision %#x\n", version);
	return 0;
}

static const struct of_device_id max98506_of_match[] = {
	{ .compatible = "maxim,max98506" },
	{ }
};
MODULE_DEVICE_TABLE(of, max98506_of_match);

static const struct i2c_device_id max98506_i2c_id[] = {
	{ "max98506" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max98506_i2c_id);

static struct i2c_driver max98506_i2c_driver = {
	.driver = {
		.name = "max98506",
		.of_match_table = max98506_of_match,
	},
	.probe = max98506_i2c_probe,
	.id_table = max98506_i2c_id,
};
module_i2c_driver(max98506_i2c_driver);

MODULE_DESCRIPTION("Playback-only MAX98506 amplifier driver");
MODULE_LICENSE("GPL");
