// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exynos8890 bootloader ECT and per-die CPU ASV parser.
 *
 * The blob is firmware data, not executable input. All offsets, counts,
 * strings and products are checked before they are consumed. Missing or
 * malformed calibration data is fatal for consumers; there is deliberately
 * no generic-voltage fallback.
 */

#include <linux/arm-smccc.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/memremap.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <linux/soc/samsung/exynos8890-ect.h>

#define ECT_SIGNATURE		"PARA"
#define ECT_MAX_SIZE		0x19000
#define ECT_MAX_DESCRIPTORS	64
#define ECT_MAX_TABLES		16
#define ECT_MAX_GROUPS		16
#define ECT_VOLT_STEP_UV	6250
#define ECT_SSA_COLS		12
#define EXYNOS8890_ASV_BASE	0x101e9000
#define EXYNOS8890_ASV_WORDS	5
#define SMC_CMD_REG		((unsigned long)-101)
#define SMC_REG_CLASS_SFR_R	(BIT(31) | BIT(30))

struct ect_cursor {
	const u8 *base;
	const u8 *pos;
	const u8 *end;
};

struct exynos8890_fuse_domain {
	u8 group;
	s8 modified_group;
	u8 ssa10;
	u8 ssa11;
	u8 ssa0;
};

struct exynos8890_ect_data {
	struct exynos8890_ect_cpu_domain little;
	struct exynos8890_ect_cpu_domain big;
};

static DEFINE_MUTEX(exynos8890_ect_lock);
static struct exynos8890_ect_data *exynos8890_ect_data;

static int ect_get_u32(struct ect_cursor *cursor, u32 *value)
{
	if (cursor->end - cursor->pos < sizeof(*value))
		return -EINVAL;

	*value = get_unaligned_le32(cursor->pos);
	cursor->pos += sizeof(*value);
	return 0;
}

static int ect_get_s32(struct ect_cursor *cursor, s32 *value)
{
	u32 raw;
	int ret;

	ret = ect_get_u32(cursor, &raw);
	if (!ret)
		*value = raw;
	return ret;
}

static int ect_get_string(struct ect_cursor *cursor, char *value, size_t size)
{
	size_t padded;
	u32 length;
	int ret;

	ret = ect_get_u32(cursor, &length);
	if (ret)
		return ret;
	if (length >= size || check_add_overflow((size_t)length, 1UL, &padded))
		return -EINVAL;
	padded = ALIGN(padded, 4);
	if (padded > cursor->end - cursor->pos || cursor->pos[length] != '\0')
		return -EINVAL;

	memcpy(value, cursor->pos, length);
	value[length] = '\0';
	cursor->pos += padded;
	return 0;
}

static int ect_subcursor(const struct ect_cursor *blob, const u8 *base,
			 u32 offset, struct ect_cursor *cursor)
{
	if (offset > blob->end - base)
		return -EINVAL;

	cursor->base = base;
	cursor->pos = base + offset;
	cursor->end = blob->end;
	return 0;
}

static const u8 *ect_find_block(const struct ect_cursor *blob, const char *target)
{
	struct ect_cursor cursor = *blob;
	char name[EXYNOS8890_ECT_CLOCK_NAME_LEN];
	u32 count, offset, total;
	u32 ignored;
	int i, ret;

	if (cursor.end - cursor.pos < 16 || memcmp(cursor.pos, ECT_SIGNATURE, 4))
		return ERR_PTR(-EINVAL);
	cursor.pos += 8;
	ret = ect_get_u32(&cursor, &total);
	if (ret || total < 16 || total > cursor.end - cursor.base)
		return ERR_PTR(-EINVAL);
	cursor.end = cursor.base + total;
	ret = ect_get_u32(&cursor, &count);
	if (ret || !count || count > ECT_MAX_DESCRIPTORS)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < count; i++) {
		ret = ect_get_string(&cursor, name, sizeof(name));
		ret |= ect_get_u32(&cursor, &offset);
		if (ret || offset >= total)
			return ERR_PTR(-EINVAL);
		if (!strcmp(name, target)) {
			/* Ensure the pointed-to block has at least its fixed header. */
			struct ect_cursor check;

			if (ect_subcursor(&cursor, blob->base, offset, &check) ||
			    ect_get_u32(&check, &ignored))
				return ERR_PTR(-EINVAL);
			return blob->base + offset;
		}
	}

	return ERR_PTR(-ENOENT);
}

static int ect_find_named(const struct ect_cursor *blob, const u8 *block,
			  const char *target, u32 *parser_version,
			  struct ect_cursor *domain)
{
	struct ect_cursor cursor;
	char name[EXYNOS8890_ECT_CLOCK_NAME_LEN];
	u32 count, offset, ignored;
	int i, ret;

	cursor.base = block;
	cursor.pos = block;
	cursor.end = blob->end;
	ret = ect_get_u32(&cursor, parser_version);
	ret |= ect_get_u32(&cursor, &ignored);
	ret |= ect_get_u32(&cursor, &count);
	if (ret || !count || count > ECT_MAX_DESCRIPTORS)
		return -EINVAL;

	for (i = 0; i < count; i++) {
		ret = ect_get_string(&cursor, name, sizeof(name));
		ret |= ect_get_u32(&cursor, &offset);
		if (ret)
			return -EINVAL;
		if (!strcmp(name, target))
			return ect_subcursor(blob, block, offset, domain);
	}

	return -ENOENT;
}

static int ect_parse_dvfs(const struct ect_cursor *blob, const char *name,
			  struct exynos8890_ect_cpu_domain *domain)
{
	const u8 *block = ect_find_block(blob, "DVFS");
	struct ect_cursor cursor;
	u32 parser_version;
	size_t cells;
	int i, j, ret;

	if (IS_ERR(block))
		return PTR_ERR(block);
	ret = ect_find_named(blob, block, name, &parser_version, &cursor);
	if (ret)
		return ret;
	if (parser_version < 1 || parser_version > 3)
		return -EOPNOTSUPP;

	strscpy(domain->name, name, sizeof(domain->name));
	ret = ect_get_u32(&cursor, &domain->max_frequency);
	ret |= ect_get_u32(&cursor, &domain->min_frequency);
	if (parser_version >= 2) {
		ret |= ect_get_s32(&cursor, &domain->boot_level);
		ret |= ect_get_s32(&cursor, &domain->resume_level);
	} else {
		domain->boot_level = -1;
		domain->resume_level = -1;
	}
	ret |= ect_get_u32(&cursor, &domain->num_clocks);
	ret |= ect_get_u32(&cursor, &domain->num_levels);
	if (ret || !domain->num_clocks ||
	    domain->num_clocks > EXYNOS8890_ECT_MAX_CLOCKS ||
	    !domain->num_levels || domain->num_levels > EXYNOS8890_ECT_MAX_LEVELS)
		return -EINVAL;

	for (i = 0; i < domain->num_clocks; i++) {
		ret = ect_get_string(&cursor, domain->clock_names[i],
				     sizeof(domain->clock_names[i]));
		if (ret)
			return ret;
	}

	for (i = 0; i < domain->num_levels; i++) {
		u32 enabled;

		ret = ect_get_u32(&cursor, &domain->levels[i].rate_khz);
		ret |= ect_get_u32(&cursor, &enabled);
		if (ret)
			return ret;
		domain->levels[i].enabled = enabled;
	}

	if (check_mul_overflow((size_t)domain->num_levels,
			       (size_t)domain->num_clocks, &cells) ||
	    cells > (cursor.end - cursor.pos) / sizeof(u32))
		return -EINVAL;

	for (i = 0; i < domain->num_levels; i++)
		for (j = 0; j < domain->num_clocks; j++) {
			ret = ect_get_u32(&cursor,
					  &domain->levels[i].clock_values[j]);
			if (ret)
				return ret;
		}

	return 0;
}

static int ect_parse_gen_ssa(const struct ect_cursor *blob, const char *name,
			     u32 table_version, u32 *subgroup,
			     u32 *ssa0_base, u32 *ssa0_step, u32 ssa1[8])
{
	const u8 *block = ect_find_block(blob, "GEN");
	struct ect_cursor cursor;
	u32 parser_version, rows, cols, row, value;
	int i, j, ret;

	if (IS_ERR(block))
		return PTR_ERR(block);
	ret = ect_find_named(blob, block, name, &parser_version, &cursor);
	if (ret)
		return ret;
	if (parser_version < 1 || parser_version > 3)
		return -EOPNOTSUPP;
	ret = ect_get_u32(&cursor, &cols);
	if (ret)
		return ret;
	ret = ect_get_u32(&cursor, &rows);
	if (ret)
		return ret;
	if (cols < ECT_SSA_COLS || !rows)
		return -EINVAL;
	row = min(table_version, rows - 1);

	for (i = 0; i <= row; i++)
		for (j = 0; j < cols; j++) {
			ret = ect_get_u32(&cursor, &value);
			if (ret)
				return ret;
			if (i != row)
				continue;
			if (j == 1)
				*subgroup = value;
			else if (j == 2)
				*ssa0_base = value;
			else if (j == 3)
				*ssa0_step = value;
			else if (j >= 4 && j < 12)
				ssa1[j - 4] = value;
		}

	return 0;
}

static int ect_parse_margin(const struct ect_cursor *blob, const char *name,
			    u32 level, u32 group, u32 *margin_uv)
{
	const u8 *block = ect_find_block(blob, "MARGIN");
	struct ect_cursor cursor;
	u32 parser_version, groups, levels, value;
	size_t index, cells;
	int ret;

	if (PTR_ERR_OR_ZERO(block) == -ENOENT) {
		*margin_uv = 0;
		return 0;
	}
	if (IS_ERR(block))
		return PTR_ERR(block);
	ret = ect_find_named(blob, block, name, &parser_version, &cursor);
	if (ret == -ENOENT) {
		*margin_uv = 0;
		return 0;
	}
	if (ret)
		return ret;
	if (parser_version < 1 || parser_version > 2)
		return -EOPNOTSUPP;
	ret = ect_get_u32(&cursor, &groups);
	ret |= ect_get_u32(&cursor, &levels);
	if (ret || group >= groups || level >= levels ||
	    check_mul_overflow((size_t)groups, (size_t)levels, &cells))
		return -EINVAL;
	index = level * groups + group;

	if (parser_version >= 2) {
		if (cells > cursor.end - cursor.pos)
			return -EINVAL;
		*margin_uv = cursor.pos[index] * ECT_VOLT_STEP_UV;
		return 0;
	}
	if (cells > (cursor.end - cursor.pos) / sizeof(u32))
		return -EINVAL;
	cursor.pos += index * sizeof(u32);
	ret = ect_get_u32(&cursor, &value);
	if (!ret)
		*margin_uv = value;
	return ret;
}

static int ect_adjust_voltage(u32 base, u32 margin, u32 ssa1,
			      u32 ssa0_base, u32 ssa0, u32 ssa0_step,
			      u32 *voltage)
{
	u32 floor;

	if (check_add_overflow(base, margin, voltage) ||
	    check_add_overflow(*voltage, ssa1, voltage) ||
	    check_mul_overflow(ssa0, ssa0_step, &floor) ||
	    check_add_overflow(ssa0_base, floor, &floor))
		return -EOVERFLOW;
	*voltage = max(*voltage, floor);
	return 0;
}

static int ect_store_voltage(struct exynos8890_ect_cpu_domain *domain,
			     u32 frequency, u32 voltage, u32 enabled)
{
	int i;

	for (i = 0; i < domain->num_levels; i++) {
		if (domain->levels[i].rate_khz != frequency)
			continue;
		domain->levels[i].voltage_uv = voltage;
		domain->levels[i].enabled &= enabled;
		return 0;
	}

	return -ENOENT;
}

static int ect_apply_asv(const struct ect_cursor *blob, const char *name,
			 struct exynos8890_ect_cpu_domain *domain,
			 const struct exynos8890_fuse_domain *fuse,
			 u32 table_version, const char *ssa_name)
{
	const u8 *block = ect_find_block(blob, "ASV");
	struct ect_cursor cursor;
	struct ect_cursor scan;
	u32 parser_version, groups, levels, tables;
	u32 max_table_version = 0, effective_table_version;
	u32 frequencies[EXYNOS8890_ECT_MAX_LEVELS];
	u32 ssa1[8], subgroup = 0, ssa0_base = 0, ssa0_step = 0;
	s32 group = fuse->group + fuse->modified_group;
	bool found = false;
	int i, j, ret;

	if (IS_ERR(block))
		return PTR_ERR(block);
	ret = ect_find_named(blob, block, name, &parser_version, &cursor);
	if (ret)
		return ret;
	if (parser_version < 1 || parser_version > 3)
		return -EOPNOTSUPP;
	ret = ect_get_u32(&cursor, &groups);
	ret |= ect_get_u32(&cursor, &levels);
	ret |= ect_get_u32(&cursor, &tables);
	if (ret || !groups || groups > ECT_MAX_GROUPS || group < 0 || group >= groups ||
	    !levels || levels > EXYNOS8890_ECT_MAX_LEVELS ||
	    !tables || tables > ECT_MAX_TABLES)
		return -EINVAL;
	for (i = 0; i < levels; i++) {
		u32 raw_frequency;

		ret = ect_get_u32(&cursor, &raw_frequency);
		if (ret || check_mul_overflow(raw_frequency, 1000U,
					      &frequencies[i]))
			return ret ?: -EOVERFLOW;
	}

	scan = cursor;
	for (i = 0; i < tables; i++) {
		u32 version;
		size_t cells, skip;

		ret = ect_get_u32(&scan, &version);
		if (ret)
			return ret;
		max_table_version = max(max_table_version, version);
		if (parser_version >= 2) {
			if (check_mul_overflow((size_t)levels, sizeof(u32), &skip) ||
			    check_add_overflow(skip, 2 * sizeof(u32), &skip) ||
			    skip > scan.end - scan.pos)
				return -EINVAL;
			scan.pos += skip;
		}
		if (check_mul_overflow((size_t)levels, (size_t)groups, &cells) ||
		    (parser_version < 3 &&
		     check_mul_overflow(cells, sizeof(u32), &cells)) ||
		    cells > scan.end - scan.pos)
			return -EINVAL;
		scan.pos += cells;
	}
	effective_table_version = min(table_version, max_table_version);

	memset(ssa1, 0, sizeof(ssa1));
	ret = ect_parse_gen_ssa(blob, ssa_name, effective_table_version, &subgroup,
				&ssa0_base, &ssa0_step, ssa1);
	if (ret)
		return ret;

	for (i = 0; i < tables; i++) {
		u32 version;
		s32 boot, resume;
		u32 enabled[EXYNOS8890_ECT_MAX_LEVELS];
		size_t cells;

		ret = ect_get_u32(&cursor, &version);
		if (ret)
			return ret;
		for (j = 0; j < levels; j++)
			enabled[j] = 1;
		if (parser_version >= 2) {
			ret = ect_get_s32(&cursor, &boot);
			ret |= ect_get_s32(&cursor, &resume);
			for (j = 0; !ret && j < levels; j++)
				ret = ect_get_u32(&cursor, &enabled[j]);
			if (ret)
				return ret;
		}
		if (check_mul_overflow((size_t)levels, (size_t)groups, &cells))
			return -EINVAL;
		if (parser_version >= 3) {
			if (cells > cursor.end - cursor.pos)
				return -EINVAL;
			if (version == effective_table_version) {
				for (j = 0; j < levels; j++) {
					u32 margin, ssa, voltage;

					voltage = cursor.pos[j * groups + group] * ECT_VOLT_STEP_UV;
					ret = ect_parse_margin(blob, name, j, group, &margin);
					if (ret)
						return ret;
					ssa = j < subgroup ? ssa1[fuse->ssa10] :
							ssa1[fuse->ssa11 + 4];
					ret = ect_adjust_voltage(voltage, margin, ssa,
							 ssa0_base, fuse->ssa0,
							 ssa0_step, &voltage);
					if (ret)
						return ret;
					ret = ect_store_voltage(domain, frequencies[j],
							voltage, enabled[j]);
					if (ret && ret != -ENOENT)
						return ret;
				}
				found = true;
			}
			cursor.pos += cells;
		} else {
			if (cells > (cursor.end - cursor.pos) / sizeof(u32))
				return -EINVAL;
			if (version == effective_table_version) {
				for (j = 0; j < levels; j++) {
					u32 margin, ssa, voltage;
					size_t index = j * groups + group;

					voltage = get_unaligned_le32(cursor.pos +
							       index * sizeof(u32));
					ret = ect_parse_margin(blob, name, j, group, &margin);
					if (ret)
						return ret;
					ssa = j < subgroup ? ssa1[fuse->ssa10] :
							ssa1[fuse->ssa11 + 4];
					ret = ect_adjust_voltage(voltage, margin, ssa,
							 ssa0_base, fuse->ssa0,
							 ssa0_step, &voltage);
					if (ret)
						return ret;
					ret = ect_store_voltage(domain, frequencies[j],
							voltage, enabled[j]);
					if (ret && ret != -ENOENT)
						return ret;
				}
				found = true;
			}
			cursor.pos += cells * sizeof(u32);
		}
	}

	if (!found)
		return -ENOENT;
	for (i = 0; i < domain->num_levels; i++)
		if (domain->levels[i].enabled && !domain->levels[i].voltage_uv)
			return -EINVAL;
	return 0;
}

static int exynos8890_read_asv_words(u32 words[EXYNOS8890_ASV_WORDS])
{
	struct arm_smccc_res res;
	int i;

	for (i = 0; i < EXYNOS8890_ASV_WORDS; i++) {
		u32 address = EXYNOS8890_ASV_BASE + i * sizeof(u32);

		arm_smccc_smc(SMC_CMD_REG,
			      SMC_REG_CLASS_SFR_R | (address >> 2), 0, 0, 0, 0, 0, 0,
			      &res);
		if (res.a0)
			return -EIO;
		words[i] = res.a2;
	}
	return 0;
}

static s8 exynos8890_signed_nibble(u32 value)
{
	return sign_extend32(value & 0xf, 3);
}

static void exynos8890_decode_fuse_half(u16 value,
				       struct exynos8890_fuse_domain *fuse)
{
	fuse->group = value & 0xf;
	fuse->modified_group = exynos8890_signed_nibble(value >> 4);
	fuse->ssa10 = (value >> 8) & 0x3;
	fuse->ssa11 = (value >> 10) & 0x3;
	fuse->ssa0 = (value >> 12) & 0xf;
}

static int exynos8890_validate_cpu_domain(
		struct exynos8890_ect_cpu_domain *domain, u32 min_rate,
		u32 max_rate, u32 max_voltage)
{
	u32 previous_rate = U32_MAX;
	u32 previous_voltage = U32_MAX;
	u32 enabled_max = 0, enabled_min = 0;
	unsigned int enabled = 0;
	int i, j;

	for (i = 0; i < domain->num_levels; i++) {
		struct exynos8890_ect_cpu_level *level = &domain->levels[i];

		if (level->rate_khz >= previous_rate || level->rate_khz < min_rate ||
		    level->rate_khz > max_rate ||
		    level->clock_values[0] != level->rate_khz)
			return -EINVAL;
		for (j = 1; j < domain->num_clocks; j++)
			if (level->clock_values[j] > 63)
				return -ERANGE;
		previous_rate = level->rate_khz;
		if (!level->enabled)
			continue;
		if (level->voltage_uv < 500000 || level->voltage_uv > max_voltage ||
		    level->voltage_uv > previous_voltage)
			return -ERANGE;
		previous_voltage = level->voltage_uv;
		if (!enabled_max)
			enabled_max = level->rate_khz;
		enabled_min = level->rate_khz;
		enabled++;
	}

	if (!enabled)
		return -ENODATA;
	if ((domain->boot_level < -1 || domain->boot_level >= domain->num_levels) ||
	    (domain->resume_level < -1 || domain->resume_level >= domain->num_levels))
		return -EINVAL;
	if ((domain->boot_level >= 0 &&
	     !domain->levels[domain->boot_level].enabled) ||
	    (domain->resume_level >= 0 &&
	     !domain->levels[domain->resume_level].enabled))
		return -EINVAL;
	domain->max_frequency = enabled_max;
	domain->min_frequency = enabled_min;
	return 0;
}

static int exynos8890_ect_probe(struct platform_device *pdev)
{
	struct device_node *memory_node;
	struct exynos8890_fuse_domain little_fuse, big_fuse;
	struct exynos8890_ect_data *data;
	struct ect_cursor blob;
	struct resource resource;
	void *mapping;
	u32 words[EXYNOS8890_ASV_WORDS];
	u32 total_size, table_version;
	int ret;

	memory_node = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!memory_node)
		return dev_err_probe(&pdev->dev, -EINVAL, "missing ECT memory region\n");
	ret = of_address_to_resource(memory_node, 0, &resource);
	of_node_put(memory_node);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "invalid ECT memory region\n");
	if (resource_size(&resource) != ECT_MAX_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid ECT region size\n");

	mapping = devm_memremap(&pdev->dev, resource.start, resource_size(&resource),
				MEMREMAP_WB);
	if (IS_ERR(mapping))
		return dev_err_probe(&pdev->dev, PTR_ERR(mapping), "failed to map ECT\n");
	if (!mapping)
		return -ENOMEM;
	if (memcmp(mapping, ECT_SIGNATURE, 4))
		return dev_err_probe(&pdev->dev, -ENODATA, "ECT signature is absent\n");
	total_size = get_unaligned_le32(mapping + 8);
	if (total_size < 16 || total_size > resource_size(&resource))
		return dev_err_probe(&pdev->dev, -EINVAL, "invalid ECT total size\n");

	blob.base = mapping;
	blob.pos = mapping;
	blob.end = mapping + total_size;
	ret = exynos8890_read_asv_words(words);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "failed to read ASV fuses\n");
	if (!(words[3] & BIT(7)))
		return dev_err_probe(&pdev->dev, -ENODATA,
				     "device has no fused ASV group\n");
	table_version = words[3] & GENMASK(6, 0);
	exynos8890_decode_fuse_half(words[0] & 0xffff, &big_fuse);
	exynos8890_decode_fuse_half(words[0] >> 16, &little_fuse);

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	ret = ect_parse_dvfs(&blob, "dvfs_little", &data->little);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to parse little CPU DVFS data\n");
	ret = ect_parse_dvfs(&blob, "dvfs_big", &data->big);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to parse big CPU DVFS data\n");
	ret = ect_apply_asv(&blob, "dvfs_little", &data->little, &little_fuse,
			    table_version, "SSA_LITTLE");
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to qualify little CPU ASV data\n");
	ret = ect_apply_asv(&blob, "dvfs_big", &data->big, &big_fuse,
			    table_version, "SSA_BIG");
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "failed to qualify big CPU ASV data\n");
	ret = exynos8890_validate_cpu_domain(&data->little, 130000, 1976000,
					     1475000);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unsafe little CPU ECT voltage ordering\n");
	ret = exynos8890_validate_cpu_domain(&data->big, 208000, 3016000,
					    1575000);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "unsafe big CPU ECT voltage ordering\n");

	mutex_lock(&exynos8890_ect_lock);
	if (exynos8890_ect_data) {
		mutex_unlock(&exynos8890_ect_lock);
		return -EBUSY;
	}
	exynos8890_ect_data = data;
	mutex_unlock(&exynos8890_ect_lock);
	platform_set_drvdata(pdev, data);

	dev_info(&pdev->dev,
		 "qualified ECT table %u for Apollo and Mongoose (size %#x, crc32 %#x)\n",
		 table_version, total_size, crc32_le(~0, mapping, total_size));
	return 0;
}

static void exynos8890_ect_remove(struct platform_device *pdev)
{
	mutex_lock(&exynos8890_ect_lock);
	if (exynos8890_ect_data == platform_get_drvdata(pdev))
		exynos8890_ect_data = NULL;
	mutex_unlock(&exynos8890_ect_lock);
}

int exynos8890_ect_get_cpu_domain(const char *name,
				  struct exynos8890_ect_cpu_domain *domain)
{
	int ret = 0;

	if (!name || !domain)
		return -EINVAL;
	mutex_lock(&exynos8890_ect_lock);
	if (!exynos8890_ect_data)
		ret = -EPROBE_DEFER;
	else if (!strcmp(name, "dvfs_little"))
		*domain = exynos8890_ect_data->little;
	else if (!strcmp(name, "dvfs_big"))
		*domain = exynos8890_ect_data->big;
	else
		ret = -ENOENT;
	mutex_unlock(&exynos8890_ect_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(exynos8890_ect_get_cpu_domain);

static const struct of_device_id exynos8890_ect_of_match[] = {
	{ .compatible = "samsung,exynos8890-ect" },
	{ }
};
MODULE_DEVICE_TABLE(of, exynos8890_ect_of_match);

static struct platform_driver exynos8890_ect_driver = {
	.probe = exynos8890_ect_probe,
	.remove = exynos8890_ect_remove,
	.driver = {
		.name = "exynos8890-ect",
		.of_match_table = exynos8890_ect_of_match,
	},
};
module_platform_driver(exynos8890_ect_driver);

MODULE_DESCRIPTION("Samsung Exynos8890 ECT and CPU ASV parser");
MODULE_LICENSE("GPL");
