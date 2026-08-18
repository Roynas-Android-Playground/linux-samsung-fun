// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common Clock Framework support for Exynos8890 CMU_MFC.
 *
 * The MFC-local clock controller gates the codec and two System MMUs
 * separately from the top-level 600 MHz MFC bus clock. Keep Q-channel
 * automation disabled for clocks owned by Linux during bring-up.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/samsung,exynos8890-cmu-mfc.h>

#include "clk.h"

#define MFC_NR_CLK	(CLK_GOUT_MFC_ACLK_MFC + 1)

#define CLK_CON_MUX_ACLK_MFC_600_USER	0x0200
#define CLK_CON_DIV_PCLK_MFC_150	0x0400
#define CLK_ENABLE_ACLK_MFC_600		0x0800
#define CLK_ENABLE_PCLK_MFC_150		0x0820
#define QCH_CTRL_MFC			0x2000
#define QCH_CTRL_SMMU_MFC_0		0x2024
#define QCH_CTRL_SMMU_MFC_1		0x2028

/* Match exynos8890_init_clocks() in clk-exynos8890.c. */
#define QCH_EN_MASK		BIT(0)
#define QCH_MASK		(GENMASK(19, 16) | BIT(12))
#define QCH_DIS			(QCH_MASK | FIELD_PREP(QCH_EN_MASK, 0))
#define QCH_OFF_START		0x2000
#define QCH_OFF_END		0x23ff

static const unsigned long mfc_clk_regs[] __initconst = {
	CLK_CON_MUX_ACLK_MFC_600_USER,
	CLK_CON_DIV_PCLK_MFC_150,
	CLK_ENABLE_ACLK_MFC_600,
	CLK_ENABLE_PCLK_MFC_150,
	QCH_CTRL_MFC,
	QCH_CTRL_SMMU_MFC_0,
	QCH_CTRL_SMMU_MFC_1,
};

PNAME(mfc_mux_aclk_mfc_600_user_p) = {
	"oscclk", "gout_top_aclk_mfc_600"
};

static const struct samsung_mux_clock mfc_mux_clks[] __initconst = {
	MUX(CLK_MOUT_MFC_ACLK_MFC_600_USER, "mout_mfc_aclk_mfc_600_user",
	    mfc_mux_aclk_mfc_600_user_p, CLK_CON_MUX_ACLK_MFC_600_USER, 12, 1),
};

static const struct samsung_div_clock mfc_div_clks[] __initconst = {
	DIV(CLK_DOUT_MFC_PCLK_MFC_150, "dout_mfc_pclk_mfc_150",
	    "mout_mfc_aclk_mfc_600_user", CLK_CON_DIV_PCLK_MFC_150, 0, 2),
};

static const struct samsung_gate_clock mfc_gate_clks[] __initconst = {
	GATE(CLK_GOUT_MFC_ACLK_MFC, "gout_mfc_aclk_mfc",
	     "mout_mfc_aclk_mfc_600_user", CLK_ENABLE_ACLK_MFC_600, 4, 0, 0),
	GATE(CLK_GOUT_MFC_ACLK_SMMU_MFC_0, "gout_mfc_aclk_smmu_mfc_0",
	     "mout_mfc_aclk_mfc_600_user", CLK_ENABLE_ACLK_MFC_600, 5, 0, 0),
	GATE(CLK_GOUT_MFC_ACLK_SMMU_MFC_1, "gout_mfc_aclk_smmu_mfc_1",
	     "mout_mfc_aclk_mfc_600_user", CLK_ENABLE_ACLK_MFC_600, 6, 0, 0),
	GATE(CLK_GOUT_MFC_PCLK_SMMU_MFC_0, "gout_mfc_pclk_smmu_mfc_0",
	     "dout_mfc_pclk_mfc_150", CLK_ENABLE_PCLK_MFC_150, 7, 0, 0),
	GATE(CLK_GOUT_MFC_PCLK_SMMU_MFC_1, "gout_mfc_pclk_smmu_mfc_1",
	     "dout_mfc_pclk_mfc_150", CLK_ENABLE_PCLK_MFC_150, 8, 0, 0),
};

static const struct samsung_cmu_info mfc_cmu_info __initconst = {
	.mux_clks		= mfc_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(mfc_mux_clks),
	.div_clks		= mfc_div_clks,
	.nr_div_clks		= ARRAY_SIZE(mfc_div_clks),
	.gate_clks		= mfc_gate_clks,
	.nr_gate_clks		= ARRAY_SIZE(mfc_gate_clks),
	.nr_clk_ids		= MFC_NR_CLK,
	.clk_regs		= mfc_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(mfc_clk_regs),
};

static void __init exynos8890_mfc_init_clocks(struct device_node *np)
{
	void __iomem *reg_base;
	size_t i;

	reg_base = of_iomap(np, 0);
	if (!reg_base)
		panic("%s: failed to map registers\n", __func__);

	for (i = 0; i < ARRAY_SIZE(mfc_clk_regs); i++) {
		unsigned long off = mfc_clk_regs[i];

		if (off >= QCH_OFF_START && off <= QCH_OFF_END)
			writel(QCH_DIS, reg_base + off);
	}

	iounmap(reg_base);
}

static int __init exynos8890_cmu_mfc_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;

	exynos8890_mfc_init_clocks(np);
	samsung_cmu_register_one(np, &mfc_cmu_info);

	return 0;
}

static const struct of_device_id exynos8890_cmu_mfc_of_match[] = {
	{ .compatible = "samsung,exynos8890-cmu-mfc" },
	{ },
};

static struct platform_driver exynos8890_cmu_mfc_driver __refdata = {
	.driver = {
		.name = "exynos8890-cmu-mfc",
		.of_match_table = exynos8890_cmu_mfc_of_match,
		.suppress_bind_attrs = true,
	},
	.probe = exynos8890_cmu_mfc_probe,
};

static int __init exynos8890_cmu_mfc_init(void)
{
	return platform_driver_register(&exynos8890_cmu_mfc_driver);
}
core_initcall(exynos8890_cmu_mfc_init);
