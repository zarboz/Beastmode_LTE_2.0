/* Copyright (c) 2009-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/clk.h>
#include <mach/hardware.h>
#include <mach/iommu_domains.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/fb.h>
#include <linux/msm_mdp.h>
#include <linux/file.h>
#include <linux/android_pmem.h>
#include <linux/major.h>
#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/msm_kgsl.h>
#include <mach/debug_display.h>
#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"

#define VERSION_KEY_MASK	0xFFFFFF00

struct mdp4_overlay_ctrl {
	struct mdp4_overlay_pipe plist[OVERLAY_PIPE_MAX];
	struct mdp4_overlay_pipe *stage[MDP4_MIXER_MAX][MDP4_MIXER_STAGE_MAX];
	uint32 cs_controller;
	uint32 panel_3d;
	uint32 panel_mode;
	uint32 mixer0_played;
	uint32 mixer1_played;
	uint32 mixer2_played;
} mdp4_overlay_db = {
	.cs_controller = CS_CONTROLLER_0,
	.plist = {
		{
			.pipe_type = OVERLAY_TYPE_RGB,
			.pipe_num = OVERLAY_PIPE_RGB1,
			.pipe_ndx = 1,
		},
		{
			.pipe_type = OVERLAY_TYPE_RGB,
			.pipe_num = OVERLAY_PIPE_RGB2,
			.pipe_ndx = 2,
		},
		{
			.pipe_type = OVERLAY_TYPE_VIDEO,
			.pipe_num = OVERLAY_PIPE_VG1,
			.pipe_ndx = 3,
		},
		{
			.pipe_type = OVERLAY_TYPE_VIDEO,
			.pipe_num = OVERLAY_PIPE_VG2,
			.pipe_ndx = 4,
		},
		{
			.pipe_type = OVERLAY_TYPE_BF,
			.pipe_num = OVERLAY_PIPE_RGB3,
			.pipe_ndx = 5,
			.mixer_num = MDP4_MIXER0,
		},
		{
			.pipe_type = OVERLAY_TYPE_BF,
			.pipe_num = OVERLAY_PIPE_VG3,
			.pipe_ndx = 6,
			.mixer_num = MDP4_MIXER1,
		},
		{
			.pipe_type = OVERLAY_TYPE_BF,
			.pipe_num = OVERLAY_PIPE_VG4,
			.pipe_ndx = 7,
			.mixer_num = MDP4_MIXER2,
		},
	},
};

static struct mdp4_overlay_ctrl *ctrl = &mdp4_overlay_db;
static int new_perf_level;
static struct ion_client *display_iclient;
static struct mdp4_iommu_pipe_info mdp_iommu[MDP4_MIXER_MAX][OVERLAY_PIPE_RGB3];

int mdp4_overlay_iommu_map_buf(int mem_id,
	struct mdp4_overlay_pipe *pipe, unsigned int plane,
	unsigned long *start, unsigned long *len,
	struct ion_handle **srcp_ihdl)
{
	struct mdp4_iommu_pipe_info *iom_pipe_info;

	if (!display_iclient)
		return -EINVAL;

	*srcp_ihdl = ion_import_fd(display_iclient, mem_id);
	if (IS_ERR_OR_NULL(*srcp_ihdl)) {
		pr_err("ion_import_fd() failed\n");
		return PTR_ERR(*srcp_ihdl);
	}
	pr_debug("%s(): ion_hdl %p, ion_buf %p\n", __func__, *srcp_ihdl,
		ion_share(display_iclient, *srcp_ihdl));
	pr_debug("mixer %u, pipe %u, plane %u\n", pipe->mixer_num,
		pipe->pipe_ndx, plane);
	if (ion_map_iommu(display_iclient, *srcp_ihdl,
		DISPLAY_DOMAIN, GEN_POOL, SZ_4K, 0, start,
		len, 0, ION_IOMMU_UNMAP_DELAYED)) {
		ion_free(display_iclient, *srcp_ihdl);
		pr_err("ion_map_iommu() failed\n");
		return -EINVAL;
	}

	iom_pipe_info = &mdp_iommu[pipe->mixer_num][pipe->pipe_ndx - 1];
	if (!iom_pipe_info->ihdl[plane]) {
		iom_pipe_info->ihdl[plane] = *srcp_ihdl;
	} else {
		if (iom_pipe_info->prev_ihdl[plane]) {
			ion_unmap_iommu(display_iclient,
				iom_pipe_info->prev_ihdl[plane],
				DISPLAY_DOMAIN, GEN_POOL);
			ion_free(display_iclient,
				iom_pipe_info->prev_ihdl[plane]);
			pr_debug("Previous: mixer %u, pipe %u, plane %u, "
				"prev_ihdl %p\n", pipe->mixer_num,
				pipe->pipe_ndx, plane,
				iom_pipe_info->prev_ihdl[plane]);
		}

		iom_pipe_info->prev_ihdl[plane] = iom_pipe_info->ihdl[plane];
		iom_pipe_info->ihdl[plane] = *srcp_ihdl;
	}
	pr_debug("mem_id %d, start 0x%lx, len 0x%lx\n",
		mem_id, *start, *len);
	return 0;
}

void mdp4_iommu_unmap(struct mdp4_overlay_pipe *pipe)
{
	struct mdp4_iommu_pipe_info *iom_pipe_info;
	unsigned char i, j;

	if (!display_iclient)
		return;

	for (j = 0; j < OVERLAY_PIPE_RGB3; j++) {
		iom_pipe_info = &mdp_iommu[pipe->mixer_num][j];
		for (i = 0; i < MDP4_MAX_PLANE; i++) {
			if (iom_pipe_info->prev_ihdl[i]) {
				pr_debug("%s(): mixer %u, pipe %u, plane %u, "
					"prev_ihdl %p\n", __func__,
					pipe->mixer_num, j + 1, i,
					iom_pipe_info->prev_ihdl[i]);
				ion_unmap_iommu(display_iclient,
					iom_pipe_info->prev_ihdl[i],
					DISPLAY_DOMAIN, GEN_POOL);
				ion_free(display_iclient,
					iom_pipe_info->prev_ihdl[i]);
				iom_pipe_info->prev_ihdl[i] = NULL;
			}

			if (iom_pipe_info->mark_unmap) {
				if (iom_pipe_info->ihdl[i]) {
					if (pipe->mixer_num == MDP4_MIXER1)
						mdp4_overlay_dtv_wait4vsync();
					pr_debug("%s(): mixer %u, pipe %u, plane %u, "
						"ihdl %p\n", __func__,
						pipe->mixer_num, j + 1, i,
						iom_pipe_info->ihdl[i]);
					ion_unmap_iommu(display_iclient,
						iom_pipe_info->ihdl[i],
						DISPLAY_DOMAIN, GEN_POOL);
					ion_free(display_iclient,
						iom_pipe_info->ihdl[i]);
					iom_pipe_info->ihdl[i] = NULL;
				}
			}
		}
		iom_pipe_info->mark_unmap = 0;
	}
}

/* static array with index 0 for unset status and 1 for set status */
static bool overlay_status[MDP4_OVERLAY_TYPE_MAX]; 

#define NONE 0
#define SUSPEND_RESUME 0x1
#define FPS 0x2
#define BLIT_TIME 0x4
#define SHOW_UPDATES 0x8

#define DLOG(mask, fmt, args...) \
do { \
	if ((msmfb_debug_mask | FPS) & mask) \
		printk(KERN_INFO "[DISP] "": "fmt, ##args); \
} while (0)

extern int msmfb_debug_mask;


void mdp4_overlay_status_write(enum mdp4_overlay_status type, bool val)
{
	overlay_status[type] = val;
}

bool mdp4_overlay_status_read(enum mdp4_overlay_status type)
{
	return overlay_status[type];
}

int mdp4_overlay_mixer_play(int mixer_num)
{
	if (mixer_num == MDP4_MIXER2)
		return ctrl->mixer2_played;
	else if (mixer_num == MDP4_MIXER1)
		return ctrl->mixer1_played;
	else
		return ctrl->mixer0_played;
}

void mdp4_overlay_panel_3d(int mixer_num, uint32 panel_3d)
{
	ctrl->panel_3d = panel_3d;
}

void mdp4_overlay_panel_mode(int mixer_num, uint32 mode)
{
	ctrl->panel_mode |= mode;
}

void mdp4_overlay_panel_mode_unset(int mixer_num, uint32 mode)
{
	ctrl->panel_mode &= ~mode;
}

uint32 mdp4_overlay_panel_list(void)
{
	return ctrl->panel_mode;
}

void mdp4_overlay_dmae_cfg(struct msm_fb_data_type *mfd, int atv)
{
	uint32	dmae_cfg_reg;

	if (atv)
		dmae_cfg_reg = DMA_DEFLKR_EN;
	else
		dmae_cfg_reg = 0;

	if (mfd->fb_imgType == MDP_BGR_565)
		dmae_cfg_reg |= DMA_PACK_PATTERN_BGR;
	else
		dmae_cfg_reg |= DMA_PACK_PATTERN_RGB;


	if (mfd->panel_info.bpp == 18) {
		dmae_cfg_reg |= DMA_DSTC0G_6BITS |	/* 666 18BPP */
		    DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;
	} else if (mfd->panel_info.bpp == 16) {
		dmae_cfg_reg |= DMA_DSTC0G_6BITS |	/* 565 16BPP */
		    DMA_DSTC1B_5BITS | DMA_DSTC2R_5BITS;
	} else {
		dmae_cfg_reg |= DMA_DSTC0G_8BITS |	/* 888 16BPP */
		    DMA_DSTC1B_8BITS | DMA_DSTC2R_8BITS;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* dma2 config register */
	MDP_OUTP(MDP_BASE + 0xb0000, dmae_cfg_reg);
	if (atv) {
		MDP_OUTP(MDP_BASE + 0xb0070, 0xeb0010);
		MDP_OUTP(MDP_BASE + 0xb0074, 0xf00010);
		MDP_OUTP(MDP_BASE + 0xb0078, 0xf00010);
		MDP_OUTP(MDP_BASE + 0xb3000, 0x80);
		MDP_OUTP(MDP_BASE + 0xb3010, 0x1800040);
		MDP_OUTP(MDP_BASE + 0xb3014, 0x1000080);
		MDP_OUTP(MDP_BASE + 0xb4004, 0x67686970);
	} else {
		MDP_OUTP(MDP_BASE + 0xb0070, 0xff0000);
		MDP_OUTP(MDP_BASE + 0xb0074, 0xff0000);
		MDP_OUTP(MDP_BASE + 0xb0078, 0xff0000);
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

#ifdef CONFIG_FB_MSM_HDMI_3D
void unfill_black_screen(void) { return; }
#else
void unfill_black_screen(void)
{
	uint32 temp_src_format;
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	/*
	* VG2 Constant Color
	*/
	temp_src_format = inpdw(MDP_BASE + 0x30050);
	MDP_OUTP(MDP_BASE + 0x30050, temp_src_format&(~BIT(22)));
	/*
	* MDP_OVERLAY_REG_FLUSH
	*/
	MDP_OUTP(MDP_BASE + 0x18000, BIT(3));
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	return;
}
#endif

#ifdef CONFIG_FB_MSM_HDMI_3D
void fill_black_screen(void) { return; }
#else
void fill_black_screen(void)
{
	/*Black color*/
	uint32 color = 0x00000000;
	uint32 temp_src_format;
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	/*
	* VG2 Constant Color
	*/
	MDP_OUTP(MDP_BASE + 0x31008, color);
	/*
	* MDP_VG2_SRC_FORMAT
	*/
	temp_src_format = inpdw(MDP_BASE + 0x30050);
	MDP_OUTP(MDP_BASE + 0x30050, temp_src_format | BIT(22));
	/*
	* MDP_OVERLAY_REG_FLUSH
	*/
	MDP_OUTP(MDP_BASE + 0x18000, BIT(3));
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	return;
}
#endif

void mdp4_overlay_dmae_xy(struct mdp4_overlay_pipe *pipe)
{

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* dma_p source */
	MDP_OUTP(MDP_BASE + 0xb0004,
			(pipe->src_height << 16 | pipe->src_width));
	MDP_OUTP(MDP_BASE + 0xb0008, pipe->srcp0_addr);
	MDP_OUTP(MDP_BASE + 0xb000c, pipe->srcp0_ystride);

	/* dma_p dest */
	MDP_OUTP(MDP_BASE + 0xb0010, (pipe->dst_y << 16 | pipe->dst_x));

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_dmap_cfg(struct msm_fb_data_type *mfd, int lcdc)
{
	uint32	dma2_cfg_reg;

	dma2_cfg_reg = DMA_DITHER_EN;
#ifdef BLT_RGB565
	/* RGB888 is 0 */
	dma2_cfg_reg |= DMA_BUF_FORMAT_RGB565; /* blt only */
#endif

	if (mfd->fb_imgType == MDP_BGR_565)
		dma2_cfg_reg |= DMA_PACK_PATTERN_BGR;
	else
		dma2_cfg_reg |= DMA_PACK_PATTERN_RGB;


	if (mfd->panel_info.bpp == 18) {
		dma2_cfg_reg |= DMA_DSTC0G_6BITS |	/* 666 18BPP */
		    DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;
	} else if (mfd->panel_info.bpp == 16) {
		dma2_cfg_reg |= DMA_DSTC0G_6BITS |	/* 565 16BPP */
		    DMA_DSTC1B_5BITS | DMA_DSTC2R_5BITS;
	} else {
		dma2_cfg_reg |= DMA_DSTC0G_8BITS |	/* 888 16BPP */
		    DMA_DSTC1B_8BITS | DMA_DSTC2R_8BITS;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

#ifndef CONFIG_FB_MSM_LCDC_CHIMEI_WXGA_PANEL
	if (lcdc)
		dma2_cfg_reg |= DMA_PACK_ALIGN_MSB;
#endif

	/* dma2 config register */
	MDP_OUTP(MDP_BASE + 0x90000, dma2_cfg_reg);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

/*
 * mdp4_overlay_dmap_xy: called form baselayer only
 */
void mdp4_overlay_dmap_xy(struct mdp4_overlay_pipe *pipe)
{
	uint32 off, bpp;
	bool enable_mdp = false;

	if (mdp_is_in_isr == FALSE) {
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		enable_mdp = true;
	}

	/* dma_p source */
	MDP_OUTP(MDP_BASE + 0x90004,
			(pipe->src_height << 16 | pipe->src_width));
	if (pipe->blt_addr) {
#ifdef BLT_RGB565
		bpp = 2; /* overlay ouput is RGB565 */
#else
		bpp = 3; /* overlay ouput is RGB888 */
#endif
		off = 0;
		if (pipe->dmap_cnt & 0x01)
			off = pipe->src_height * pipe->src_width * bpp;
		MDP_OUTP(MDP_BASE + 0x90008, pipe->blt_addr + off);
		/* RGB888, output of overlay blending */
		MDP_OUTP(MDP_BASE + 0x9000c, pipe->src_width * bpp);
	} else {
		MDP_OUTP(MDP_BASE + 0x90008, pipe->srcp0_addr);
		MDP_OUTP(MDP_BASE + 0x9000c, pipe->srcp0_ystride);
	}

	/* dma_p dest */
	MDP_OUTP(MDP_BASE + 0x90010, (pipe->dst_y << 16 | pipe->dst_x));

	if (enable_mdp)
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

#define MDP4_VG_PHASE_STEP_DEFAULT	0x20000000
#define MDP4_VG_PHASE_STEP_SHIFT	29

static int mdp4_leading_0(uint32 num)
{
	uint32 bit = 0x80000000;
	int i;

	for (i = 0; i < 32; i++) {
		if (bit & num)
			return i;
		bit >>= 1;
	}

	return i;
}

static uint32 mdp4_scale_phase_step(int f_num, uint32 src, uint32 dst)
{
	uint32 val, s;
	int	n;

	n = mdp4_leading_0(src);
	if (n > f_num)
		n = f_num;
	s = src << n;	/* maximum to reduce lose of resolution */
	val = s / dst;
	if (n < f_num) {
		n = f_num - n;
		val <<= n;
		val |= ((s % dst) << n) / dst;
	}

	return val;
}

static void mdp4_scale_setup(struct mdp4_overlay_pipe *pipe)
{
	pipe->phasex_step = MDP4_VG_PHASE_STEP_DEFAULT;
	pipe->phasey_step = MDP4_VG_PHASE_STEP_DEFAULT;

	if (pipe->dst_h && pipe->src_h != pipe->dst_h) {
		if (pipe->dst_h > pipe->src_h * 8)	/* too much */
			return;
		pipe->op_mode |= MDP4_OP_SCALEY_EN;

		if (pipe->pipe_num >= OVERLAY_PIPE_VG1) {
			if ((pipe->flags & MDP_BACKEND_COMPOSITION) &&
				pipe->alpha_enable && pipe->dst_h > pipe->src_h)
				pipe->op_mode |= MDP4_OP_SCALEY_PIXEL_RPT;
			else if (pipe->dst_h <= (pipe->src_h / 4))
				pipe->op_mode |= MDP4_OP_SCALEY_MN_PHASE;
			else
				pipe->op_mode |= MDP4_OP_SCALEY_FIR;
			pipe->op_mode |= MDP4_OP_SCALEY_FIR;
		} else { /* RGB pipe */
			pipe->op_mode |= MDP4_OP_SCALE_RGB_ENHANCED |
				MDP4_OP_SCALE_RGB_BILINEAR |
				MDP4_OP_SCALE_ALPHA_BILINEAR;
		}

		pipe->phasey_step = mdp4_scale_phase_step(29,
					pipe->src_h, pipe->dst_h);
	}

	if (pipe->dst_w && pipe->src_w != pipe->dst_w) {
		if (pipe->dst_w > pipe->src_w * 8)	/* too much */
			return;
		pipe->op_mode |= MDP4_OP_SCALEX_EN;

		if (pipe->pipe_num >= OVERLAY_PIPE_VG1) {
			if ((pipe->flags & MDP_BACKEND_COMPOSITION) &&
				pipe->alpha_enable && pipe->dst_w > pipe->src_w)
				pipe->op_mode |= MDP4_OP_SCALEX_PIXEL_RPT;
			else if (pipe->dst_w <= (pipe->src_w / 4))
				pipe->op_mode |= MDP4_OP_SCALEX_MN_PHASE;
			else
				pipe->op_mode |= MDP4_OP_SCALEX_FIR;
			pipe->op_mode |= MDP4_OP_SCALEX_FIR;
		} else { /* RGB pipe */
			pipe->op_mode |= MDP4_OP_SCALE_RGB_ENHANCED |
				MDP4_OP_SCALE_RGB_BILINEAR |
				MDP4_OP_SCALE_ALPHA_BILINEAR;
		}

		pipe->phasex_step = mdp4_scale_phase_step(29,
					pipe->src_w, pipe->dst_w);
	}
}

void mdp4_overlay_rgb_setup(struct mdp4_overlay_pipe *pipe)
{
	char *rgb_base;
	uint32 src_size, src_xy, dst_size, dst_xy;
	uint32 format, pattern;
	uint32 offset = 0;

	rgb_base = MDP_BASE + MDP4_RGB_BASE;
	rgb_base += (MDP4_RGB_OFF * pipe->pipe_num);

	src_size = ((pipe->src_h << 16) | pipe->src_w);
	src_xy = ((pipe->src_y << 16) | pipe->src_x);
	dst_size = ((pipe->dst_h << 16) | pipe->dst_w);
	dst_xy = ((pipe->dst_y << 16) | pipe->dst_x);

	if ((pipe->src_x + pipe->src_w) > 0x7FF) {
		offset += pipe->src_x * pipe->bpp;
		src_xy &= 0xFFFF0000;
	}

	if ((pipe->src_y + pipe->src_h) > 0x7FF) {
		offset += pipe->src_y * pipe->src_width * pipe->bpp;
		src_xy &= 0x0000FFFF;
	}

	format = mdp4_overlay_format(pipe);
	pattern = mdp4_overlay_unpack_pattern(pipe);

#ifdef MDP4_IGC_LUT_ENABLE
	pipe->op_mode |= MDP4_OP_IGC_LUT_EN;
#endif

	mdp4_scale_setup(pipe);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	outpdw(rgb_base + 0x0000, src_size);	/* MDP_RGB_SRC_SIZE */
	outpdw(rgb_base + 0x0004, src_xy);	/* MDP_RGB_SRC_XY */
	outpdw(rgb_base + 0x0008, dst_size);	/* MDP_RGB_DST_SIZE */
	outpdw(rgb_base + 0x000c, dst_xy);	/* MDP_RGB_DST_XY */

	outpdw(rgb_base + 0x0010, pipe->srcp0_addr + offset);
	outpdw(rgb_base + 0x0040, pipe->srcp0_ystride);

	outpdw(rgb_base + 0x0050, format);/* MDP_RGB_SRC_FORMAT */
	outpdw(rgb_base + 0x0054, pattern);/* MDP_RGB_SRC_UNPACK_PATTERN */
	outpdw(rgb_base + 0x0058, pipe->op_mode);/* MDP_RGB_OP_MODE */
	outpdw(rgb_base + 0x005c, pipe->phasex_step);
	outpdw(rgb_base + 0x0060, pipe->phasey_step);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp4_stat.pipe[pipe->pipe_num]++;
}


static void mdp4_overlay_vg_get_src_offset(struct mdp4_overlay_pipe *pipe,
	char *vg_base, uint32 *luma_off, uint32 *chroma_off)
{
	uint32 src_xy;
	*luma_off = 0;
	*chroma_off = 0;

	if (pipe->src_x && (pipe->frame_format ==
		MDP4_FRAME_FORMAT_LINEAR)) {
		src_xy = (pipe->src_y << 16) | pipe->src_x;
		src_xy &= 0xffff0000;
		outpdw(vg_base + 0x0004, src_xy);	/* MDP_RGB_SRC_XY */

		switch (pipe->src_format) {
		case MDP_Y_CR_CB_H2V2:
		case MDP_Y_CR_CB_GH2V2:
		case MDP_Y_CB_CR_H2V2:
				*luma_off = pipe->src_x;
				*chroma_off = pipe->src_x/2;
			break;

		case MDP_Y_CBCR_H2V2_TILE:
		case MDP_Y_CRCB_H2V2_TILE:
		case MDP_Y_CBCR_H2V2:
		case MDP_Y_CRCB_H2V2:
		case MDP_Y_CRCB_H1V1:
		case MDP_Y_CBCR_H1V1:
		case MDP_Y_CRCB_H2V1:
		case MDP_Y_CBCR_H2V1:
			*luma_off = pipe->src_x;
			*chroma_off = pipe->src_x;
			break;

		case MDP_YCRYCB_H2V1:
			if (pipe->src_x & 0x1)
				pipe->src_x += 1;
			*luma_off += pipe->src_x * 2;
			break;

		case MDP_ARGB_8888:
		case MDP_RGBA_8888:
		case MDP_BGRA_8888:
		case MDP_RGBX_8888:
		case MDP_RGB_565:
		case MDP_BGR_565:
		case MDP_XRGB_8888:
		case MDP_RGB_888:
			*luma_off = pipe->src_x * pipe->bpp;
			break;

		default:
			pr_err("Source format %u not supported for x offset adjustment\n",
				pipe->src_format);
			break;
		}
	}
}

void mdp4_overlay_vg_setup(struct mdp4_overlay_pipe *pipe)
{
	char *vg_base;
	uint32 frame_size, src_size, src_xy, dst_size, dst_xy;
	uint32 format, pattern, luma_offset, chroma_offset;
	uint32 mask;
	int pnum, ptype;

	pnum = pipe->pipe_num - OVERLAY_PIPE_VG1; /* start from 0 */
	vg_base = MDP_BASE + MDP4_VIDEO_BASE;
	vg_base += (MDP4_VIDEO_OFF * pnum);

	frame_size = ((pipe->src_height << 16) | pipe->src_width);
	src_size = ((pipe->src_h << 16) | pipe->src_w);
	src_xy = ((pipe->src_y << 16) | pipe->src_x);
	dst_size = ((pipe->dst_h << 16) | pipe->dst_w);
	dst_xy = ((pipe->dst_y << 16) | pipe->dst_x);

	ptype = mdp4_overlay_format2type(pipe->src_format);
	format = mdp4_overlay_format(pipe);
	pattern = mdp4_overlay_unpack_pattern(pipe);

	/* not RGB use VG pipe, pure VG pipe */
	if (ptype != OVERLAY_TYPE_RGB)
		pipe->op_mode |= (MDP4_OP_CSC_EN | MDP4_OP_SRC_DATA_YCBCR);

#ifdef MDP4_IGC_LUT_ENABLE
	pipe->op_mode |= MDP4_OP_IGC_LUT_EN;
#endif

	mdp4_scale_setup(pipe);

	luma_offset = 0;
	chroma_offset = 0;

	if (ptype == OVERLAY_TYPE_RGB) {
		if ((pipe->src_y + pipe->src_h) > 0x7FF) {
			luma_offset = pipe->src_y * pipe->src_width * pipe->bpp;
			src_xy &= 0x0000FFFF;
		}

		if ((pipe->src_x + pipe->src_w) > 0x7FF) {
			luma_offset += pipe->src_x * pipe->bpp;
			src_xy &= 0xFFFF0000;
		}
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	outpdw(vg_base + 0x0000, src_size);	/* MDP_RGB_SRC_SIZE */
	outpdw(vg_base + 0x0004, src_xy);	/* MDP_RGB_SRC_XY */
	outpdw(vg_base + 0x0008, dst_size);	/* MDP_RGB_DST_SIZE */
	outpdw(vg_base + 0x000c, dst_xy);	/* MDP_RGB_DST_XY */

	if (pipe->frame_format != MDP4_FRAME_FORMAT_LINEAR)
		outpdw(vg_base + 0x0048, frame_size);	/* TILE frame size */

	/*
	 * Adjust src X offset to avoid MDP from overfetching pixels
	 * present before the offset. This is required for video
	 * frames coming with unused green pixels along the left margin
	 */
	/* not RGB use VG pipe, pure VG pipe */
	if (ptype != OVERLAY_TYPE_RGB) {
		mdp4_overlay_vg_get_src_offset(pipe, vg_base, &luma_offset,
			&chroma_offset);
	}

	/* luma component plane */
	outpdw(vg_base + 0x0010, pipe->srcp0_addr + luma_offset);

	/* chroma component plane or  planar color 1 */
	outpdw(vg_base + 0x0014, pipe->srcp1_addr + chroma_offset);

	/* planar color 2 */
	outpdw(vg_base + 0x0018, pipe->srcp2_addr + chroma_offset);

	outpdw(vg_base + 0x0040,
			pipe->srcp1_ystride << 16 | pipe->srcp0_ystride);

	outpdw(vg_base + 0x0044,
			pipe->srcp3_ystride << 16 | pipe->srcp2_ystride);

	outpdw(vg_base + 0x0050, format);	/* MDP_RGB_SRC_FORMAT */
	outpdw(vg_base + 0x0054, pattern);	/* MDP_RGB_SRC_UNPACK_PATTERN */
	outpdw(vg_base + 0x0058, pipe->op_mode);/* MDP_RGB_OP_MODE */
	outpdw(vg_base + 0x005c, pipe->phasex_step);
	outpdw(vg_base + 0x0060, pipe->phasey_step);

	if (pipe->op_mode & MDP4_OP_DITHER_EN) {
		outpdw(vg_base + 0x0068,
			pipe->r_bit << 4 | pipe->b_bit << 2 | pipe->g_bit);
	}

	if (pipe->flags & MDP_SHARPENING) {
		outpdw(vg_base + 0x8200,
			mdp4_ss_table_value(pipe->req_data.dpp.sharp_strength,
									0));
		outpdw(vg_base + 0x8204,
			mdp4_ss_table_value(pipe->req_data.dpp.sharp_strength,
									1));
	}

	if (mdp_rev > MDP_REV_41) {
		/* mdp chip select controller */
		mask = 0;
		if (pipe->pipe_num == OVERLAY_PIPE_VG1)
			mask = 0; /* bit 5 */
		else if (pipe->pipe_num == OVERLAY_PIPE_VG2)
			mask = 0x02000; /* bit 13 */
		if (mask) {
			if (pipe->op_mode & MDP4_OP_SCALEY_MN_PHASE)
				ctrl->cs_controller &= ~mask;
			else
				ctrl->cs_controller |= mask;
			/* NOT double buffered */
			outpdw(MDP_BASE + 0x00c0, ctrl->cs_controller);
		}
	}


	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	mdp4_stat.pipe[pipe->pipe_num]++;
}

int mdp4_overlay_format2type(uint32 format)
{
	switch (format) {
	case MDP_RGB_565:
	case MDP_RGB_888:
	case MDP_BGR_565:
	case MDP_XRGB_8888:
	case MDP_ARGB_8888:
	case MDP_RGBA_8888:
	case MDP_BGRA_8888:
	case MDP_RGBX_8888:
		return OVERLAY_TYPE_RGB;
	case MDP_YCRYCB_H2V1:
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CBCR_H2V1:
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CBCR_H2V2_TILE:
	case MDP_Y_CRCB_H2V2_TILE:
	case MDP_Y_CR_CB_H2V2:
	case MDP_Y_CR_CB_GH2V2:
	case MDP_Y_CB_CR_H2V2:
	case MDP_Y_CRCB_H1V1:
	case MDP_Y_CBCR_H1V1:
		return OVERLAY_TYPE_VIDEO;
	default:
		mdp4_stat.err_format++;
		return -ERANGE;
	}

}

#define C3_ALPHA	3	/* alpha */
#define C2_R_Cr		2	/* R/Cr */
#define C1_B_Cb		1	/* B/Cb */
#define C0_G_Y		0	/* G/luma */
#define YUV_444_MAX_WIDTH		1280	/* Max width for YUV 444*/

int mdp4_overlay_format2pipe(struct mdp4_overlay_pipe *pipe)
{
	switch (pipe->src_format) {
	case MDP_RGB_565:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 1;	/* R, 5 bits */
		pipe->b_bit = 1;	/* B, 5 bits */
		pipe->g_bit = 2;	/* G, 6 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_RGB_888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 3;	/* 3 bpp */
		break;
	case MDP_BGR_565:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 1;	/* R, 5 bits */
		pipe->b_bit = 1;	/* B, 5 bits */
		pipe->g_bit = 2;	/* G, 6 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_XRGB_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_ARGB_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_RGBA_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_RGBX_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_BGRA_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_YCRYCB_H2V1:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C0_G_Y;	/* G */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 2;		/* 2 bpp */
		pipe->chroma_sample = MDP4_CHROMA_H2V1;
		break;
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CBCR_H2V1:
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CRCB_H1V1:
	case MDP_Y_CBCR_H1V1:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_PSEUDO_PLANAR;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 1;		/* 2 */
		pipe->element3 = C0_G_Y;	/* not used */
		pipe->element2 = C0_G_Y;	/* not used */
		if (pipe->src_format == MDP_Y_CRCB_H2V1) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_H2V1;
		} else if (pipe->src_format == MDP_Y_CRCB_H1V1) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			if (pipe->src_width > YUV_444_MAX_WIDTH)
				pipe->chroma_sample = MDP4_CHROMA_H1V2;
			else
				pipe->chroma_sample = MDP4_CHROMA_RGB;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V1) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_H2V1;
		} else if (pipe->src_format == MDP_Y_CBCR_H1V1) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			if (pipe->src_width > YUV_444_MAX_WIDTH)
				pipe->chroma_sample = MDP4_CHROMA_H1V2;
			else
				pipe->chroma_sample = MDP4_CHROMA_RGB;
		} else if (pipe->src_format == MDP_Y_CRCB_H2V2) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_420;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V2) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_420;
		}
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_Y_CBCR_H2V2_TILE:
	case MDP_Y_CRCB_H2V2_TILE:
		pipe->frame_format = MDP4_FRAME_FORMAT_VIDEO_SUPERTILE;
		pipe->fetch_plane = OVERLAY_PLANE_PSEUDO_PLANAR;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 1;		/* 2 */
		pipe->element3 = C0_G_Y;	/* not used */
		pipe->element2 = C0_G_Y;	/* not used */
		if (pipe->src_format == MDP_Y_CRCB_H2V2_TILE) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_420;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V2_TILE) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_420;
		}
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_Y_CR_CB_H2V2:
	case MDP_Y_CR_CB_GH2V2:
	case MDP_Y_CB_CR_H2V2:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_PLANAR;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->chroma_sample = MDP4_CHROMA_420;
		pipe->bpp = 2;	/* 2 bpp */
		break;
	default:
		/* not likely */
		mdp4_stat.err_format++;
		return -ERANGE;
	}

	return 0;
}

/*
 * color_key_convert: output with 12 bits color key
 */
static uint32 color_key_convert(int start, int num, uint32 color)
{
	uint32 data;

	data = (color >> start) & ((1 << num) - 1);

	/* convert to 8 bits */
	if (num == 5)
		data = ((data << 3) | (data >> 2));
	else if (num == 6)
		data = ((data << 2) | (data >> 4));

	/* convert 8 bits to 12 bits */
	data = (data << 4) | (data >> 4);

	return data;
}

void transp_color_key(int format, uint32 transp,
			uint32 *c0, uint32 *c1, uint32 *c2)
{
	int b_start, g_start, r_start;
	int b_num, g_num, r_num;

	switch (format) {
	case MDP_RGB_565:
		b_start = 0;
		g_start = 5;
		r_start = 11;
		r_num = 5;
		g_num = 6;
		b_num = 5;
		break;
	case MDP_RGB_888:
	case MDP_XRGB_8888:
	case MDP_ARGB_8888:
	case MDP_BGRA_8888:
		b_start = 0;
		g_start = 8;
		r_start = 16;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_RGBA_8888:
	case MDP_RGBX_8888:
		b_start = 16;
		g_start = 8;
		r_start = 0;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_BGR_565:
		b_start = 11;
		g_start = 5;
		r_start = 0;
		r_num = 5;
		g_num = 6;
		b_num = 5;
		break;
	case MDP_Y_CB_CR_H2V2:
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CBCR_H2V1:
		b_start = 8;
		g_start = 16;
		r_start = 0;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_Y_CR_CB_H2V2:
	case MDP_Y_CR_CB_GH2V2:
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CRCB_H1V1:
	case MDP_Y_CBCR_H1V1:
		b_start = 0;
		g_start = 16;
		r_start = 8;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	default:
		b_start = 0;
		g_start = 8;
		r_start = 16;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	}

	*c0 = color_key_convert(g_start, g_num, transp);
	*c1 = color_key_convert(b_start, b_num, transp);
	*c2 = color_key_convert(r_start, r_num, transp);
}

uint32 mdp4_overlay_format(struct mdp4_overlay_pipe *pipe)
{
	uint32	format;

	format = 0;

	if (pipe->solid_fill)
		format |= MDP4_FORMAT_SOLID_FILL;

	if (pipe->unpack_align_msb)
		format |= MDP4_FORMAT_UNPACK_ALIGN_MSB;

	if (pipe->unpack_tight)
		format |= MDP4_FORMAT_UNPACK_TIGHT;

	if (pipe->alpha_enable)
		format |= MDP4_FORMAT_ALPHA_ENABLE;

	if (pipe->flags & MDP_SOURCE_ROTATED_90)
		format |= MDP4_FORMAT_90_ROTATED;
	format |= (pipe->unpack_count << 13);
	format |= ((pipe->bpp - 1) << 9);
	format |= (pipe->a_bit << 6);
	format |= (pipe->r_bit << 4);
	format |= (pipe->b_bit << 2);
	format |= pipe->g_bit;

	format |= (pipe->frame_format << 29);

	if (pipe->fetch_plane == OVERLAY_PLANE_PSEUDO_PLANAR ||
			pipe->fetch_plane == OVERLAY_PLANE_PLANAR) {
		/* video/graphic */
		format |= (pipe->fetch_plane << 19);
		format |= (pipe->chroma_site << 28);
		format |= (pipe->chroma_sample << 26);
	}

	return format;
}

uint32 mdp4_overlay_unpack_pattern(struct mdp4_overlay_pipe *pipe)
{
	return (pipe->element3 << 24) | (pipe->element2 << 16) |
			(pipe->element1 << 8) | pipe->element0;
}

/*
 * mdp4_overlayproc_cfg: only be called from base layer
 */
void mdp4_overlayproc_cfg(struct mdp4_overlay_pipe *pipe)
{
	uint32 data, intf;
	char *overlay_base;
	bool enable_mdp = false;

	intf = 0;
	if (pipe->mixer_num == MDP4_MIXER2)
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC2_BASE;
	else if (pipe->mixer_num == MDP4_MIXER1) {
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC1_BASE;/* 0x18000 */
		intf = inpdw(MDP_BASE + 0x0038); /* MDP_DISP_INTF_SEL */
		intf >>= 4;
		intf &= 0x03;
	} else
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */

	if (mdp_is_in_isr == FALSE) {
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		enable_mdp = true;
	}

	/*
	 * BLT only siupport at primary display
	 */
	if (pipe->blt_addr) {
		int off, bpp;
#ifdef BLT_RGB565
		bpp = 2;  /* overlay ouput is RGB565 */
#else
		bpp = 3;  /* overlay ouput is RGB888 */
#endif
		data = pipe->src_height;
		data <<= 16;
		data |= pipe->src_width;
		outpdw(overlay_base + 0x0008, data); /* ROI, height + width */
		if (pipe->mixer_num == MDP4_MIXER0) {
			off = 0;
			if (pipe->ov_cnt & 0x01)
				off = pipe->src_height * pipe->src_width * bpp;

			outpdw(overlay_base + 0x000c, pipe->blt_addr + off);
			/* overlay ouput is RGB888 */
			outpdw(overlay_base + 0x0010, pipe->src_width * bpp);
			outpdw(overlay_base + 0x001c, pipe->blt_addr + off);
			/* MDDI - BLT + on demand */
			outpdw(overlay_base + 0x0004, 0x08);
#ifdef BLT_RGB565
			outpdw(overlay_base + 0x0014, 0x1); /* RGB565 */
#else
			outpdw(overlay_base + 0x0014, 0x0); /* RGB888 */
#endif
		} else if (pipe->mixer_num == MDP4_MIXER2) {
			if (ctrl->panel_mode & MDP4_PANEL_WRITEBACK) {
				off = 0;
				bpp = 1;
				if (pipe->ov_cnt & 0x01)
					off = pipe->src_height *
							pipe->src_width * bpp;

				outpdw(overlay_base + 0x000c,
						pipe->blt_addr + off);
				/* overlay ouput is RGB888 */
				outpdw(overlay_base + 0x0010,
					((pipe->src_width << 16) |
					 pipe->src_width));
				outpdw(overlay_base + 0x001c,
						pipe->blt_addr + off);
				off = pipe->src_height * pipe->src_width;
				/* align chroma to 2k address */
				off = (off + 2047) & ~2047;
				/* UV plane adress */
				outpdw(overlay_base + 0x0020,
						pipe->blt_addr + off);
				/* MDDI - BLT + on demand */
				outpdw(overlay_base + 0x0004, 0x08);
				/* pseudo planar + writeback */
				outpdw(overlay_base + 0x0014, 0x012);
				/* rgb->yuv */
				outpdw(overlay_base + 0x0200, 0x05);
			}
		}
	} else {
		data = pipe->src_height;
		data <<= 16;
		data |= pipe->src_width;
		outpdw(overlay_base + 0x0008, data); /* ROI, height + width */
		outpdw(overlay_base + 0x000c, pipe->srcp0_addr);
		outpdw(overlay_base + 0x0010, pipe->srcp0_ystride);
		outpdw(overlay_base + 0x0004, 0x01); /* directout */
	}

	if (pipe->mixer_num == MDP4_MIXER1) {
		if (intf == TV_INTF) {
			outpdw(overlay_base + 0x0014, 0x02); /* yuv422 */
			/* overlay1 CSC config */
			outpdw(overlay_base + 0x0200, 0x05); /* rgb->yuv */
		}
	}

#ifdef MDP4_IGC_LUT_ENABLE
	outpdw(overlay_base + 0x0014, 0x4);	/* GC_LUT_EN, 888 */
#endif

	if (enable_mdp)
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

int mdp4_overlay_pipe_staged(int mixer)
{
	uint32 data, mask, i, off;
	int p1, p2;

	if (mixer == MDP4_MIXER2)
		off = 0x100F0;
	else
		off = 0x10100;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	data = inpdw(MDP_BASE + off);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	p1 = 0;
	p2 = 0;
	for (i = 0; i < 8; i++) {
		mask = data & 0x0f;
		if (mask) {
			if (mask <= 4)
				p1++;
			else
				p2++;
		}
		data >>= 4;
	}

	if (mixer)
		return p2;
	else
		return p1;
}

int mdp4_mixer_info(int mixer_num, struct mdp_mixer_info *info)
{

	int ndx, cnt;
	struct mdp4_overlay_pipe *pipe;

	if (mixer_num >= MDP4_MIXER_MAX)
		return -ENODEV;

	cnt = 0;
	ndx = 1; /* ndx 0 if not used */

	for ( ; ndx < MDP4_MIXER_STAGE_MAX; ndx++) {
		pipe = ctrl->stage[mixer_num][ndx];
		if (pipe == NULL)
			continue;
		info->z_order = pipe->mixer_stage - MDP4_MIXER_STAGE0;
		info->ptype = pipe->pipe_type;
		info->pnum = pipe->pipe_num;
		info->pndx = pipe->pipe_ndx;
		info->mixer_num = pipe->mixer_num;
		info++;
		cnt++;
	}
	return cnt;
}

void mdp4_mixer_stage_up(struct mdp4_overlay_pipe *pipe)
{
	uint32 data, mask, snum, stage, mixer, pnum, off;
	struct mdp4_overlay_pipe *spipe;

	spipe = mdp4_overlay_stage_pipe(pipe->mixer_num, pipe->mixer_stage);
	if ((spipe != NULL) && (spipe->pipe_num != pipe->pipe_num)) {
		mdp4_stat.err_stage++;
		return;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	stage = pipe->mixer_stage;
	mixer = pipe->mixer_num;
	pnum = pipe->pipe_num;

	if (mixer == MDP4_MIXER2)
		off = 0x100F0;
	else
		off = 0x10100;

	/* MDP_LAYERMIXER_IN_CFG, shard by both mixer 0 and 1  */
	data = inpdw(MDP_BASE + off);

	if (mixer >= MDP4_MIXER1)
		stage += 8;

	if (pipe->pipe_type == OVERLAY_TYPE_BF) {
		snum = 16 + (4 * mixer);
	} else if (pipe->pipe_num >= OVERLAY_PIPE_VG1) {/* VG1 and VG2 */
		pnum -= OVERLAY_PIPE_VG1; /* start from 0 */
		snum = 0;
		snum += (4 * pnum);
	} else {
		snum = 8;
		snum += (4 * pnum);	/* RGB1 and RGB2 */
	}

	mask = 0x0f;
	mask <<= snum;
	stage <<= snum;
	data &= ~mask;	/* clear old bits */

	data |= stage;

	outpdw(MDP_BASE + off, data); /* MDP_LAYERMIXER_IN_CFG */

	data = inpdw(MDP_BASE + off);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	ctrl->stage[pipe->mixer_num][pipe->mixer_stage] = pipe;	/* keep it */
}

void mdp4_mixer_stage_down(struct mdp4_overlay_pipe *pipe)
{
	uint32 data, mask, snum, stage, mixer, pnum, off;

	stage = pipe->mixer_stage;
	mixer = pipe->mixer_num;
	pnum = pipe->pipe_num;

	if (pipe != ctrl->stage[mixer][stage])	/* not runing */
		return;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	if (mixer == MDP4_MIXER2)
		off = 0x100F0;
	else
		off = 0x10100;

	/* MDP_LAYERMIXER_IN_CFG, shard by both mixer 0 and 1  */
	data = inpdw(MDP_BASE + off);

	if (mixer >= MDP4_MIXER1)
		stage += 8;

	if (pipe->pipe_type == OVERLAY_TYPE_BF) {
		snum = 16 + (4 * mixer);
	} else if (pipe->pipe_num >= OVERLAY_PIPE_VG1) {
		pnum -= OVERLAY_PIPE_VG1; /* start from 0 */
		snum = 0;
		snum += (4 * pnum);
	} else {
		snum = 8;
		snum += (4 * pnum);	/* RGB1 and RGB2 */
	}

	mask = 0x0f;
	mask <<= snum;
	data &= ~mask;	/* clear old bits */

	outpdw(MDP_BASE + off, data); /* MDP_LAYERMIXER_IN_CFG */

	data = inpdw(MDP_BASE + off);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	ctrl->stage[pipe->mixer_num][pipe->mixer_stage] = NULL;	/* clear it */
}

static int mdp4_overlay_update_staging(void)
{
	struct mdp4_overlay_pipe *staging[MDP4_MIXER_MAX][MDP4_MIXER_STAGE_MAX];
	struct mdp4_overlay_pipe *pipe;
	unsigned int data, snum, pnum, mixer, stage;
	int i;

	memset(&staging, 0, sizeof(staging));

	for (i = 0; i < OVERLAY_PIPE_MAX; i++) {
		pipe = &ctrl->plist[i];
		if (!pipe->pipe_used)
			continue;

		if (pipe->req_data.id) /* user allocated */
			stage = pipe->req_data.z_order + MDP4_MIXER_STAGE0;
		else
			stage = pipe->mixer_stage;
		if (staging[pipe->mixer_num][stage])
			return -EINVAL;
		staging[pipe->mixer_num][stage] = pipe;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	data = inpdw(MDP_BASE + 0x10100); /* MDP_LAYERMIXER_IN_CFG */

	for (mixer = 0; mixer < MDP4_MIXER_MAX; mixer++) {
		for (stage = 0; stage < MDP4_MIXER_STAGE_MAX; stage++) {
			pipe = staging[mixer][stage];
			if (pipe) {
				ctrl->stage[mixer][stage] = pipe;
				pipe->mixer_stage = stage;
				pnum = pipe->pipe_num;
				if (pnum >= OVERLAY_PIPE_VG1) /* VG1 and VG2 */
					snum = 4 * (pnum - OVERLAY_PIPE_VG1);
				else /* RGB1 and RGB2 */
					snum = 8 + (4 * pnum);
				data &= ~(0x0F << snum);
				data |= (stage + (mixer * 8)) << snum;
			}
		}
	}

	outpdw(MDP_BASE + 0x10100, data); /* MDP_LAYERMIXER_IN_CFG */

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	return 0;
}

void mdp4_mixer_blend_setup(struct mdp4_overlay_pipe *pipe)
{
	struct mdp4_overlay_pipe *bg_pipe;
	unsigned char *overlay_base, *rgb_base;
	uint32 c0, c1, c2, blend_op, constant_color = 0, rgb_src_format;
	uint32 fg_color3_out, fg_alpha = 0, bg_alpha = 0;
	int off;

	if (pipe->mixer_stage == MDP4_MIXER_STAGE_BASE)
		return;

	/* mixer numer, /dev/fb0, /dev/fb1, /dev/fb2 */
	if (pipe->mixer_num == MDP4_MIXER2)
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC2_BASE;/* 0x88000 */
	else if (pipe->mixer_num == MDP4_MIXER1)
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC1_BASE;/* 0x18000 */
	else
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */

	/* stage 0 to stage 2 */
	off = 0x20 * (pipe->mixer_stage - MDP4_MIXER_STAGE0);

	bg_pipe = mdp4_overlay_stage_pipe(pipe->mixer_num,
					  MDP4_MIXER_STAGE_BASE);
	if (bg_pipe == NULL) {
		pr_err("%s: Error: no bg_pipe\n", __func__);
		return;
	}

	if (bg_pipe->pipe_type == OVERLAY_TYPE_BF &&
	    pipe->mixer_stage > MDP4_MIXER_STAGE0) {
		bg_pipe = mdp4_overlay_stage_pipe(pipe->mixer_num,
						  MDP4_MIXER_STAGE0);
	}

	if (pipe->alpha_enable) {
		/* alpha channel is lost on VG pipe when downscaling */
		if (pipe->pipe_type == OVERLAY_TYPE_VIDEO &&
		    (pipe->dst_w < pipe->src_w || pipe->dst_h < pipe->src_h))
			fg_alpha = 0;
		else
			fg_alpha = 1;
	}

	if (!fg_alpha && bg_pipe && bg_pipe->alpha_enable) {
		struct mdp4_overlay_pipe *tmp;
		int stage;

		bg_alpha = 1;
		/* check all bg layers are opaque to propagate bg alpha */
		stage = bg_pipe->mixer_stage + 1;
		for (; stage < pipe->mixer_stage; stage++) {
			tmp = mdp4_overlay_stage_pipe(pipe->mixer_num, stage);
			if (!tmp || tmp->alpha_enable || tmp->is_fg) {
				bg_alpha = 0;
				break;
			}
		}
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	blend_op = (MDP4_BLEND_FG_ALPHA_FG_CONST |
		    MDP4_BLEND_BG_ALPHA_BG_CONST);
	outpdw(overlay_base + off + 0x108, pipe->alpha);
	outpdw(overlay_base + off + 0x10c, 0xff - pipe->alpha);
	fg_color3_out = 1; /* keep fg alpha by default */

	if (pipe->is_fg) {
		if (pipe->alpha == 0xff &&
		    bg_pipe && bg_pipe->pipe_num <= OVERLAY_PIPE_RGB2) {
			rgb_base = MDP_BASE + MDP4_RGB_BASE;
			rgb_base += MDP4_RGB_OFF * bg_pipe->pipe_num;
			rgb_src_format = inpdw(rgb_base + 0x50);
			rgb_src_format |= MDP4_FORMAT_SOLID_FILL;
			outpdw(rgb_base + 0x50, rgb_src_format);
			outpdw(rgb_base + 0x1008, constant_color);
		}
#if 1 /* HTC_CSP_START */
	} else if (fg_alpha && pipe->mixer_num == MDP4_MIXER0) {
#else /* HTC_CSP_END */
	} else if (fg_alpha) {
#endif
		blend_op = (MDP4_BLEND_BG_ALPHA_FG_PIXEL |
			    MDP4_BLEND_BG_INV_ALPHA);
		fg_color3_out = 1; /* keep fg alpha */
	} else if (bg_alpha) {
		blend_op = (MDP4_BLEND_BG_ALPHA_BG_PIXEL |
			    MDP4_BLEND_FG_ALPHA_BG_PIXEL |
			    MDP4_BLEND_FG_INV_ALPHA);
		fg_color3_out = 0; /* keep bg alpha */
	}

	if (pipe->transp != MDP_TRANSP_NOP) {
		if (pipe->is_fg) {
			transp_color_key(pipe->src_format, pipe->transp,
					&c0, &c1, &c2);
			/* Fg blocked */
			blend_op |= MDP4_BLEND_FG_TRANSP_EN;
			/* lower limit */
			outpdw(overlay_base + off + 0x110,
					(c1 << 16 | c0));/* low */
			outpdw(overlay_base + off + 0x114, c2);/* low */
			/* upper limit */
			outpdw(overlay_base + off + 0x118,
					(c1 << 16 | c0));
			outpdw(overlay_base + off + 0x11c, c2);
		} else if (bg_pipe) {
			transp_color_key(bg_pipe->src_format,
				pipe->transp, &c0, &c1, &c2);
			/* bg blocked */
			blend_op |= MDP4_BLEND_BG_TRANSP_EN;
			/* lower limit */
			outpdw(overlay_base + 0x180,
					(c1 << 16 | c0));/* low */
			outpdw(overlay_base + 0x184, c2);/* low */
			/* upper limit */
			outpdw(overlay_base + 0x188,
					(c1 << 16 | c0));/* high */
			outpdw(overlay_base + 0x18c, c2);/* high */
		}
	}

	outpdw(overlay_base + off + 0x104, blend_op);
	outpdw(overlay_base + (off << 5) + 0x1004, fg_color3_out);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_reg_flush(struct mdp4_overlay_pipe *pipe, int all)
{
	struct mdp4_overlay_pipe *bg_pipe;
	uint32 bits = 0;

	wmb(); /* make sure registers updated */

	if (pipe->mixer_num == MDP4_MIXER1)
		bits |= 0x02;
	else {
		if (!(pipe->flags & MDP_OV_PLAY_NOWAIT))
			bits |= 0x01;
	}


	if (all && pipe->pipe_type != OVERLAY_TYPE_BF) {
		if (pipe->pipe_num <= OVERLAY_PIPE_RGB2) {
			if (pipe->pipe_num == OVERLAY_PIPE_RGB2)
				bits |= 0x20;
			else
				bits |= 0x10;
		} else {
			if (pipe->is_fg && pipe->alpha == 0xFF) {
				bg_pipe = mdp4_overlay_stage_pipe(
							pipe->mixer_num,
							MDP4_MIXER_STAGE_BASE);
				if (bg_pipe->pipe_num <= OVERLAY_PIPE_RGB2) {
					if (bg_pipe->pipe_num ==
							OVERLAY_PIPE_RGB2)
						bits |= 0x20;
					else
						bits |= 0x10;
				}
			}
			if (pipe->pipe_num == OVERLAY_PIPE_VG2)
				bits |= 0x08;
			else
				bits |= 0x04;
		}
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	outpdw(MDP_BASE + 0x18000, bits);	/* MDP_OVERLAY_REG_FLUSH */
	wmb();
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

struct mdp4_overlay_pipe *mdp4_overlay_stage_pipe(int mixer, int stage)
{
	return ctrl->stage[mixer][stage];
}

struct mdp4_overlay_pipe *mdp4_overlay_ndx2pipe(int ndx)
{
	struct mdp4_overlay_pipe *pipe;

	if (ndx <= 0 || ndx > OVERLAY_PIPE_MAX) {
		PR_DISP_INFO("%s: ndx incorrect:%d\n", __func__, ndx);
		return NULL;
	}

	pipe = &ctrl->plist[ndx - 1];	/* ndx start from 1 */

	if (pipe->pipe_used == 0) {
		PR_DISP_INFO("%s: pipe_used not used ndx=%d stage=%d mixer=%d\n", __func__, pipe->pipe_ndx, pipe->mixer_stage, pipe->mixer_num);
		return NULL;
	}

	return pipe;
}

struct mdp4_overlay_pipe *mdp4_overlay_pipe_alloc(int ptype, int mixer)
{
	int i;
	struct mdp4_overlay_pipe *pipe;

	for (i = 0; i < OVERLAY_PIPE_MAX; i++) {
		pipe = &ctrl->plist[i];
		if ((pipe->pipe_used == 0) && ((pipe->pipe_type == ptype) ||
		    (ptype == OVERLAY_TYPE_RGB &&
		     pipe->pipe_type == OVERLAY_TYPE_VIDEO))) {
			if (ptype == OVERLAY_TYPE_BF &&
			    mixer != pipe->mixer_num)
				continue;
			init_completion(&pipe->comp);
			init_completion(&pipe->dmas_comp);
			//pr_debug("%s: pipe=%x ndx=%d num=%d\n", __func__,
			//	(int)pipe, pipe->pipe_ndx, pipe->pipe_num);
			return pipe;
		}
	}

	pr_err("%s: ptype=%d FAILED\n", __func__, ptype);

	return NULL;
}


void mdp4_overlay_pipe_free(struct mdp4_overlay_pipe *pipe)
{
	uint32 ptype, num, ndx, mixer;
	struct mdp4_iommu_pipe_info *iom_pipe_info;

	pr_info("%s: pipe=%x ndx=%d stage %d mixer %d\n", __func__, (int)pipe, pipe->pipe_ndx, pipe->mixer_stage, pipe->mixer_num);

	ptype = pipe->pipe_type;
	num = pipe->pipe_num;
	ndx = pipe->pipe_ndx;
	mixer = pipe->mixer_num;
	iom_pipe_info = &mdp_iommu[pipe->mixer_num][pipe->pipe_ndx - 1];
	iom_pipe_info->mark_unmap = 1;

	memset(pipe, 0, sizeof(*pipe));

	pipe->pipe_type = ptype;
	pipe->pipe_num = num;
	pipe->pipe_ndx = ndx;
	pipe->mixer_num = mixer;
}

int mdp4_overlay_req_check(uint32 id, uint32 z_order, uint32 mixer)
{
	struct mdp4_overlay_pipe *pipe;

	pipe = ctrl->stage[mixer][z_order];

	if (pipe == NULL)
		return 0;

	if (pipe->pipe_ndx == id)	/* same req, recycle */
		return 0;

	if (id == MSMFB_NEW_REQUEST) {  /* new request */
		if (pipe->pipe_num >= OVERLAY_PIPE_VG1) /* share pipe */
			return 0;
	}

	return -EPERM;
}

static int mdp4_overlay_validate_downscale(struct mdp_overlay *req,
	struct msm_fb_data_type *mfd, uint32 perf_level, uint32 pclk_rate)
{
	__u32 panel_clk_khz, mdp_clk_khz;
	__u32 num_hsync_pix_clks, mdp_clks_per_hsync, src_wh;
	__u32 hsync_period_ps, mdp_period_ps, total_hsync_period_ps;
	unsigned long fill_rate_y_dir, fill_rate_x_dir;
	unsigned long fillratex100, mdp_pixels_produced;
	unsigned long mdp_clk_hz;

	pr_debug("%s: LCDC Mode Downscale validation with MDP Core"
		" Clk rate\n", __func__);
	pr_debug("src_w %u, src_h %u, dst_w %u, dst_h %u\n",
		req->src_rect.w, req->src_rect.h, req->dst_rect.w,
		req->dst_rect.h);


	panel_clk_khz = pclk_rate/1000;
	mdp_clk_hz = mdp_perf_level2clk_rate(perf_level);

	if (!mdp_clk_hz || !req->dst_rect.w || !req->dst_rect.h) {
		pr_debug("mdp_perf_level2clk_rate returned 0,"
			 "or dst_rect height/width is 0,"
			 "Downscale Validation incomplete\n");
		return 0;
	}

	mdp_clk_khz = mdp_clk_hz/1000;

	num_hsync_pix_clks = mfd->panel_info.lcdc.h_back_porch +
		mfd->panel_info.lcdc.h_front_porch +
		mfd->panel_info.lcdc.h_pulse_width +
		mfd->panel_info.xres;

	hsync_period_ps = 1000000000/panel_clk_khz;
	mdp_period_ps = 1000000000/mdp_clk_khz;

	total_hsync_period_ps = num_hsync_pix_clks * hsync_period_ps;
	mdp_clks_per_hsync = total_hsync_period_ps/mdp_period_ps;

	pr_debug("hsync_period_ps %u, mdp_period_ps %u,"
		"total_hsync_period_ps %u\n", hsync_period_ps,
		mdp_period_ps, total_hsync_period_ps);

	src_wh = req->src_rect.w * req->src_rect.h;
	if (src_wh % req->dst_rect.h)
		fill_rate_y_dir = (src_wh / req->dst_rect.h) + 1;
	else
		fill_rate_y_dir = (src_wh / req->dst_rect.h);

	fill_rate_x_dir = (mfd->panel_info.xres - req->dst_rect.w)
		+ req->src_rect.w;

	if (fill_rate_y_dir >= fill_rate_x_dir)
		fillratex100 = 100 * fill_rate_y_dir / mfd->panel_info.xres;
	else
		fillratex100 = 100 * fill_rate_x_dir / mfd->panel_info.xres;

	pr_debug("mdp_clks_per_hsync %u, fill_rate_y_dir %lu,"
		"fill_rate_x_dir %lu\n", mdp_clks_per_hsync,
		fill_rate_y_dir, fill_rate_x_dir);

	mdp_pixels_produced = 100 * mdp_clks_per_hsync/fillratex100;
	pr_debug("fillratex100 %lu, mdp_pixels_produced %lu\n",
		fillratex100, mdp_pixels_produced);
	if (mdp_pixels_produced <= mfd->panel_info.xres) {
		mdp4_stat.err_underflow++;
		return -ERANGE;
	}

	return 0;
}

static int mdp4_overlay_req2pipe(struct mdp_overlay *req, int mixer,
			struct mdp4_overlay_pipe **ppipe,
			struct msm_fb_data_type *mfd)
{
	struct mdp4_overlay_pipe *pipe;
	struct mdp4_iommu_pipe_info *iom_pipe_info;
	int ret, ptype;

	if (mfd == NULL) {
		pr_err("%s: mfd == NULL, -ENODEV\n", __func__);
		return -ENODEV;
	}

	if (mixer >= MDP4_MIXER_MAX) {
		pr_err("%s: mixer out of range!\n", __func__);
		mdp4_stat.err_mixer++;
		return -ERANGE;
	}

	if (req->z_order < 0 || req->z_order > 2) {
		pr_err("%s: z_order=%d out of range!\n", __func__,
				req->z_order);
		mdp4_stat.err_zorder++;
		return -ERANGE;
	}

	if (req->src_rect.h == 0 || req->src_rect.w == 0) {
		pr_err("%s: src img of zero size!\n", __func__);
		mdp4_stat.err_size++;
		return -EINVAL;
	}


	if (req->dst_rect.h > (req->src_rect.h * 8)) {	/* too much */
		mdp4_stat.err_scale++;
		pr_err("%s: scale up, too much (h)!\n", __func__);
		return -ERANGE;
	}

	if (req->src_rect.h > (req->dst_rect.h * 8)) {	/* too little */
		mdp4_stat.err_scale++;
		pr_err("%s: scale down, too little (h)!\n", __func__);
		return -ERANGE;
	}

	if (req->dst_rect.w > (req->src_rect.w * 8)) {	/* too much */
		mdp4_stat.err_scale++;
		pr_err("%s: scale up, too much (w)!\n", __func__);
		return -ERANGE;
	}

	if (req->src_rect.w > (req->dst_rect.w * 8)) {	/* too little */
		mdp4_stat.err_scale++;
		pr_err("%s: scale down, too little (w)!\n", __func__);
		return -ERANGE;
	}

	if (mdp_hw_revision == MDP4_REVISION_V1) {
		/*  non integer down saceling ratio  smaller than 1/4
		 *  is not supportted
		 */
		if (req->src_rect.h > (req->dst_rect.h * 4)) {
			if (req->src_rect.h % req->dst_rect.h) {
				mdp4_stat.err_scale++;
				pr_err("%s: need integer (h)!\n", __func__);
				return -ERANGE;
			}
		}

		if (req->src_rect.w > (req->dst_rect.w * 4)) {
			if (req->src_rect.w % req->dst_rect.w) {
				mdp4_stat.err_scale++;
				pr_err("%s: need integer (w)!\n", __func__);
				return -ERANGE;
			}
		}
	}

	if (((req->src_rect.x + req->src_rect.w) > req->src.width) ||
		((req->src_rect.y + req->src_rect.h) > req->src.height)) {
		mdp4_stat.err_size++;
		pr_err("%s invalid src rectangle %d %d %d %d %d %d\n", __func__, req->src_rect.x, req->src_rect.w, req->src.width, req->src_rect.y, req->src_rect.h, req->src.height);
		return -ERANGE;
	}

	if (ctrl->panel_3d != MDP4_3D_SIDE_BY_SIDE) {
		int xres;
		int yres;

		xres = mfd->panel_info.xres;
		yres = mfd->panel_info.yres;

		if (((req->dst_rect.x + req->dst_rect.w) > xres) ||
			((req->dst_rect.y + req->dst_rect.h) > yres)) {
			mdp4_stat.err_size++;
			pr_err("%s invalid dst rectangle\n", __func__);
			return -ERANGE;
		}
	}

	ptype = mdp4_overlay_format2type(req->src.format);
	if (ptype < 0) {
		pr_err("%s: mdp4_overlay_format2type!\n", __func__);
		return ptype;
	}

	if (req->flags & MDP_OV_PIPE_SHARE)
		ptype = OVERLAY_TYPE_VIDEO; /* VG pipe supports both RGB+YUV */

	if (req->id == MSMFB_NEW_REQUEST)  /* new request */ {
#if 0 // remove overlay in next frame update
		if (mixer == MDP4_MIXER0)
			mdp4_overlay_update_layers(mfd, mixer);
#endif
		pipe = mdp4_overlay_pipe_alloc(ptype, mixer);
	} else {
		pipe = mdp4_overlay_ndx2pipe(req->id);
	}

	if (pipe == NULL) {
		pr_err("%s: pipe == NULL! mixer %d format %x\n", __func__, mixer, req->src.format);
		return -ENOMEM;
	}

	if (!display_iclient && !IS_ERR_OR_NULL(mfd->iclient)) {
		display_iclient = mfd->iclient;
		pr_debug("%s(): display_iclient %p\n", __func__,
			display_iclient);
	}

	iom_pipe_info = &mdp_iommu[pipe->mixer_num][pipe->pipe_ndx - 1];
	iom_pipe_info->mark_unmap = 0;

	pipe->src_format = req->src.format;
	ret = mdp4_overlay_format2pipe(pipe);

	if (ret < 0) {
		pr_err("%s: mdp4_overlay_format2pipe!\n", __func__);
		return ret;
	}

	/*
	 * base layer == 1, reserved for frame buffer
	 * zorder 0 == stage 0 == 2
	 * zorder 1 == stage 1 == 3
	 * zorder 2 == stage 2 == 4
	 */
	if (req->id == MSMFB_NEW_REQUEST) {  /* new request */
		pipe->pipe_used++;
		pipe->mixer_num = mixer;
		pipe->mixer_stage = req->z_order + MDP4_MIXER_STAGE0;
		pr_debug("%s: zorder=%d pipe ndx=%d num=%d\n", __func__,
			req->z_order, pipe->pipe_ndx, pipe->pipe_num);
		pr_info("%s: pipe=%x ndx=%d num=%d zorder=%d mixer %d format = %x\n", __func__,
				(int)pipe, pipe->pipe_ndx, pipe->pipe_num, pipe->mixer_stage, pipe->mixer_num, pipe->src_format);

	}

	pipe->src_width = req->src.width & 0x1fff;	/* source img width */
	pipe->src_height = req->src.height & 0x1fff;	/* source img height */
	pipe->src_h = req->src_rect.h & 0x07ff;
	pipe->src_w = req->src_rect.w & 0x07ff;
	pipe->src_y = req->src_rect.y & 0x07ff;
	pipe->src_x = req->src_rect.x & 0x07ff;
	pipe->dst_h = req->dst_rect.h & 0x07ff;
	pipe->dst_w = req->dst_rect.w & 0x07ff;
	pipe->dst_y = req->dst_rect.y & 0x07ff;
	pipe->dst_x = req->dst_rect.x & 0x07ff;

	pipe->op_mode = 0;

	if (req->flags & MDP_FLIP_LR)
		pipe->op_mode |= MDP4_OP_FLIP_LR;

	if (req->flags & MDP_FLIP_UD)
		pipe->op_mode |= MDP4_OP_FLIP_UD;

	if (req->flags & MDP_DITHER)
		pipe->op_mode |= MDP4_OP_DITHER_EN;

	if (req->flags & MDP_DEINTERLACE)
		pipe->op_mode |= MDP4_OP_DEINT_EN;

	if (req->flags & MDP_DEINTERLACE_ODD)
		pipe->op_mode |= MDP4_OP_DEINT_ODD_REF;

	pipe->is_fg = req->is_fg;/* control alpha and color key */

	pipe->alpha = req->alpha & 0x0ff;

	pipe->transp = req->transp_mask;

	*ppipe = pipe;

	return 0;
}

static int get_img(struct msmfb_data *img, struct fb_info *info,
	struct mdp4_overlay_pipe *pipe, unsigned int plane,
	unsigned long *start, unsigned long *len, struct file **srcp_file,
	struct ion_handle **srcp_ihdl)
{
	struct file *file;
	int put_needed, ret = 0, fb_num;
#ifdef CONFIG_ANDROID_PMEM
	unsigned long vstart;
#endif
	if (img->flags & MDP_BLIT_SRC_GEM) {
		*srcp_file = NULL;
		return kgsl_gem_obj_addr(img->memory_id, (int) img->priv,
					 start, len);
	}

	if (img->flags & MDP_MEMORY_ID_TYPE_FB) {
		file = fget_light(img->memory_id, &put_needed);
		if (file == NULL)
			return -EINVAL;

		if (MAJOR(file->f_dentry->d_inode->i_rdev) == FB_MAJOR) {
			fb_num = MINOR(file->f_dentry->d_inode->i_rdev);
			if (get_fb_phys_info(start, len, fb_num,
				DISPLAY_SUBSYSTEM_ID))
				ret = -1;
			else
				*srcp_file = file;
		} else
			ret = -1;
		if (ret)
			fput_light(file, put_needed);
		return ret;
	}

#ifdef CONFIG_MSM_MULTIMEDIA_USE_ION
	return mdp4_overlay_iommu_map_buf(img->memory_id, pipe, plane,
		start, len, srcp_ihdl);
#endif
#ifdef CONFIG_ANDROID_PMEM
	if (!get_pmem_file(img->memory_id, start, &vstart,
					    len, srcp_file))
		return 0;
	else
		return -EINVAL;
#endif
}

#ifdef CONFIG_FB_MSM_MIPI_DSI
int mdp4_overlay_3d_sbys(struct fb_info *info, struct msmfb_overlay_3d *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	int ret = -EPERM;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
		mdp4_dsi_cmd_3d_sbys(mfd, req);
		ret = 0;
	} else if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO) {
		mdp4_dsi_video_3d_sbys(mfd, req);
		ret = 0;
	}
	mutex_unlock(&mfd->dma->ov_mutex);

	return ret;
}
#else
int mdp4_overlay_3d_sbys(struct fb_info *info, struct msmfb_overlay_3d *req)
{
	/* do nothing */
	return -EPERM;
}
#endif

int mdp4_overlay_blt(struct fb_info *info, struct msmfb_overlay_blt *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	if (mfd == NULL)
		return -ENODEV;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD)
		mdp4_dsi_overlay_blt(mfd, req);
	else if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO)
		mdp4_dsi_video_overlay_blt(mfd, req);
	else if (ctrl->panel_mode & MDP4_PANEL_LCDC)
		mdp4_lcdc_overlay_blt(mfd, req);

	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

int mdp4_overlay_blt_offset(struct fb_info *info, struct msmfb_overlay_blt *req)
{
	int ret = 0;

	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD)
		ret = mdp4_dsi_overlay_blt_offset(mfd, req);
	else if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO)
		ret = mdp4_dsi_video_overlay_blt_offset(mfd, req);
	else if (ctrl->panel_mode & MDP4_PANEL_LCDC)
		ret = mdp4_lcdc_overlay_blt_offset(mfd, req);

	mutex_unlock(&mfd->dma->ov_mutex);

	return ret;
}

int mdp4_overlay_get(struct fb_info *info, struct mdp_overlay *req)
{
	struct mdp4_overlay_pipe *pipe;

	pipe = mdp4_overlay_ndx2pipe(req->id);
	if (pipe == NULL)
		return -ENODEV;

	*req = pipe->req_data;

	if (mdp4_overlay_borderfill_supported())
		req->flags |= MDP_BORDERFILL_SUPPORTED;

	return 0;
}

#define OVERLAY_VGA_SIZE	0x04B000
#define OVERLAY_720P_TILE_SIZE  0x0E6000
#define OVERLAY_WSVGA_SIZE 0x98000 /* 1024x608, align 600 to 32bit */

#ifdef CONFIG_MSM_BUS_SCALING
#define OVERLAY_BUS_SCALE_TABLE_BASE	6
#endif

static int mdp4_overlay_is_rgb_type(int format)
{
	switch (format) {
	case MDP_RGB_565:
	case MDP_RGB_888:
	case MDP_BGR_565:
	case MDP_XRGB_8888:
	case MDP_ARGB_8888:
	case MDP_RGBA_8888:
	case MDP_BGRA_8888:
	case MDP_RGBX_8888:
		return 1;
	default:
		return 0;
	}
}

static uint32 mdp4_overlay_get_perf_level(struct mdp_overlay *req,
					  struct msm_fb_data_type *mfd)
{
	int is_fg = 0;

	if (req->is_fg && ((req->alpha & 0x0ff) == 0xff))
		is_fg = 1;

	if (mdp4_extn_disp)
		return OVERLAY_PERF_LEVEL1;

	if (req->flags & MDP_DEINTERLACE)
		return OVERLAY_PERF_LEVEL1;

	if (ctrl->plist[OVERLAY_PIPE_VG1].pipe_used &&
	    ctrl->plist[OVERLAY_PIPE_VG2].pipe_used)
		return OVERLAY_PERF_LEVEL1;

	if (mdp4_overlay_is_rgb_type(req->src.format) && is_fg &&
		((req->src.width * req->src.height) <= OVERLAY_WSVGA_SIZE))
		return OVERLAY_PERF_LEVEL4;
	else if (mdp4_overlay_is_rgb_type(req->src.format))
		return OVERLAY_PERF_LEVEL1;

	if (req->src.width*req->src.height <= OVERLAY_VGA_SIZE)
		return OVERLAY_PERF_LEVEL4;
	else if (req->src.width*req->src.height <= OVERLAY_720P_TILE_SIZE) {
		u32 max, min;
		max = (req->dst_rect.h > req->dst_rect.w) ?
			req->dst_rect.h : req->dst_rect.w;
		min = (mfd->panel_info.yres > mfd->panel_info.xres) ?
			mfd->panel_info.xres : mfd->panel_info.yres;
		if (max > min)	/* landscape mode */
			return OVERLAY_PERF_LEVEL3;
		else		/* potrait mode */
			return OVERLAY_PERF_LEVEL2;
	}
	else
		return OVERLAY_PERF_LEVEL1;
}

void mdp4_update_perf_level(u32 perf_level)
{
	new_perf_level = perf_level;
}

static int old_perf_level;

void mdp4_set_perf_level(void)
{
	int cur_perf_level;

	if (mdp4_extn_disp)
		cur_perf_level = OVERLAY_PERF_LEVEL1;
	else
		cur_perf_level = new_perf_level;

	if (old_perf_level != cur_perf_level) {
		mdp_set_core_clk(cur_perf_level);
		old_perf_level = cur_perf_level;
	}
}

static void mdp4_overlay_update_blt_mode(struct msm_fb_data_type *mfd)
{
	if (mfd->use_ov0_blt == mfd->ov0_blt_state)
		return;

	if (mfd->use_ov0_blt) {
		if (mfd->panel_info.type == LCDC_PANEL)
			mdp4_lcdc_overlay_blt_start(mfd);
		else if (mfd->panel_info.type == MIPI_VIDEO_PANEL)
			mdp4_dsi_video_blt_start(mfd);
		else if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD)
			mdp4_dsi_overlay_blt_start(mfd);
	} else {
		if (mfd->panel_info.type == LCDC_PANEL)
			mdp4_lcdc_overlay_blt_stop(mfd);
		else if (mfd->panel_info.type == MIPI_VIDEO_PANEL)
			mdp4_dsi_video_blt_stop(mfd);
		else if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD)
			mdp4_dsi_overlay_blt_stop(mfd);
	}
	mfd->ov0_blt_state = mfd->use_ov0_blt;
}

static u32 mdp4_overlay_blt_enable(struct mdp_overlay *req,
	struct msm_fb_data_type *mfd, uint32 perf_level)
{
	u32 clk_rate = mfd->panel_info.clk_rate;
	u32 pull_mode = 0, use_blt = 0;

#if 1 /* HTC_CSP_START */
	if (mfd->panel_info.type == MIPI_VIDEO_PANEL || mfd->panel_info.type == MIPI_CMD_PANEL)
#else
	if (mfd->panel_info.type == MIPI_VIDEO_PANEL)
#endif /* HTC_CSP_END */
		clk_rate = (&mfd->panel_info.mipi)->dsi_pclk_rate;

	if ((mfd->panel_info.type == LCDC_PANEL) ||
#if 1 /* HTC_CSP_START */
		(mfd->panel_info.type == MIPI_VIDEO_PANEL) || (mfd->panel_info.type == MIPI_CMD_PANEL))
#else
		(mfd->panel_info.type == MIPI_VIDEO_PANEL))
#endif /* HTC_CSP_END */
		pull_mode = 1;
	if (pull_mode && (req->src_rect.h > req->dst_rect.h ||
		req->src_rect.w > req->dst_rect.w)) {
		if (mdp4_overlay_validate_downscale(req, mfd, perf_level,
			clk_rate))
			use_blt = 1;
	}

	if (mfd->mdp_rev == MDP_REV_41) {
		/*
		* writeback (blt) mode to provide work around for
		* dsi cmd mode interface hardware bug.
		*/
		if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
			if (req->dst_rect.x != 0)
				use_blt = 1;
		}

		if (mfd->panel_info.xres > 1280)
			use_blt = 1;
	}
	return use_blt;
}

int mdp4_overlay_set(struct fb_info *info, struct mdp_overlay *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	int ret, mixer, perf_level;
	struct mdp4_overlay_pipe *pipe;
	int i;
	int mixer0_layer = 0;
	int video_layer = 0;
	struct mdp4_overlay_pipe *pipe_tmp;

	if (mfd == NULL) {
		pr_err("%s: mfd == NULL, -ENODEV\n", __func__);
		return -ENODEV;
	}

	if (!mfd->panel_power_on)	/* suspended */
		return -EPERM;

	if (req->src.format == MDP_FB_FORMAT)
		req->src.format = mfd->fb_imgType;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex)) {
		pr_err("%s: mutex_lock_interruptible, -EINTR\n", __func__);
		return -EINTR;
	}

	if ((display1_mfd) && (req->id == MSMFB_NEW_REQUEST)) {
		mdp4_dsi_cmd_dma_busy_wait(display1_mfd);
		mdp4_overlay_update_layers(display1_mfd, MDP4_MIXER0);
	}
	perf_level = mdp4_overlay_get_perf_level(req, mfd);

	mixer = mfd->panel_info.pdest;	/* DISPLAY_1 or DISPLAY_2 */

	ret = mdp4_overlay_req2pipe(req, mixer, &pipe, mfd);

	if (ret < 0) {
		mutex_unlock(&mfd->dma->ov_mutex);
		pr_err("%s: mdp4_overlay_req2pipe, ret=%d\n", __func__, ret);
		return ret;
	}

	if (mixer == MDP4_MIXER0) {
		u32 use_blt = mdp4_overlay_blt_enable(req, mfd,	perf_level);
		mfd->use_ov0_blt &= ~(1 << (pipe->pipe_ndx-1));
		mfd->use_ov0_blt |= (use_blt << (pipe->pipe_ndx-1));
	}

	/* return id back to user */
	req->id = pipe->pipe_ndx;	/* pipe_ndx start from 1 */
	pipe->req_data = *req;		/* keep original req */

	pipe->flags = req->flags;

	if (!IS_ERR_OR_NULL(mfd->iclient)) {
		pr_debug("pipe->flags 0x%x\n", pipe->flags);
		if (pipe->flags & MDP_SECURE_OVERLAY_SESSION) {
			mfd->mem_hid &= ~BIT(ION_IOMMU_HEAP_ID);
			mfd->mem_hid |= ION_SECURE;
		} else {
			mfd->mem_hid |= BIT(ION_IOMMU_HEAP_ID);
			mfd->mem_hid &= ~ION_SECURE;
		}
	}

	if (pipe->flags & MDP_SHARPENING) {
		bool test = ((pipe->req_data.dpp.sharp_strength > 0) &&
			((req->src_rect.w > req->dst_rect.w) &&
			 (req->src_rect.h > req->dst_rect.h)));
		if (test) {
			pr_debug("%s: No sharpening while downscaling.\n",
								__func__);
			pipe->flags &= ~MDP_SHARPENING;
		}
	}

	/* precompute HSIC matrices */
	if (req->flags & MDP_DPP_HSIC)
		mdp4_hsic_set(pipe, &(req->dpp));

	mdp4_stat.overlay_set[pipe->mixer_num]++;

	if (ctrl->panel_mode & MDP4_PANEL_MDDI) {
		if (mdp_hw_revision == MDP4_REVISION_V2_1 &&
			pipe->mixer_num == MDP4_MIXER0)
			mdp4_overlay_status_write(MDP4_OVERLAY_TYPE_SET, true);
	}

	if (ctrl->panel_mode & MDP4_PANEL_DTV &&
	    pipe->mixer_num == MDP4_MIXER1)
		mdp4_overlay_dtv_set(mfd, pipe);

	if (new_perf_level != perf_level) {
		u32 old_level = new_perf_level;
		mdp4_update_perf_level(perf_level);

		/* change clck base on perf level */
		if (pipe->mixer_num == MDP4_MIXER0) {
			if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO) {
				if (old_level > perf_level)
					mdp4_set_perf_level();
			} else if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
				mdp4_dsi_cmd_dma_busy_wait(mfd);
				mdp4_dsi_blt_dmap_busy_wait(mfd);
				mdp4_set_perf_level();
			} else if (ctrl->panel_mode & MDP4_PANEL_LCDC) {
				if (old_level > perf_level)
					mdp4_set_perf_level();
			} else if (ctrl->panel_mode & MDP4_PANEL_MDDI) {
				mdp4_mddi_dma_busy_wait(mfd);
				mdp4_set_perf_level();
			}
		} else {
			if (ctrl->panel_mode & MDP4_PANEL_DTV)
				mdp4_overlay_dtv_ov_done_push(mfd, pipe);
		}
	}
	mutex_unlock(&mfd->dma->ov_mutex);

#ifdef CONFIG_MSM_BUS_SCALING

	for (i = OVERLAY_PIPE_RGB2; i <= OVERLAY_PIPE_VG2; i++) {
		pipe_tmp = &ctrl->plist[i];
		if (pipe_tmp->pipe_used && pipe_tmp->mixer_num == MDP4_MIXER0) {
			mixer0_layer++;
			if (!mdp4_overlay_is_rgb_type(pipe_tmp->src_format))
				video_layer++;
		}
	}
	if (pipe->mixer_num == MDP4_MIXER0 && (mdp_bus_scale_table_num() > OVERLAY_BUS_SCALE_TABLE_BASE) && (mixer0_layer > 1 || video_layer != 1)) {
		//bypass case
		mdp_bus_scale_update_request(OVERLAY_BUS_SCALE_TABLE_BASE + mixer0_layer - 1);
	} else if (pipe->mixer_num == MDP4_MIXER0) {
		mdp_bus_scale_update_request(OVERLAY_BUS_SCALE_TABLE_BASE
						- perf_level);
	}
#endif

	return 0;
}

int mdp4_overlay_unset(struct fb_info *info, int ndx)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct mdp4_overlay_pipe *pipe;
	struct dpp_ctrl dpp;
	int i;
	boolean is_mixer0 = false;

	if (mfd == NULL) {
		PR_DISP_INFO("%s: mdp=NULL\n", __func__);
		return -ENODEV;
	}

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex)) {
		PR_DISP_INFO("%s: mutex_lock_interruptible fail\n", __func__);
		return -EINTR;
	}

	pipe = mdp4_overlay_ndx2pipe(ndx);

	if (pipe == NULL) {
		mutex_unlock(&mfd->dma->ov_mutex);
		return -ENODEV;
	}

	if (pipe->mixer_num == MDP4_MIXER2)
		ctrl->mixer2_played = 0;
	else if (pipe->mixer_num == MDP4_MIXER1)
		ctrl->mixer1_played = 0;
	else {
		/* mixer 0 */
		ctrl->mixer0_played = 0;
#ifdef CONFIG_FB_MSM_MIPI_DSI
		if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
			if (mfd->panel_power_on) {
				mdp4_dsi_blt_dmap_busy_wait(mfd);
			}
		}
#else
		if (ctrl->panel_mode & MDP4_PANEL_MDDI) {
			if (mfd->panel_power_on)
				mdp4_mddi_dma_busy_wait(mfd);
		}
#endif
	}

	if (pipe->mixer_num == MDP4_MIXER0) {
		is_mixer0 = true;
		if (mfd->use_ov0_blt) {
			mdp4_mixer_stage_down(pipe);
			mfd->use_ov0_blt &= ~(1 << (pipe->pipe_ndx-1));
			mdp4_overlay_update_blt_mode(mfd);
			if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
				if (mfd->panel_power_on) {
					mdp4_dsi_cmd_overlay_restore();
					mdp4_dsi_blt_dmap_busy_wait(mfd);
				}
			}
			if (!mfd->use_ov0_blt)
				mdp4_free_writeback_buf(mfd, MDP4_MIXER0);
		} else {
			mfd->use_ov0_blt &= ~(1 << (pipe->pipe_ndx-1));
		}
	} else {
		mdp4_mixer_stage_down(pipe);

		if (ctrl->panel_mode & MDP4_PANEL_DTV)
			mdp4_overlay_dtv_unset(mfd, pipe);
	}

	/* Reset any HSIC settings to default */
	if (pipe->flags & MDP_DPP_HSIC) {
		for (i = 0; i < NUM_HSIC_PARAM; i++)
			dpp.hsic_params[i] = 0;

		mdp4_hsic_set(pipe, &dpp);
		mdp4_hsic_update(pipe);
	}

	mdp4_stat.overlay_unset[pipe->mixer_num]++;

	mdp4_overlay_pipe_free(pipe);
#if 0
	if (is_mixer0) {
		u32 stage = MDP4_MIXER_STAGE0;
		struct mdp4_overlay_pipe *temp_pipe = NULL;
		for (stage = MDP4_MIXER_STAGE0; stage < MDP4_MIXER_STAGE_MAX; stage++) {
			temp_pipe = ctrl->stage[MDP4_MIXER0][stage];
			if (temp_pipe != NULL && temp_pipe->pipe_used) break;
		}
		if (stage == MDP4_MIXER_STAGE_MAX) {
			mdp4_overlay_update_layers(mfd, MDP4_MIXER0);
			if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
				if (mfd->panel_power_on)
					mdp4_dsi_cmd_overlay_restore();
			} else if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO) {
				if (mfd->panel_power_on)
					mdp4_overlay_dsi_video_vsync_push(mfd, 0);
			}
		}
	}
#endif
	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

int mdp4_check_dtv_pipe(void)
{
	int stage;
	for (stage = MDP4_MIXER_STAGE0 ; stage < MDP4_MIXER_STAGE_MAX ; stage++) {
		if (ctrl->stage[MDP4_MIXER1][stage] != NULL &&
			ctrl->stage[MDP4_MIXER1][stage]->pipe_num <= OVERLAY_PIPE_VG2){
			return true;
		}
	}
	return false;
}

int mdp4_overlay_update_layers(struct msm_fb_data_type *mfd, int mixer)
{
	struct mdp4_overlay_pipe *pipe;
	u32 stage, snum, pnum;
	u32 mask = 0, pipe_cnt = 0;
	boolean pipe_mixer_change = false;

	if (mixer >= MDP4_MIXER_MAX)
		return -ENODEV;

	for (stage = MDP4_MIXER_STAGE0; stage < MDP4_MIXER_STAGE_MAX; stage++) {
		pipe = ctrl->stage[mixer][stage];
		if (pipe) {
			if (!pipe->pipe_used) {
				printk("%s: unstaging pipe ndx=%d\n", __func__,
				       pipe->pipe_ndx);
				pnum = pipe->pipe_num;
				if (pnum >= OVERLAY_PIPE_VG1) /* VG1 and VG2 */
					snum = 4 * (pnum - OVERLAY_PIPE_VG1);
				else /* RGB1 and RGB2 */
				snum = 8 + (4 * pnum);
				mask |= (0xF << snum);

				ctrl->stage[mixer][stage] = NULL;
			} else {
				if (mixer != pipe->mixer_num) {
					printk("%s: unstaging pipe ndx=%d and pipe is allocated by mixer %d\n", __func__,
				       pipe->pipe_ndx, pipe->mixer_num);
					ctrl->stage[mixer][stage] = NULL;
					pipe_mixer_change = true;
				}
				pipe_cnt++;
			}
		}
	}

	if (mask) {
		u32 bits, data;
		if (mixer == MDP4_MIXER0)
			bits = 0x1;
		else
			bits = 0x2;

		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
		data = inpdw(MDP_BASE + 0x10100); /* MDP_LAYERMIXER_IN_CFG */
		data &= ~mask;
		printk("%s: mask=%08X new layermixer_cfg=%08X\n", __func__,
		       mask, data);
		outpdw(MDP_BASE + 0x10100, data);
		outpdw(MDP_BASE + 0x18000, bits); /* MDP_OVERLAY_REG_FLUSH */
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

		mdp4_overlay_update_blt_mode(mfd);

		if (pipe_cnt == 0) {
			mdp4_update_perf_level(OVERLAY_PERF_LEVEL4);
#ifdef CONFIG_MSM_BUS_SCALING
			if (mixer == MDP4_MIXER0 && mfd->panel_power_on)
				mdp_bus_scale_update_request(2);
#endif
		}
	} else if (pipe_mixer_change) {
		mdp4_overlay_update_blt_mode(mfd);
	}

	return 0;
}

struct tile_desc {
	uint32 width;  /* tile's width */
	uint32 height; /* tile's height */
	uint32 row_tile_w; /* tiles per row's width */
	uint32 row_tile_h; /* tiles per row's height */
};

void tile_samsung(struct tile_desc *tp)
{
	/*
	 * each row of samsung tile consists of two tiles in height
	 * and two tiles in width which means width should align to
	 * 64 x 2 bytes and height should align to 32 x 2 bytes.
	 * video decoder generate two tiles in width and one tile
	 * in height which ends up height align to 32 X 1 bytes.
	 */
	tp->width = 64;		/* 64 bytes */
	tp->row_tile_w = 2;	/* 2 tiles per row's width */
	tp->height = 32;	/* 32 bytes */
	tp->row_tile_h = 1;	/* 1 tiles per row's height */
}

uint32 tile_mem_size(struct mdp4_overlay_pipe *pipe, struct tile_desc *tp)
{
	uint32 tile_w, tile_h;
	uint32 row_num_w, row_num_h;


	tile_w = tp->width * tp->row_tile_w;
	tile_h = tp->height * tp->row_tile_h;

	row_num_w = (pipe->src_width + tile_w - 1) / tile_w;
	row_num_h = (pipe->src_height + tile_h - 1) / tile_h;
	return ((row_num_w * row_num_h * tile_w * tile_h) + 8191) & ~8191;
}

int mdp4_overlay_play_wait(struct fb_info *info, struct msmfb_overlay_data *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct mdp4_overlay_pipe *pipe;

	if (mfd == NULL)
		return -ENODEV;

	if (!mfd->panel_power_on) /* suspended */
		return -EPERM;

	pipe = mdp4_overlay_ndx2pipe(req->id);

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	mdp4_overlay_dtv_wait_for_ov(mfd, pipe);
	mdp4_iommu_unmap(pipe);

	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

int mdp4_overlay_play(struct fb_info *info, struct msmfb_overlay_data *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct msmfb_data *img;
	struct mdp4_overlay_pipe *pipe;
	ulong start, addr;
	ulong len = 0;
	struct file *srcp0_file = NULL;
	struct file *srcp1_file = NULL, *srcp2_file = NULL;
	struct ion_handle *srcp0_ihdl = NULL;
	struct ion_handle *srcp1_ihdl = NULL, *srcp2_ihdl = NULL;
	uint32_t overlay_version = 0;
	int ret = 0;
	int req_stage;
	static uint64_t ovp_dt;
	static int64_t ovp_count;
	static ktime_t ovp_last_sec;
	ktime_t ovp_now;
	static bool ignore_bkl_zero = false;

	if (mfd == NULL)
		return -ENODEV;

	if (!mfd->panel_power_on) /* suspended */
		return -EPERM;

	pipe = mdp4_overlay_ndx2pipe(req->id);
	if (pipe == NULL) {
		mdp4_stat.err_play++;
		return -ENODEV;
	}

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	img = &req->data;
	get_img(img, info, pipe, 0, &start, &len, &srcp0_file,
		&srcp0_ihdl);
	if (len == 0) {
		mutex_unlock(&mfd->dma->ov_mutex);
		pr_err("%s: pmem Error\n", __func__);
		ret = -1;
		goto end;
	}

	addr = start + img->offset;
	pipe->srcp0_addr = addr;
	pipe->srcp0_ystride = pipe->src_width * pipe->bpp;

	if ((req->version_key & VERSION_KEY_MASK) == 0xF9E8D700)
		overlay_version = (req->version_key & ~VERSION_KEY_MASK);

	if (pipe->fetch_plane == OVERLAY_PLANE_PSEUDO_PLANAR) {
		if (overlay_version > 0) {
			img = &req->plane1_data;
			get_img(img, info, pipe, 1, &start, &len, &srcp1_file,
				&srcp1_ihdl);
			if (len == 0) {
				mutex_unlock(&mfd->dma->ov_mutex);
				pr_err("%s: Error to get plane1\n", __func__);
				ret = -EINVAL;
				goto end;
			}
			pipe->srcp1_addr = start + img->offset;
		} else if (pipe->frame_format ==
				MDP4_FRAME_FORMAT_VIDEO_SUPERTILE) {
			struct tile_desc tile;

			tile_samsung(&tile);
			pipe->srcp1_addr = addr + tile_mem_size(pipe, &tile);
		} else {
			pipe->srcp1_addr = addr + (pipe->src_width *
						pipe->src_height);
		}
		pipe->srcp0_ystride = pipe->src_width;
		if ((pipe->src_format == MDP_Y_CRCB_H1V1) ||
			(pipe->src_format == MDP_Y_CBCR_H1V1)) {
			if (pipe->src_width > YUV_444_MAX_WIDTH)
				pipe->srcp1_ystride = pipe->src_width << 2;
			else
				pipe->srcp1_ystride = pipe->src_width << 1;
		} else
			pipe->srcp1_ystride = pipe->src_width;

	} else if (pipe->fetch_plane == OVERLAY_PLANE_PLANAR) {
		if (overlay_version > 0) {
			img = &req->plane1_data;
			get_img(img, info, pipe, 1, &start, &len, &srcp1_file,
				&srcp1_ihdl);
			if (len == 0) {
				mutex_unlock(&mfd->dma->ov_mutex);
				pr_err("%s: Error to get plane1\n", __func__);
				ret = -EINVAL;
				goto end;
			}
			pipe->srcp1_addr = start + img->offset;

			img = &req->plane2_data;
			get_img(img, info, pipe, 2, &start, &len, &srcp2_file,
				&srcp2_ihdl);
			if (len == 0) {
				mutex_unlock(&mfd->dma->ov_mutex);
				pr_err("%s: Error to get plane2\n", __func__);
				ret = -EINVAL;
				goto end;
			}
			pipe->srcp2_addr = start + img->offset;
		} else {
			if (pipe->src_format == MDP_Y_CR_CB_GH2V2) {
				addr += (ALIGN(pipe->src_width, 16) *
					pipe->src_height);
				pipe->srcp1_addr = addr;
				addr += ((ALIGN((pipe->src_width / 2), 16)) *
					(pipe->src_height / 2));
				pipe->srcp2_addr = addr;
			} else {
				addr += (pipe->src_width * pipe->src_height);
				pipe->srcp1_addr = addr;
				addr += ((pipe->src_width / 2) *
					(pipe->src_height / 2));
				pipe->srcp2_addr = addr;
			}
		}
		/* mdp planar format expects Cb in srcp1 and Cr in p2 */
		if ((pipe->src_format == MDP_Y_CR_CB_H2V2) ||
			(pipe->src_format == MDP_Y_CR_CB_GH2V2))
			swap(pipe->srcp1_addr, pipe->srcp2_addr);

		if (pipe->src_format == MDP_Y_CR_CB_GH2V2) {
			pipe->srcp0_ystride = ALIGN(pipe->src_width, 16);
			pipe->srcp1_ystride = ALIGN(pipe->src_width / 2, 16);
			pipe->srcp2_ystride = ALIGN(pipe->src_width / 2, 16);
		} else {
			pipe->srcp0_ystride = pipe->src_width;
			pipe->srcp1_ystride = pipe->src_width / 2;
			pipe->srcp2_ystride = pipe->src_width / 2;
		}
	}

	if (mfd->use_ov0_blt)
		mdp4_overlay_update_blt_mode(mfd);

	/* Change register after overlay0_done, test first in dsi_cmd interface */
	if (pipe->mixer_num == MDP4_MIXER0 && ctrl->panel_mode & MDP4_PANEL_DSI_CMD)
		mdp4_dsi_cmd_dma_busy_wait(mfd);

	if (pipe->pipe_num >= OVERLAY_PIPE_VG1)
		mdp4_overlay_vg_setup(pipe);	/* video/graphic pipe */
	else {
		if (pipe->flags & MDP_SHARPENING) {
			pr_debug(
			"%s: Sharpening/Smoothing not supported on RGB pipe\n",
								     __func__);
			pipe->flags &= ~MDP_SHARPENING;
		}
		mdp4_overlay_rgb_setup(pipe);	/* rgb pipe */
	}

	if (pipe->mixer_num == MDP4_MIXER0 && !(pipe->flags & MDP_OV_PLAY_NOWAIT))
		mdp4_overlay_update_layers(mfd, pipe->mixer_num);

	req_stage = pipe->req_data.z_order + MDP4_MIXER_STAGE0;
	if (pipe->mixer_stage != req_stage) {
		if (mdp4_overlay_update_staging()) {
			pr_warn("%s: error updating stages ndx=%d zorder=%d\n",
				__func__, pipe->pipe_ndx, req_stage);
			pipe->req_data.z_order = pipe->mixer_stage -
				MDP4_MIXER_STAGE0;
		}
	}

	mdp4_mixer_blend_setup(pipe);
	mdp4_mixer_stage_up(pipe);

	if (pipe->mixer_num == MDP4_MIXER2) {
		ctrl->mixer2_played++;
#ifdef CONFIG_FB_MSM_WRITEBACK_MSM_PANEL
		if (ctrl->panel_mode & MDP4_PANEL_WRITEBACK) {
			mdp4_writeback_dma_busy_wait(mfd);
			mdp4_writeback_kickoff_video(mfd, pipe);
		}
#endif
	} else if (pipe->mixer_num == MDP4_MIXER1) {
		ctrl->mixer1_played++;
		/* enternal interface */
		if (ctrl->panel_mode & MDP4_PANEL_DTV)
#ifdef CONFIG_FB_MSM_DTV
			mdp4_overlay_dtv_ov_done_push(mfd, pipe);
#else
			mdp4_overlay_reg_flush(pipe, 1);
#endif
		else if (ctrl->panel_mode & MDP4_PANEL_ATV)
			mdp4_overlay_reg_flush(pipe, 1);
	} else {

		/* primary interface */
		ctrl->mixer0_played++;
		if (ctrl->panel_mode & MDP4_PANEL_LCDC) {
			mdp4_overlay_reg_flush(pipe, 1);
			if (!mfd->use_ov0_blt)
				mdp4_overlay_update_blt_mode(mfd);
			mdp4_overlay_lcdc_vsync_push(mfd, pipe);
		}
#ifdef CONFIG_FB_MSM_MIPI_DSI
		else if (ctrl->panel_mode & MDP4_PANEL_DSI_VIDEO) {
			mdp4_overlay_reg_flush(pipe, 1);
			if (!mfd->use_ov0_blt)
				mdp4_overlay_update_blt_mode(mfd);
			if (pipe->flags & MDP_OV_PLAY_NOWAIT) {
				mdp4_stat.overlay_play[pipe->mixer_num]++;
				mutex_unlock(&mfd->dma->ov_mutex);
				goto end;
			}
			mdp4_overlay_dsi_video_vsync_push(mfd, pipe);
		}
#endif
		else {
			/* mddi & mipi dsi cmd mode */
			if (pipe->flags & MDP_OV_PLAY_NOWAIT) {
				mdp4_stat.overlay_play[pipe->mixer_num]++;
				mutex_unlock(&mfd->dma->ov_mutex);
				goto end;
			}
#ifdef CONFIG_FB_MSM_MIPI_DSI
			if (ctrl->panel_mode & MDP4_PANEL_DSI_CMD) {
				mdp4_dsi_cmd_dma_busy_wait(mfd);
				mdp4_dsi_cmd_kickoff_video(mfd, pipe);
			}
#else
			if (ctrl->panel_mode & MDP4_PANEL_MDDI) {
				mdp4_mddi_dma_busy_wait(mfd);
				mdp4_mddi_kickoff_video(mfd, pipe);
			}
#endif
		}
	}

	/* write out DPP HSIC registers */
	if (pipe->flags & MDP_DPP_HSIC)
		mdp4_hsic_update(pipe);
	if (!(pipe->flags & MDP_OV_PLAY_NOWAIT))
		mdp4_iommu_unmap(pipe);
	mdp4_stat.overlay_play[pipe->mixer_num]++;
	mutex_unlock(&mfd->dma->ov_mutex);

	++mfd->panel_info.frame_count;

	if (msmfb_debug_mask & FPS) {
		ovp_now = ktime_get();
		ovp_dt = ktime_to_ns(ktime_sub(ovp_now, ovp_last_sec));
		ovp_count++;
		if (ovp_dt > NSEC_PER_SEC) {
			int64_t fps = ovp_count * NSEC_PER_SEC * 100;
			ovp_count = 0;
			ovp_last_sec = ktime_get();
			do_div(fps, ovp_dt);
			DLOG(FPS, "overlay fps * 100: %llu\n", fps);
		}
	}


	if (mfd->request_display_on) {
		msm_fb_display_on(mfd);

		if (!ignore_bkl_zero) {
			PR_DISP_INFO("%s: bl_level %d ignore_bkl_zero %d\n", __func__, mfd->bl_level, ignore_bkl_zero);
			/* If userspace did not set backlight value, set a default backlight value before display on */
			if (mfd->bl_level == 0)
				mfd->bl_level = DEFAULT_BRIGHTNESS;
			ignore_bkl_zero = true;
		}

		if (mfd->bl_level != 0)
			msm_fb_set_backlight(mfd, mfd->bl_level);

		mfd->request_display_on = 0;
		PR_DISP_INFO("overlay_play: msm_fb_display_on done\n");
	}

end:
#ifdef CONFIG_ANDROID_PMEM
	if (srcp0_file)
		put_pmem_file(srcp0_file);
	if (srcp1_file)
		put_pmem_file(srcp1_file);
	if (srcp2_file)
		put_pmem_file(srcp2_file);
#endif
	return ret;
}
