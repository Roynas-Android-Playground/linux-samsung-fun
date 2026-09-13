/* SPDX-License-Identifier: GPL-2.0-only */
/* Samsung universal8890 decon_8890/panels/mdnie_lite_table_hero1.h
 * AUTO_UI (2016.10.13), for S6E3HA3/herolte, not hero2/HF4.
 * Extracted from the GPL-2.0 Samsung kernel tree; byte tables unchanged.
 * Coordinate white correction and resume transition-disable are applied
 * to a per-panel copy, never to these shared tables.
 */

#ifndef S6E3HA3_MDNIE_H
#define S6E3HA3_MDNIE_H
static const unsigned char coordinate_data_1[] = {
	0xff, 0xff, 0xff, /* dummy */
	0xff, 0xfb, 0xfb, /* Tune_1 */
	0xff, 0xfd, 0xff, /* Tune_2 */
	0xfb, 0xfa, 0xff, /* Tune_3 */
	0xff, 0xfe, 0xfc, /* Tune_4 */
	0xff, 0xff, 0xff, /* Tune_5 */
	0xfb, 0xfc, 0xff, /* Tune_6 */
	0xfd, 0xff, 0xfa, /* Tune_7 */
	0xfd, 0xff, 0xfd, /* Tune_8 */
	0xfb, 0xff, 0xff, /* Tune_9 */
};

static const unsigned char AUTO_UI_1[] = {
	0xDF,
	0x30, //linear_on ascr_skin_on strength 0 0 00000
	0x14, //ascr_trans_on ascr_trans_slope 0 0000
	0x01, //ascr_trans_interval 0000 0000
	0x67, //ascr_skin_cb
	0xa9, //ascr_skin_cr
	0x17, //ascr_dist_up
	0x29, //ascr_dist_down
	0x19, //ascr_dist_right
	0x27, //ascr_dist_left
	0x00, //ascr_div_up 20
	0x59,
	0x0b,
	0x00, //ascr_div_down
	0x31,
	0xf4,
	0x00, //ascr_div_right
	0x51,
	0xec,
	0x00, //ascr_div_left
	0x34,
	0x83,
	0xff, //ascr_skin_Rr
	0x00, //ascr_skin_Rg
	0x00, //ascr_skin_Rb
	0xff, //ascr_skin_Yr
	0xff, //ascr_skin_Yg
	0x00, //ascr_skin_Yb
	0xff, //ascr_skin_Mr
	0x00, //ascr_skin_Mg
	0xff, //ascr_skin_Mb
	0xff, //ascr_skin_Wr
	0xff, //ascr_skin_Wg
	0xff, //ascr_skin_Wb
	0x00, //ascr_Cr
	0xff, //ascr_Rr
	0xff, //ascr_Cg
	0x00, //ascr_Rg
	0xff, //ascr_Cb
	0x00, //ascr_Rb
	0xff, //ascr_Mr
	0x00, //ascr_Gr
	0x00, //ascr_Mg
	0xff, //ascr_Gg
	0xff, //ascr_Mb
	0x00, //ascr_Gb
	0xff, //ascr_Yr
	0x00, //ascr_Br
	0xff, //ascr_Yg
	0x00, //ascr_Bg
	0x00, //ascr_Yb
	0xff, //ascr_Bb
	0xff, //ascr_Wr
	0x00, //ascr_Kr
	0xff, //ascr_Wg
	0x00, //ascr_Kg
	0xff, //ascr_Wb
	0x00, //ascr_Kb
};

static const unsigned char AUTO_UI_2[] = {
	0xDE,
	0x00, //lce_on gain 0 000 0000
	0x24, //lce_color_gain 00 0000
	0x96, //lce_min_ref_offset
	0xb3, //lce_illum_gain
	0x01, //lce_ref_offset 9
	0x0e,
	0x01, //lce_ref_gain 9
	0x00,
	0x77, //lce_block_size h v 0000 0000
	0x03, //lce_dark_th 000
	0x07, //lce_reduct_slope 0000
	0x40, //lce_black cc0 cc1 color_th 0 0 0 0000
	0x00, //lce_adaptive_slce adaptive_illum adaptive_reflect 0 0 0
	0x08, //lce_large_w
	0x04, //lce_med_w
	0x04, //lce_small_w
	0x0c, //glare_on uni_mode luma_ctrl_sel chroma_ctrl_sel 0 0 0 0
	0x80, //glare_blk_avg_th 0000 0000
	0x00, //glare_luma_gain 000 0000
	0x3e, //glare_luma_gain_min 000 0000
	0x7e, //glare_luma_en 0000 0000
	0xc0, //glare_luma_ctl_start 0000 0000
	0x28, //glare_luma_gain_min_th 0000 0000
	0x18, //glare_chroma_inf 0 0000
	0xa0, //glare_chroma_min 0000 0000
	0x07, //glare_chroma_gain 0 0000
	0x65, //glare_chroma_blk_th 0000 0000
	0x00, //glare_luma_th 00 0000
	0x01, //glare_luma_step 000 0000
	0x00, //glare_luma_upper_limit 0000 0000
	0x40, //glare_uni_luma_gain 000 0000
	0xf0, //glare_blk_max_th 0000 0000
	0x89, //glare_blk_max_w 0000 0000
	0x18, //glare_blk_max_curve blk_max_sh 0 0000
	0xf0, //glare_con_ext_max_th 0000 0000
	0x89, //glare_con_ext_max_w 0000 0000
	0x38, //glare_con_ext_max_sign con_ext_max_curve con_ext_max_sh 0 0 0000
	0x00, //glare_max_cnt_th 0000
	0x00, //glare_max_cnt_th 0000 0000
	0x0c, //glare_max_cnt_w 0000 0000
	0x38, //glare_max_cnt_sign max_cnt_curve max_cnt_sh 0 0 0000
	0xba, //glare_y_avg_th 0000 0000
	0x1e, //glare_y_avg_w 0000 0000
	0x38, //glare_y_avg_sign y_avg_curve y_avg_sh 0 0 0000
	0x00, //glare_max_cnt_th_dmc 0000
	0x05, //glare_max_cnt_th_dmc 0000 0000
	0x00, //nr fa de cs gamma 0 0000
	0xff, //nr_mask_th
	0x00, //de_skin
	0x00, //de_gain 10
	0x00,
	0x02, //de_maxplus 11
	0x00,
	0x02, //de_maxminus 11
	0x00,
	0x14, //fa_edge_th
	0x00, //fa_step_p 13
	0x01,
	0x00, //fa_step_n 13
	0x01,
	0x00, //fa_max_de_gain 13
	0x70,
	0x01, //fa_pcl_ppi 14
	0xc0,
	0x67, //fa_skin_cb
	0xa9, //fa_skin_cr
	0x17, //fa_dist_up
	0x29, //fa_dist_down
	0x19, //fa_dist_right
	0x27, //fa_dist_left
	0x02, //fa_div_dist_up
	0xc8,
	0x01, //fa_div_dist_down
	0x90,
	0x02, //fa_div_dist_right
	0x8f,
	0x01, //fa_div_dist_left
	0xa4,
	0x20, //fa_px_min_weight
	0x20, //fa_fr_min_weight
	0x07, //fa_skin_zone_w
	0x07, //fa_skin_zone_h
	0x28, //fa_os_cnt_10_co
	0x3c, //fa_os_cnt_20_co
	0x01, //cs_gain 10
	0x00,
	0x00, //curve_1_b
	0x20, //curve_1_a
	0x00, //curve_2_b
	0x20, //curve_2_a
	0x00, //curve_3_b
	0x20, //curve_3_a
	0x00, //curve_4_b
	0x20, //curve_4_a
	0x00, //curve_5_b
	0x20, //curve_5_a
	0x00, //curve_6_b
	0x20, //curve_6_a
	0x00, //curve_7_b
	0x20, //curve_7_a
	0x00, //curve_8_b
	0x20, //curve_8_a
	0x00, //curve_9_b
	0x20, //curve_9_a
	0x00, //curve10_b
	0x20, //curve10_a
	0x00, //curve11_b
	0x20, //curve11_a
	0x00, //curve12_b
	0x20, //curve12_a
	0x00, //curve13_b
	0x20, //curve13_a
	0x00, //curve14_b
	0x20, //curve14_a
	0x00, //curve15_b
	0x20, //curve15_a
	0x00, //curve16_b
	0x20, //curve16_a
	0x00, //curve17_b
	0x20, //curve17_a
	0x00, //curve18_b
	0x20, //curve18_a
	0x00, //curve19_b
	0x20, //curve19_a
	0x00, //curve20_b
	0x20, //curve20_a
	0x00, //curve21_b
	0x20, //curve21_a
	0x00, //curve22_b
	0x20, //curve22_a
	0x00, //curve23_b
	0x20, //curve23_a
	0x00, //curve24_b
	0xff, //curve24_a
};

static const unsigned char AUTO_UI_3[] = {
	0xDD,
	0x01, //mdnie_en
	0x00, //mask 0000
	0x30, //ascr algo aolce 00 00 00
	0x00, //gam_scr_bypass 00
	0x00, //agm_bypass_mode cc_bypass_mode cs_bypass_mode fa_de_bypass_mode 00 00 00 00
	0x00, //fa_bypass_mode nr_bypass_mode glare_bypass_mode aolce_bypass_mode 00 00 00 00
	0x00, //ap_roi_mode ddi_roi_en 000 0
	0x00, //ap_roi_sx 0000
	0x00, //ap_roi_sx 0000 0000
	0x00, //ap_roi_sy 0000
	0x00, //ap_roi_sy 0000 0000
	0x00, //ap_roi_ex 0000
	0x00, //ap_roi_ex 0000 0000
	0x00, //ap_roi_ey 0000
	0x00, //ap_roi_ey 0000 0000
};
#endif
