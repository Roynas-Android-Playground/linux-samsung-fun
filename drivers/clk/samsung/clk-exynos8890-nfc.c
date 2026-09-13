// SPDX-License-Identifier: GPL-2.0-only
/* Exynos8890 PMU-controlled external 26 MHz clock outputs. */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define EXYNOS8890_PMU_DEBUG1		0x0a0c
#define EXYNOS8890_PMU_DEBUG		0x0a00
#define EXYNOS8890_CLKOUT_OSCCLK_NFC	BIT(1)
#define EXYNOS8890_CLKOUT_TCXO_26M	BIT(19)
#define EXYNOS8890_CLKOUT_TCXO_AUD	GENMASK(12, 8)
#define EXYNOS8890_CLKOUT0_DISABLE	BIT(0)

struct exynos8890_clkout_data {
	const char *name;
	u32 reg;
	u32 mask;
	u32 enabled;
	u32 disabled;
};

struct exynos8890_nfc_clk {
	struct clk_hw hw;
	struct regmap *pmu;
	const struct exynos8890_clkout_data *data;
};

#define to_exynos8890_nfc_clk(_hw) container_of(_hw, struct exynos8890_nfc_clk, hw)

static int exynos8890_nfc_clk_prepare(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);
	u32 value;
	int ret;

	ret = regmap_update_bits(clk->pmu, clk->data->reg, clk->data->mask,
				 clk->data->enabled);
	if (ret)
		return ret;
	ret = regmap_read(clk->pmu, clk->data->reg, &value);
	if (ret)
		return ret;
	if ((value & clk->data->mask) != clk->data->enabled)
		return -EIO;

	return 0;
}

static void exynos8890_nfc_clk_unprepare(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);
	u32 value = 0;
	int ret;

	ret = regmap_update_bits(clk->pmu, clk->data->reg, clk->data->mask,
				 clk->data->disabled);
	if (!ret)
		ret = regmap_read(clk->pmu, clk->data->reg, &value);
	if (ret || (value & clk->data->mask) != clk->data->disabled)
		pr_err_ratelimited("exynos8890-clkout: failed to disable %s: %d\n",
				   clk->data->name, ret ?: -EIO);
}

static int exynos8890_nfc_clk_is_prepared(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);
	u32 value;

	if (regmap_read(clk->pmu, clk->data->reg, &value))
		return 0;
	return (value & clk->data->mask) == clk->data->enabled;
}

static const struct clk_ops exynos8890_nfc_clk_ops = {
	.prepare = exynos8890_nfc_clk_prepare,
	.unprepare = exynos8890_nfc_clk_unprepare,
	.is_prepared = exynos8890_nfc_clk_is_prepared,
};

static int exynos8890_nfc_clk_probe(struct platform_device *pdev)
{
	struct clk_parent_data parent_data = { .index = 0 };
	struct clk_init_data init = { };
	struct exynos8890_nfc_clk *clk;
	int ret;

	clk = devm_kzalloc(&pdev->dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;
	clk->data = device_get_match_data(&pdev->dev);
	if (!clk->data)
		return -EINVAL;
	clk->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						   "samsung,pmu-syscon");
	if (IS_ERR(clk->pmu))
		return PTR_ERR(clk->pmu);
	init.name = clk->data->name;
	init.ops = &exynos8890_nfc_clk_ops;
	init.parent_data = &parent_data;
	init.num_parents = 1;
	clk->hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &clk->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
					   &clk->hw);
}

static const struct exynos8890_clkout_data exynos8890_nfc_clk_data = {
	.name = "oscclk_nfc",
	.reg = EXYNOS8890_PMU_DEBUG1,
	.mask = EXYNOS8890_CLKOUT_OSCCLK_NFC | EXYNOS8890_CLKOUT_TCXO_26M,
	.enabled = EXYNOS8890_CLKOUT_TCXO_26M,
	.disabled = EXYNOS8890_CLKOUT_OSCCLK_NFC,
};

static const struct exynos8890_clkout_data exynos8890_audio_clk_data = {
	.name = "oscclk_aud",
	.reg = EXYNOS8890_PMU_DEBUG,
	.mask = EXYNOS8890_CLKOUT_TCXO_AUD | EXYNOS8890_CLKOUT0_DISABLE,
	.enabled = EXYNOS8890_CLKOUT_TCXO_AUD,
	.disabled = EXYNOS8890_CLKOUT0_DISABLE,
};

static const struct of_device_id exynos8890_nfc_clk_of_match[] = {
	{ .compatible = "samsung,exynos8890-nfc-clock",
	  .data = &exynos8890_nfc_clk_data },
	{ .compatible = "samsung,exynos8890-audio-clock",
	  .data = &exynos8890_audio_clk_data },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_nfc_clk_of_match);

static struct platform_driver exynos8890_nfc_clk_driver = {
	.probe = exynos8890_nfc_clk_probe,
	.driver = {
		.name = "exynos8890-nfc-clock",
		.of_match_table = exynos8890_nfc_clk_of_match,
	},
};
module_platform_driver(exynos8890_nfc_clk_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 external clock outputs");
MODULE_LICENSE("GPL");
