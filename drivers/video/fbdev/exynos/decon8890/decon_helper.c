/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Helper file for Samsung EXYNOS DECON driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>
#include <asm/cacheflush.h>
#include <asm/page.h>

#ifdef CONFIG_FB_DSU
#include <linux/ion.h>
#endif

#include "decon.h"
#include "dsim.h"
#include "vpp.h"
#include "decon_helper.h"
#include <video/mipi_display.h>
#include <asm/ftrace.h>

int decon_clk_set_parent(struct device *dev, const char *child, const char *parent)
{
	struct clk *p;
	struct clk *c;

	p = clk_get(dev, parent);
	if (IS_ERR_OR_NULL(p)) {
		decon_err("%s: couldn't get clock : %s\n", __func__, parent);
		return -ENODEV;
	}

	c = clk_get(dev, child);
	if (IS_ERR_OR_NULL(c)) {
		decon_err("%s: couldn't get clock : %s\n", __func__, child);
		return -ENODEV;
	}

	clk_set_parent(c, p);
	clk_put(p);
	clk_put(c);

	return 0;
}

int decon_clk_set_rate(struct device *dev, struct clk *clk,
		const char *conid, unsigned long rate)
{
	if (IS_ERR_OR_NULL(clk)) {
		if (IS_ERR_OR_NULL(conid)) {
			decon_err("%s: couldn't set clock(%ld)\n", __func__, rate);
			return -ENODEV;
		}
		clk = clk_get(dev, conid);
		clk_set_rate(clk, rate);
		clk_put(clk);
	} else {
		clk_set_rate(clk, rate);
	}

	return 0;
}

unsigned long decon_clk_get_rate(struct device *dev, const char *clkid)
{
	struct clk *target;
	unsigned long rate;

	target = clk_get(dev, clkid);
	if (IS_ERR_OR_NULL(target)) {
		decon_err("%s: couldn't get clock : %s\n", __func__, clkid);
		return -ENODEV;
	}

	rate = clk_get_rate(target);
	clk_put(target);

	return rate;
}

void decon_to_psr_info(struct decon_device *decon, struct decon_mode_info *psr)
{
	psr->psr_mode = decon->pdata->psr_mode;
	psr->trig_mode = decon->pdata->trig_mode;
	psr->dsi_mode = decon->pdata->dsi_mode;
	psr->out_type = decon->pdata->out_type;
}

void decon_to_init_param(struct decon_device *decon, struct decon_param *p)
{
	struct decon_lcd *lcd_info = decon->lcd_info;
	struct v4l2_mbus_framefmt mbus_fmt;

	mbus_fmt.width = 0;
	mbus_fmt.height = 0;
	mbus_fmt.code = 0;
	mbus_fmt.field = 0;
	mbus_fmt.colorspace = 0;

	p->lcd_info = lcd_info;
	p->psr.psr_mode = decon->pdata->psr_mode;
	p->psr.trig_mode = decon->pdata->trig_mode;
	p->psr.dsi_mode = decon->pdata->dsi_mode;
	p->psr.out_type = decon->pdata->out_type;
	p->nr_windows = decon->pdata->max_win;
	p->disp_ss_regs = decon->ss_regs;
	decon_dbg("###psr_mode %d trig_mode %d dsi_mode %d out_type %d nr_windows %d LCD[%d %d]\n",
		p->psr.psr_mode, p->psr.trig_mode, p->psr.dsi_mode, p->psr.out_type, p->nr_windows,
		decon->lcd_info->xres, decon->lcd_info->yres);
}

u32 decon_get_bpp(enum decon_pixel_format fmt)
{
	switch (fmt) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
		return 32;

	case DECON_PIXEL_FORMAT_RGBA_5551:
	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_NV16:
	case DECON_PIXEL_FORMAT_NV61:
	case DECON_PIXEL_FORMAT_YVU422_3P:
		return 16;

	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
	case DECON_PIXEL_FORMAT_YUV420:
	case DECON_PIXEL_FORMAT_YVU420:
	case DECON_PIXEL_FORMAT_YUV420M:
	case DECON_PIXEL_FORMAT_YVU420M:
		return 12;

	default:
		break;
	}

	return 0;
}

int decon_get_plane_cnt(enum decon_pixel_format format)
{
	switch (format) {
	case DECON_PIXEL_FORMAT_ARGB_8888:
	case DECON_PIXEL_FORMAT_ABGR_8888:
	case DECON_PIXEL_FORMAT_RGBA_8888:
	case DECON_PIXEL_FORMAT_BGRA_8888:
	case DECON_PIXEL_FORMAT_XRGB_8888:
	case DECON_PIXEL_FORMAT_XBGR_8888:
	case DECON_PIXEL_FORMAT_RGBX_8888:
	case DECON_PIXEL_FORMAT_BGRX_8888:
	case DECON_PIXEL_FORMAT_RGBA_5551:
	case DECON_PIXEL_FORMAT_RGB_565:
	case DECON_PIXEL_FORMAT_NV12N:
	case DECON_PIXEL_FORMAT_NV12N_10B:
		return 1;

	case DECON_PIXEL_FORMAT_NV16:
	case DECON_PIXEL_FORMAT_NV61:
	case DECON_PIXEL_FORMAT_NV12:
	case DECON_PIXEL_FORMAT_NV21:
	case DECON_PIXEL_FORMAT_NV12M:
	case DECON_PIXEL_FORMAT_NV21M:
		return 2;

	case DECON_PIXEL_FORMAT_YVU422_3P:
	case DECON_PIXEL_FORMAT_YUV420:
	case DECON_PIXEL_FORMAT_YVU420:
	case DECON_PIXEL_FORMAT_YUV420M:
	case DECON_PIXEL_FORMAT_YVU420M:
		return 3;

	default:
		decon_err("invalid format(%d)\n", format);
		return 1;
	}
}

/**
* ----- APIs for DISPLAY_SUBSYSTEM_EVENT_LOG -----
*/
/* ===== STATIC APIs ===== */

#ifdef CONFIG_FB_DSU
static char logBuffer[10][1024];
static char* logLast;
static int logCnt = 0;

void decon_get_window_rect_log( char* buffer, struct decon_device *decon, struct decon_win_config_data *win_data )
{
	struct decon_win_config *win_config = win_data->config;
	char* cPos = buffer;
	int i;

	cPos += sprintf( cPos, "DSU window.%llu:", ktime_to_ms(ktime_get()) );

	cPos += sprintf( cPos, "[%d-%d,dst[%4d,%4d,%4d,%4d],rect[%4d,%4d,%4d,%4d]] ",
		DECON_WIN_UPDATE_IDX, win_config[DECON_WIN_UPDATE_IDX].enableDSU,
		win_config[DECON_WIN_UPDATE_IDX].dst.x, win_config[DECON_WIN_UPDATE_IDX].dst.y, win_config[DECON_WIN_UPDATE_IDX].dst.w, win_config[DECON_WIN_UPDATE_IDX].dst.h,
		win_config[DECON_WIN_UPDATE_IDX].left, win_config[DECON_WIN_UPDATE_IDX].top, win_config[DECON_WIN_UPDATE_IDX].right, win_config[DECON_WIN_UPDATE_IDX].bottom );

	for (i = 0; i < decon->pdata->max_win; i++) {
		if( win_config[i].dst.x || win_config[i].dst.y || win_config[i].dst.w || win_config[i].dst.h||
			win_config[i].src.x || win_config[i].src.y || win_config[i].src.w || win_config[i].src.h ) {
			cPos += sprintf( cPos, "(%d.%d-src(%4d,%4d,%4d,%4d),dst(%4d,%4d,%4d,%4d)) ", i, win_config[i].state,
				win_config[i].src.x, win_config[i].src.y, win_config[i].src.w, win_config[i].src.h,
				win_config[i].dst.x, win_config[i].dst.y, win_config[i].dst.w, win_config[i].dst.h );
		}
	}
}


void decon_store_window_rect_log( struct decon_device *decon, struct decon_win_config_data *win_data )
{
	decon_get_window_rect_log( &(logBuffer[logCnt][0]), decon, win_data);
	logLast = &(logBuffer[logCnt][0]);

	logCnt++;
	if( logCnt >= 10 ) logCnt = 0;
}

char* decon_last_window_rect_log( void )
{
	return logLast;
}

void decon_print_bufered_window_rect_log( void )
{
	int i;

	for( i = logCnt; i < 10; i++ ) pr_info( "(history) %s\n", logBuffer[i] );
	for( i = 0; i < logCnt; i++ ) pr_info( "(history) %s\n", logBuffer[i] );
}
#endif


#ifdef CONFIG_CHECK_DECON_TIME

void init_debug_buffer(struct time_buffer* debug_buf)
{
	int i = 0, j = 0;

	if(debug_buf == NULL) {
		pr_info("%s debug buf is NULL\n", __func__);
		return ;
	}
	debug_buf->qIndex = 0;
	debug_buf->overtime_count = 0;
	for(i = 0; i < UPDATE_DEBUG_BUFFER_MAX; i++) {
		for(j = 0; j < TIME_TABLE_SEQ_MAX; j++) {
			debug_buf->time_Q[i].time_table[j].tv_sec = 0;
			debug_buf->time_Q[i].time_table[j].tv_usec = 0;
		}
		debug_buf->time_Q[i].total_diff = 0;
	}
}

void set_time_to_buffer(struct time_buffer* debug_buf, enum TIME_TABLE_UPDATE_SEQ update_seq)
{
	ktime_t cur_time;

	if(debug_buf == NULL) {
		pr_info("%s debug buf is NULL\n", __func__);
		return ;
	}
	if((update_seq >= TIME_TABLE_SEQ_MAX) || (update_seq < 0)){
		pr_info("%s out of range : buffer size %d\n", __func__, update_seq);
		return ;
	}
	cur_time = ktime_get();

	if(update_seq == TIME_ENTER_UPDATE_TH)
		debug_buf->start_time = cur_time;
	if(update_seq == TIME_FINISH_UPDATE_TH)
		debug_buf->end_time = cur_time;
	if(update_seq == TIME_FINISH_VPP_SET)
		debug_buf->mid_time = cur_time;
	if(debug_buf->qIndex < 10)
		debug_buf->time_Q[debug_buf->qIndex].time_table[update_seq] = ktime_to_timeval(cur_time);
}

int check_diff_time(struct time_buffer* debug_buf)
{
	int retVal = 0, index = 0;
	ktime_t start_time, mid_time, end_time;

	if(debug_buf == NULL) {
		pr_info("%s debug buf is NULL\n", __func__);
		return retVal;
	}
	start_time = debug_buf->start_time;
	mid_time = debug_buf->mid_time;
	end_time  = debug_buf->end_time;

	index = debug_buf->qIndex;
	debug_buf->latest_end_diff = ktime_sub(end_time, start_time);
	debug_buf->latest_mid_diff = ktime_sub(mid_time, start_time);

	if((ktime_to_ms(debug_buf->latest_end_diff) > UPDATE_END_TIME_LIMIT) ||
		(ktime_to_ms(debug_buf->latest_mid_diff) > UPDATE_MID_TIME_LIMIT)){
		retVal = 1;
		debug_buf->overtime_count++;
		if(index < 10) {
			debug_buf->time_Q[index].total_diff = debug_buf->latest_end_diff;
			debug_buf->time_Q[index].mid_diff = debug_buf->latest_mid_diff;
			debug_buf->qIndex++;
		}
	}

	return retVal;
}


void show_debug_time(struct time_buffer* debug_buf, struct seq_file *s)
{
	int i = 0;
	int loop_size = 0;
	if(debug_buf == NULL) {
		pr_info("%s debug buf is NULL\n", __func__);
		return ;
	}
	loop_size = (debug_buf->overtime_count > UPDATE_DEBUG_BUFFER_MAX) ? UPDATE_DEBUG_BUFFER_MAX : debug_buf->overtime_count;
	if (s != NULL) {
		seq_printf(s, "\nDecon Update Time OverCount : %d\n", debug_buf->overtime_count);
		if(debug_buf->overtime_count) {
			seq_printf(s, "index : %-15s %-15s %-15s %-15s %-15s %-15s Total_duration  VPPSET_duration \n",
			"ENT_UP_TH", "FIN_FEN_WA", "ENT_VPP_SET",
			"FIN_VPP_SET", "FIN_UP_WA", "FIN_UP_TH");
			for(i = 0; i < loop_size; i++) {
				seq_printf(s, "%dth : [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld]        %dmsec         %dmsec\n",
					i, debug_buf->time_Q[i].time_table[TIME_ENTER_UPDATE_TH].tv_sec, debug_buf->time_Q[i].time_table[TIME_ENTER_UPDATE_TH].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_FENCE_WAIT].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_FENCE_WAIT].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_ENTER_VPP_SET].tv_sec, debug_buf->time_Q[i].time_table[TIME_ENTER_VPP_SET].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_VPP_SET].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_VPP_SET].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_WAIT].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_WAIT].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_TH].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_TH].tv_usec,
					(int)ktime_to_ms(debug_buf->time_Q[i].total_diff), (int)ktime_to_ms(debug_buf->time_Q[i].mid_diff));
			}
		}
	} else {
		decon_info("\nDecon Update Time OverCount : %d\n", debug_buf->overtime_count);
		if(debug_buf->overtime_count) {
			decon_info("index : %-15s %-15s %-15s %-15s %-15s %-15s Total_duration  VPPSET_duration \n",
			"ENT_UP_TH", "FIN_FEN_WA", "ENT_VPP_SET",
			"FIN_VPP_SET", "FIN_UP_WA", "FIN_UP_TH");
			for(i = 0; i < loop_size; i++) {
				decon_info("%dth : [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld] [%6ld.%06ld]        %dmsec         %dmsec\n",
					i, debug_buf->time_Q[i].time_table[TIME_ENTER_UPDATE_TH].tv_sec, debug_buf->time_Q[i].time_table[TIME_ENTER_UPDATE_TH].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_FENCE_WAIT].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_FENCE_WAIT].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_ENTER_VPP_SET].tv_sec, debug_buf->time_Q[i].time_table[TIME_ENTER_VPP_SET].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_VPP_SET].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_VPP_SET].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_WAIT].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_WAIT].tv_usec,
					debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_TH].tv_sec, debug_buf->time_Q[i].time_table[TIME_FINISH_UPDATE_TH].tv_usec,
					(int)ktime_to_ms(debug_buf->time_Q[i].total_diff), (int)ktime_to_ms(debug_buf->time_Q[i].mid_diff));
			}
		}
	}
	init_debug_buffer(debug_buf);
}

#endif
