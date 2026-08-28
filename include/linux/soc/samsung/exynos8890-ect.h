/* arch/arm64/mach-exynos/include/mach/apm-exynos.h
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *               http://www.samsung.com
 *
 * AP Parameter definitions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H
#define __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/fs.h>
#include <linux/debugfs.h>

#define BLOCK_HEADER		"HEADER"

#define BLOCK_AP_THERMAL	"THERMAL"
#define BLOCK_MIF_THERMAL	"MR4"
#define BLOCK_DVFS		"DVFS"
#define BLOCK_ASV		"ASV"
#define BLOCK_TIMING_PARAM	"TIMING"
#define BLOCK_RCC		"RCC"
#define BLOCK_PLL		"PLL"
#define BLOCK_MARGIN		"MARGIN"
#define BLOCK_MINLOCK		"MINLOCK"
#define BLOCK_GEN_PARAM		"GEN"
#define BLOCK_BIN		"BIN"

#define SYSFS_NODE_HEADER	"header"
#define SYSFS_NODE_AP_THERMAL	"ap_thermal"
#define SYSFS_NODE_MIF_THERMAL	"mr4"
#define SYSFS_NODE_DVFS		"dvfs_table"
#define SYSFS_NODE_ASV		"asv_table"
#define SYSFS_NODE_TIMING_PARAM	"mif_timing_parameter"
#define SYSFS_NODE_RCC		"rcc_table"
#define SYSFS_NODE_PLL		"pll_list"
#define SYSFS_NODE_MARGIN	"margin_table"
#define SYSFS_NODE_MINLOCK	"minlock_table"
#define SYSFS_NODE_GEN_PARAM	"general_parameter"
#define SYSFS_NODE_BIN		"binary"

#define PMIC_VOLTAGE_STEP	(6250)

struct ect_header
{
	char sign[4];
	char version[4];
	unsigned int total_size;
	int num_of_header;
};

struct ect_dvfs_level
{
	unsigned int level;
	int level_en;
};

struct ect_dvfs_domain
{
	char *domain_name;
	unsigned int domain_offset;

	unsigned int max_frequency;
	unsigned int min_frequency;
	int boot_level_idx;
	int resume_level_idx;
	int num_of_clock;
	int num_of_level;
	char **list_clock;
	struct ect_dvfs_level  *list_level;
	unsigned int *list_dvfs_value;
};

struct ect_dvfs_header
{
	int parser_version;
	char version[4];
	int num_of_domain;

	struct ect_dvfs_domain *domain_list;
};

struct ect_pll_frequency
{
	unsigned int frequency;
	unsigned int p;
	unsigned int m;
	unsigned int s;
	unsigned int k;
};

struct ect_pll
{
	char *pll_name;
	unsigned int pll_offset;

	unsigned int type_pll;
	int num_of_frequency;
	struct ect_pll_frequency *frequency_list;
};

struct ect_pll_header
{
	int parser_version;
	char version[4];
	int num_of_pll;

	struct ect_pll *pll_list;
};

struct ect_voltage_table
{
	int table_version;
	int boot_level_idx;
	int resume_level_idx;
	int *level_en;
	unsigned int *voltages;
	unsigned char *voltages_step;
	unsigned int volt_step;
};

struct ect_voltage_domain
{
	char *domain_name;
	unsigned int domain_offset;

	int num_of_group;
	int num_of_level;
	int num_of_table;
	unsigned int *level_list;
	struct ect_voltage_table *table_list;
};

struct ect_voltage_header
{
	int parser_version;
	char version[4];
	int num_of_domain;

	struct ect_voltage_domain *domain_list;
};

struct ect_rcc_table
{
	int table_version;

	unsigned int *rcc;
	unsigned char *rcc_compact;
};

struct ect_rcc_domain
{
	char *domain_name;
	unsigned int domain_offset;

	int num_of_group;
	int num_of_level;
	int num_of_table;
	unsigned int *level_list;
	struct ect_rcc_table *table_list;
};

struct ect_rcc_header
{
	int parser_version;
	char version[4];
	int num_of_domain;

	struct ect_rcc_domain *domain_list;
};

struct ect_mif_thermal_level
{
	int mr4_level;
	unsigned int max_frequency;
	unsigned int min_frequency;
	unsigned int refresh_rate_value;
	unsigned int polling_period;
	unsigned int sw_trip;
};

struct ect_mif_thermal_header
{
	int parser_version;
	char version[4];
	int num_of_level;

	struct ect_mif_thermal_level *level;
};

struct ect_ap_thermal_range
{
	unsigned int lower_bound_temperature;
	unsigned int upper_bound_temperature;
	unsigned int max_frequency;
	unsigned int sw_trip;
	unsigned int flag;
};

struct ect_ap_thermal_function
{
	char *function_name;
	unsigned int function_offset;

	int num_of_range;
	struct ect_ap_thermal_range *range_list;
};

struct ect_ap_thermal_header
{
	int parser_version;
	char version[4];
	int num_of_function;

	struct ect_ap_thermal_function *function_list;
};

struct ect_margin_domain
{
	char *domain_name;
	unsigned int domain_offset;

	int num_of_group;
	int num_of_level;
	unsigned int *offset;
	unsigned char *offset_compact;
	unsigned int volt_step;
};

struct ect_margin_header
{
	int parser_version;
	char version[4];
	int num_of_domain;

	struct ect_margin_domain *domain_list;
};

struct ect_timing_param_size
{
	unsigned int memory_size;
	unsigned long long parameter_key;
	unsigned int offset;

	int num_of_timing_param;
	int num_of_level;
	unsigned int *timing_parameter;
};

struct ect_timing_param_header
{
	int parser_version;
	char version[4];
	int num_of_size;

	struct ect_timing_param_size *size_list;
};

struct ect_minlock_frequency
{
	unsigned int main_frequencies;
	unsigned int sub_frequencies;
};

struct ect_minlock_domain
{
	char *domain_name;
	unsigned int domain_offset;

	int num_of_level;
	struct ect_minlock_frequency *level;
};

struct ect_minlock_header
{
	int parser_version;
	char version[4];
	int num_of_domain;

	struct ect_minlock_domain *domain_list;
};

struct ect_gen_param_table
{
	char *table_name;
	unsigned int offset;

	int num_of_col;
	int num_of_row;
	unsigned int *parameter;
};

struct ect_gen_param_header
{
	int parser_version;
	char version[4];
	int num_of_table;

	struct ect_gen_param_table *table_list;
};

struct ect_bin
{
	char *binary_name;
	unsigned int offset;

	int binary_size;
	void *ptr;
};

struct ect_bin_header
{
	int parser_version;
	char version[4];
	int num_of_binary;

	struct ect_bin *binary_list;
};

struct ect_info
{
	char *block_name;
	int block_name_length;
	int (*parser)(void *address, struct ect_info *info);
	int (*dump)(struct seq_file *s, void *data);
	struct file_operations dump_ops;
	char *dump_node_name;
	void *block_handle;
	int block_precedence;
};

#if defined(CONFIG_EXYNOS8890_ECT)
void exynos8890_ect_init(phys_addr_t address, phys_addr_t size);
int exynos8890_ect_parse_binary_header(void);
void* exynos8890_ect_get_block(char *block_name);
struct ect_dvfs_domain *exynos8890_ect_dvfs_get_domain(void *block, char *domain_name);
struct ect_pll *exynos8890_ect_pll_get_pll(void *block, char *pll_name);
struct ect_voltage_domain *exynos8890_ect_asv_get_domain(void *block, char *domain_name);
struct ect_rcc_domain *exynos8890_ect_rcc_get_domain(void *block, char *domain_name);
struct ect_mif_thermal_level *exynos8890_ect_mif_thermal_get_level(void *block, int mr4_level);
struct ect_ap_thermal_function *exynos8890_ect_ap_thermal_get_function(void *block, char *function_name);
struct ect_margin_domain *exynos8890_ect_margin_get_domain(void *block, char *domain_name);
struct ect_timing_param_size *exynos8890_ect_timing_param_get_size(void *block, int size);
struct ect_timing_param_size *exynos8890_ect_timing_param_get_key(void *block, unsigned long long key);
struct ect_minlock_domain *exynos8890_ect_minlock_get_domain(void *block, char *domain_name);
struct ect_gen_param_table *exynos8890_ect_gen_param_get_table(void *block, char *table_name);
struct ect_bin *exynos8890_ect_binary_get_bin(void *block, char *binary_name);

void exynos8890_ect_init_map_io(void);

int exynos8890_ect_strcmp(char *src1, char *src2);
int exynos8890_ect_strncmp(char *src1, char *src2, int length);

#else

static inline void exynos8890_ect_init(phys_addr_t address, phys_addr_t size) {}
static inline int exynos8890_ect_parse_binary_header(void) { return 0; }
static inline void* exynos8890_ect_get_block(char *block_name) { return NULL; }
static inline struct ect_dvfs_domain *exynos8890_ect_dvfs_get_domain(void *block, char *domain_name) { return NULL; }
static inline struct ect_pll *exynos8890_ect_pll_get_pll(void *block, char *pll_name) { return NULL; }
static inline struct ect_voltage_domain *exynos8890_ect_asv_get_domain(void *block, char *domain_name) { return NULL; }
static inline struct ect_rcc_domain *exynos8890_ect_rcc_get_domain(void *block, char *domain_name) { return NULL; }
static inline struct ect_mif_thermal_level *exynos8890_ect_mif_thermal_get_level(void *block, int mr4_level) { return NULL; }
static inline struct ect_ap_thermal_function *exynos8890_ect_ap_thermal_get_function(void *block, char *function_name) { return NULL; }
static inline struct ect_margin_domain *exynos8890_ect_margin_get_domain(void *block, char *domain_name) { return NULL; }
static inline struct ect_timing_param_size *exynos8890_ect_timing_param_get_size(void *block, int size) { return NULL; }
static inline struct ect_timing_param_size *exynos8890_ect_timing_param_get_key(void *block, unsigned long long key) { return NULL; }
static inline struct ect_minlock_domain *exynos8890_ect_minlock_get_domain(void *block, char *domain_name) { return NULL; }
static inline struct ect_gen_param_table *exynos8890_ect_gen_param_get_table(void *block, char *table_name) { return NULL; }
static inline struct ect_bin *exynos8890_ect_binary_get_bin(void *block, char *binary_name) { return NULL; }

static inline void exynos8890_ect_init_map_io(void) {}

static inline int exynos8890_ect_strcmp(char *src1, char *src2) { return -1; }
static inline int exynos8890_ect_strncmp(char *src1, char *src2, int length) { return -1; }

#endif

#endif /* __LINUX_SOC_SAMSUNG_EXYNOS8890_ECT_H */
