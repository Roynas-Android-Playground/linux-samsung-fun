// SPDX-License-Identifier: GPL-2.0-only
/* Exynos8890 PMU-controlled 26 MHz NFC clock output. */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define EXYNOS8890_PMU_DEBUG1		0x0a0c
#define EXYNOS8890_CLKOUT_OSCCLK_NFC	BIT(1)
#define EXYNOS8890_CLKOUT_TCXO_26M	BIT(19)

struct exynos8890_nfc_clk {
	struct clk_hw hw;
	struct regmap *pmu;
};

#define to_exynos8890_nfc_clk(_hw) container_of(_hw, struct exynos8890_nfc_clk, hw)

static int exynos8890_nfc_clk_prepare(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);

	return regmap_update_bits(clk->pmu, EXYNOS8890_PMU_DEBUG1,
				  EXYNOS8890_CLKOUT_OSCCLK_NFC |
				  EXYNOS8890_CLKOUT_TCXO_26M,
				  EXYNOS8890_CLKOUT_TCXO_26M);
}

static void exynos8890_nfc_clk_unprepare(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);

	regmap_update_bits(clk->pmu, EXYNOS8890_PMU_DEBUG1,
			   EXYNOS8890_CLKOUT_OSCCLK_NFC |
			   EXYNOS8890_CLKOUT_TCXO_26M,
			   EXYNOS8890_CLKOUT_OSCCLK_NFC);
}

static int exynos8890_nfc_clk_is_prepared(struct clk_hw *hw)
{
	struct exynos8890_nfc_clk *clk = to_exynos8890_nfc_clk(hw);
	u32 value;

	if (regmap_read(clk->pmu, EXYNOS8890_PMU_DEBUG1, &value))
		return 0;
	return (value & (EXYNOS8890_CLKOUT_OSCCLK_NFC |
			 EXYNOS8890_CLKOUT_TCXO_26M)) ==
		EXYNOS8890_CLKOUT_TCXO_26M;
}

static const struct clk_ops exynos8890_nfc_clk_ops = {
	.prepare = exynos8890_nfc_clk_prepare,
	.unprepare = exynos8890_nfc_clk_unprepare,
	.is_prepared = exynos8890_nfc_clk_is_prepared,
};

static int exynos8890_nfc_clk_probe(struct platform_device *pdev)
{
	struct clk_parent_data parent_data = { .index = 0 };
	struct clk_init_data init = {
		.name = "oscclk_nfc",
		.ops = &exynos8890_nfc_clk_ops,
		.parent_data = &parent_data,
		.num_parents = 1,
	};
	struct exynos8890_nfc_clk *clk;
	int ret;

	clk = devm_kzalloc(&pdev->dev, sizeof(*clk), GFP_KERNEL);
	if (!clk)
		return -ENOMEM;
	clk->pmu = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						   "samsung,pmu-syscon");
	if (IS_ERR(clk->pmu))
		return PTR_ERR(clk->pmu);
	clk->hw.init = &init;
	ret = devm_clk_hw_register(&pdev->dev, &clk->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_simple_get,
					   &clk->hw);
}

static const struct of_device_id exynos8890_nfc_clk_of_match[] = {
	{ .compatible = "samsung,exynos8890-nfc-clock" },
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

MODULE_DESCRIPTION("Samsung Exynos8890 NFC clock output");
MODULE_LICENSE("GPL");
