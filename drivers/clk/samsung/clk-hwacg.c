// SPDX-License-Identifier: GPL-2.0-only
/* Samsung hardware automatic clock-gating controls. */

#include <linux/iopoll.h>
#include <linux/slab.h>

#include "clk.h"

struct samsung_hwacg {
	struct clk_hw hw;
	void __iomem *reg;
	void __iomem *status_reg;
	void __iomem *manual_reg;
	/* Serializes register updates within one CMU provider. */
	spinlock_t *lock;
	u32 owned_mask;
	u32 auto_value;
	u32 force_value;
	u32 status_mask;
	u32 status_active;
	u32 manual_mask;
	u16 status_timeout_us;
};

#define to_samsung_hwacg(_hw) container_of(_hw, struct samsung_hwacg, hw)

static void samsung_hwacg_update(struct samsung_hwacg *hwacg, u32 value)
{
	unsigned long flags;
	u32 reg;

	spin_lock_irqsave(hwacg->lock, flags);
	if (hwacg->manual_mask) {
		reg = readl(hwacg->manual_reg);
		writel(reg | hwacg->manual_mask, hwacg->manual_reg);
	}
	reg = readl(hwacg->reg);
	reg &= ~hwacg->owned_mask;
	reg |= value & hwacg->owned_mask;
	writel(reg, hwacg->reg);
	spin_unlock_irqrestore(hwacg->lock, flags);
}

static int samsung_hwacg_enable(struct clk_hw *hw)
{
	struct samsung_hwacg *hwacg = to_samsung_hwacg(hw);
	u32 value;
	int ret;

	samsung_hwacg_update(hwacg, hwacg->force_value);
	if (!hwacg->status_mask)
		return 0;

	ret = readl_poll_timeout_atomic(hwacg->status_reg, value,
					(value & hwacg->status_mask) ==
					hwacg->status_active,
					1, hwacg->status_timeout_us);
	if (ret)
		samsung_hwacg_update(hwacg, hwacg->auto_value);

	return ret;
}

static void samsung_hwacg_disable(struct clk_hw *hw)
{
	struct samsung_hwacg *hwacg = to_samsung_hwacg(hw);

	samsung_hwacg_update(hwacg, hwacg->auto_value);
}

static int samsung_hwacg_is_enabled(struct clk_hw *hw)
{
	struct samsung_hwacg *hwacg = to_samsung_hwacg(hw);

	return (readl(hwacg->reg) & hwacg->owned_mask) ==
		(hwacg->force_value & hwacg->owned_mask);
}

static const struct clk_ops samsung_hwacg_ops = {
	.enable = samsung_hwacg_enable,
	.disable = samsung_hwacg_disable,
	.is_enabled = samsung_hwacg_is_enabled,
};

bool samsung_clk_hwacg_validate(const struct samsung_hwacg_clock *list,
			       unsigned int nr_clk, unsigned int nr_clk_ids)
{
	unsigned int i, j;

	for (i = 0; i < nr_clk; i++) {
		const struct samsung_hwacg_clock *clk = &list[i];
		bool qch = clk->type == SAMSUNG_HWACG_QCH;
		bool qstate = clk->type == SAMSUNG_HWACG_QSTATE;

		if (!clk->name || (!qch && !qstate) ||
		    clk->owned_mask != (qch ? SAMSUNG_QCH_MASK :
						 SAMSUNG_QSTATE_MASK) ||
		    clk->auto_value != (qch ? SAMSUNG_QCH_AUTO :
						 SAMSUNG_QSTATE_AUTO) ||
		    clk->force_value != (qch ? SAMSUNG_QCH_FORCE :
						  SAMSUNG_QSTATE_FORCE) ||
		    (clk->status_mask && !clk->status_timeout_us) ||
		    (clk->id && clk->id >= nr_clk_ids))
			return false;

		for (j = 0; j < i; j++) {
			const struct samsung_hwacg_clock *other = &list[j];

			if (clk->offset == other->offset ||
			    (clk->id && clk->id == other->id))
				return false;
		}
	}

	return true;
}

void __init samsung_clk_register_hwacg(struct samsung_clk_provider *ctx,
				       const struct samsung_hwacg_clock *list,
				       unsigned int nr_clk)
{
	unsigned int i;

	if (!samsung_clk_hwacg_validate(list, nr_clk, ctx->clk_data.num)) {
		pr_err("invalid Samsung HWACG clock table\n");
		return;
	}

	for (i = 0; i < nr_clk; i++, list++) {
		struct clk_init_data init = { };
		struct samsung_hwacg *hwacg;
		int ret;

		hwacg = kzalloc_obj(*hwacg);
		if (!hwacg)
			return;

		init.name = list->name;
		init.ops = &samsung_hwacg_ops;
		init.flags = list->flags;
		if (list->parent_name) {
			init.parent_names = &list->parent_name;
			init.num_parents = 1;
		}

		hwacg->hw.init = &init;
		hwacg->reg = ctx->reg_base + list->offset;
		hwacg->status_reg = ctx->reg_base + list->status_offset;
		hwacg->manual_reg = ctx->reg_base + list->manual_offset;
		hwacg->lock = &ctx->lock;
		hwacg->owned_mask = list->owned_mask;
		hwacg->auto_value = list->auto_value;
		hwacg->force_value = list->force_value;
		hwacg->status_mask = list->status_mask;
		hwacg->status_active = list->status_active;
		hwacg->manual_mask = list->manual_mask;
		hwacg->status_timeout_us = list->status_timeout_us;

		samsung_hwacg_update(hwacg, hwacg->auto_value);
		ret = clk_hw_register(ctx->dev, &hwacg->hw);
		if (ret) {
			pr_err("%s: failed to register HWACG clock: %d\n",
			       list->name, ret);
			kfree(hwacg);
			continue;
		}

		samsung_clk_add_lookup(ctx, &hwacg->hw, list->id);
	}
}
