/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * S6E3HA3 AID dimming, adapted from Samsung's universal8890
 * decon_8890/panels/dimming_core.c and init_dimming().
 * Copyright (c) 2013 Samsung Electronics
 * Minwoo Kim <minwoo7945.kim@samsung.com>
 * Original software licensed under GNU GPL version 2.
 *
 * This is the CONFIG_PANEL_S6E3HA3_DYNAMIC V0/V3 algorithm, NOT the
 * V1/V7 dimming_core_hero algorithm. Voltage math is Q20, interpolation
 * Q10. Use wide intermediates and reject invalid voltage divisors instead
 * of allowing corrupt calibration to wrap or divide by zero.
 */
#ifndef S6E3HA3_DIMMING_H
#define S6E3HA3_DIMMING_H

#include "panel-samsung-s6e3ha3-voltage.h"
#include "panel-samsung-s6e3ha3-tables.h"

struct s6e3ha3_dimming {
	int mtp[NUM_VREF][3];
	int t_gamma[NUM_VREF][3];
	int vt_mtp[3];
	int volt_vt[3];
	int volt[256][3];
};

static int s6e3ha3_decode_mtp(struct s6e3ha3_dimming *d, const u8 *mtp)
{
	int i, c, pos = 0, offset;

	for (c = 0; c < 3; c++) {
		offset = (mtp[pos] & 1) ? -mtp[pos + 1] : mtp[pos + 1];
		d->mtp[V255][c] = offset;
		d->t_gamma[V255][c] = 256 + offset;
		pos += 2;
	}
	for (i = V203; i >= V0; i--) {
		for (c = 0; c < 3; c++) {
			offset = mtp[pos] & 0x7f;
			if (mtp[pos++] & 0x80)
				offset = -offset;
			d->mtp[i][c] = offset;
			d->t_gamma[i][c] = (i == V0 ? 0 : 128) + offset;
		}
	}
	/* HA3 OTP is RG 0B; vendor VT_RGB2GRB converts to GR 0B. */
	d->vt_mtp[0] = mtp[33] >> 4;
	d->vt_mtp[1] = mtp[33] & 0xf;
	d->vt_mtp[2] = mtp[34] & 0xf;
	return 0;
}

static int s6e3ha3_generate_voltages(struct s6e3ha3_dimming *d)
{
	static const short * const interpolation[] = {
		int_tbl_v0_v3, int_tbl_v3_v11, int_tbl_v11_v23,
		int_tbl_v23_v35, int_tbl_v35_v51, int_tbl_v51_v87,
		int_tbl_v87_v151, int_tbl_v151_v203, int_tbl_v203_v255,
	};
	int c, i, g, base, next, gamma, lo, hi;
	s64 delta;

	for (c = 0; c < 3; c++) {
		if (d->vt_mtp[c] < 0 || d->vt_mtp[c] >= ARRAY_SIZE(vt_trans_volt))
			return -ERANGE;
		d->volt_vt[c] = vt_trans_volt[d->vt_mtp[c]];
		d->volt[0][c] = DOUBLE_MULTIPLE_VREGOUT;
		gamma = clamp(d->t_gamma[V255][c], 0, 511);
		d->volt[255][c] = v255_trans_volt[gamma];
		for (i = V203; i >= V3; i--) {
			base = i == V3 ? DOUBLE_MULTIPLE_VREGOUT : d->volt_vt[c];
			next = d->volt[vref_index[i + 1]][c];
			if (base <= next)
				return -ERANGE;
			gamma = clamp(d->t_gamma[i][c], 0, 255);
			delta = (s64)(base - next) * v203_trans_volt[gamma];
			d->volt[vref_index[i]][c] = base - (delta >> 10);
		}
		for (i = V0; i < V255; i++) {
			lo = vref_index[i];
			hi = vref_index[i + 1];
			for (g = lo + 1; g < hi; g++) {
				delta = (s64)(d->volt[lo][c] - d->volt[hi][c]) *
					interpolation[i][g - lo - 1];
				d->volt[g][c] = d->volt[lo][c] - (delta >> 10);
			}
		}
	}
	return 0;
}

static int s6e3ha3_brightness_index(int brightness)
{
	int i, best = 0, gap, min_gap = MAX_BRIGHTNESS;

	if (brightness < 0 || brightness > 255)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(s6e3ha3_levels); i++) {
		gap = abs((int)br_tbl_hero1_420_da[brightness] -
			  (int)s6e3ha3_levels[i].br);
		if (gap < min_gap) {
			min_gap = gap;
			best = i;
		}
	}
	return best;
}

static int s6e3ha3_lookup_gray(int gray)
{
	int bucket, lo, hi, radius, i, best, delta, best_delta;

	if (gray < 0 || gray > (MAX_BRIGHTNESS << 20))
		return -ERANGE;
	bucket = gray >> 20;
	/* Same lookup window and lower-index tie break as the vendor core. */
	for (radius = 0; radius <= MAX_BRIGHTNESS; radius++) {
		lo = lookup_tbl[clamp(bucket - radius, 0, MAX_BRIGHTNESS)];
		hi = lookup_tbl[clamp(bucket + radius, 0, MAX_BRIGHTNESS)];
		if (hi > lo)
			break;
	}
	best = lo;
	best_delta = abs(gray - (int)(gamma_multi_tbl[lo] << 10));
	for (i = lo + 1; i <= hi; i++) {
		delta = abs(gray - (int)(gamma_multi_tbl[i] << 10));
		if (delta < best_delta) {
			best_delta = delta;
			best = i;
		}
	}
	return best;
}

static int s6e3ha3_calculate_gamma(struct s6e3ha3_dimming *d, int level, u8 *gamma)
{
	const struct s6e3ha3_level *info;
	int look[NUM_VREF][3] = { 0 }, values[NUM_VREF][3] = { 0 };
	int i, c, index, base, numerator, denominator, value, pos = 0;

	if (level < 0 || level >= ARRAY_SIZE(s6e3ha3_levels))
		return -EINVAL;
	info = &s6e3ha3_levels[level];
	for (i = V3; i <= V255; i++) {
		index = s6e3ha3_lookup_gray(info->cGma[vref_index[i]] * info->refBr);
		if (index < 0)
			return index;
		index += info->rTbl[i];
		if (index < 0 || index >= 256)
			return -ERANGE;
		for (c = 0; c < 3; c++)
			look[i][c] = d->volt[index][c];
	}
	for (i = V3; i <= V255; i++) {
		for (c = 0; c < 3; c++) {
			base = (i == V3 || i == V255) ?
				DOUBLE_MULTIPLE_VREGOUT : d->volt_vt[c];
			numerator = base - look[i][c];
			denominator = i == V255 ? DOUBLE_MULTIPLE_VREGOUT :
				base - look[i + 1][c];
			if (numerator < 0 || denominator <= 0)
				return -ERANGE;
			value = div64_u64((u64)numerator * fix_const[i].de, denominator);
			value -= fix_const[i].nu;
			value += info->cTbl[i * 3 + c] - d->mtp[i][c];
			values[i][c] = clamp(value, 0, vreg_element_max[i]);
			/* Vendor DIMMING_METHOD_FILL_CENTER at native 420 nit. */
			if (info->br == 420)
				values[i][c] = i == V255 ? 256 : 128;
		}
	}
	gamma[pos++] = OLED_CMD_GAMMA;
	for (i = V255; i >= V0; i--) {
		for (c = 0; c < 3; c++) {
			if (i == V255)
				gamma[pos++] = values[i][c] >> 8;
			gamma[pos++] = values[i][c] & 0xff;
		}
	}
	gamma[pos++] = 0;
	gamma[pos] = 0;
	return 0;
}
#endif
