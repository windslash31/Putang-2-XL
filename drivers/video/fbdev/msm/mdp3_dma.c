/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
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
#include <linux/bitops.h>
#include <linux/iopoll.h>

#include "mdp3.h"
#include "mdp3_dma.h"
#include "mdp3_hwio.h"

#define DMA_STOP_POLL_SLEEP_US 1000
#define DMA_STOP_POLL_TIMEOUT_US 16000

static void mdp3_vsync_intr_handler(int type, void *arg)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)arg;
	struct mdp3_vsync_notification vsync_client;

	pr_debug("mdp3_vsync_intr_handler\n");
	spin_lock(&dma->dma_lock);
	vsync_client = dma->vsync_client;
	if (!vsync_client.handler)
		dma->cb_type &= ~MDP3_DMA_CALLBACK_TYPE_VSYNC;
	complete(&dma->vsync_comp);
	spin_unlock(&dma->dma_lock);
	if (vsync_client.handler)
		vsync_client.handler(vsync_client.arg);
	else
		mdp3_irq_disable_nosync(type);
}

static void mdp3_dma_done_intr_handler(int type, void *arg)
{
	struct mdp3_dma *dma = (struct mdp3_dma *)arg;

	pr_debug("mdp3_dma_done_intr_handler\n");
	spin_lock(&dma->dma_lock);
	dma->busy = false;
	dma->cb_type &= ~MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
	spin_unlock(&dma->dma_lock);
	complete(&dma->dma_comp);
	mdp3_irq_disable_nosync(type);
}

void mdp3_dma_callback_enable(struct mdp3_dma *dma, int type)
{
	int irq_bit;
	unsigned long flag;

	pr_debug("mdp3_dma_callback_enable type=%d\n", type);

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (dma->cb_type & type) {
		spin_unlock_irqrestore(&dma->dma_lock, flag);
		return;
	} else {
		dma->cb_type |= type;
		spin_unlock_irqrestore(&dma->dma_lock, flag);
	}

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC)
			mdp3_irq_enable(MDP3_INTR_LCDC_START_OF_FRAME);
	} else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC) {
			irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
			irq_bit += dma->dma_sel;
			mdp3_irq_enable(irq_bit);
		}

		if (type & MDP3_DMA_CALLBACK_TYPE_DMA_DONE) {
			irq_bit = MDP3_INTR_DMA_P_DONE;
			if (dma->dma_sel == MDP3_DMA_S)
				irq_bit = MDP3_INTR_DMA_S_DONE;
			mdp3_irq_enable(irq_bit);
		}
	} else {
		pr_err("mdp3_dma_callback_enable not supported interface\n");
	}
}

void mdp3_dma_callback_disable(struct mdp3_dma *dma, int type)
{
	int irq_bit;
	unsigned long flag;

	pr_debug("mdp3_dma_callback_disable type=%d\n", type);

	spin_lock_irqsave(&dma->dma_lock, flag);
	if ((dma->cb_type & type) == 0) {
		spin_unlock_irqrestore(&dma->dma_lock, flag);
		return;
	} else {
		dma->cb_type &= ~type;
		spin_unlock_irqrestore(&dma->dma_lock, flag);
	}

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC)
			mdp3_irq_disable(MDP3_INTR_LCDC_START_OF_FRAME);
	} else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		if (type & MDP3_DMA_CALLBACK_TYPE_VSYNC) {
			irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
			irq_bit += dma->dma_sel;
			mdp3_irq_disable(irq_bit);
		}

		if (type & MDP3_DMA_CALLBACK_TYPE_DMA_DONE) {
			irq_bit = MDP3_INTR_DMA_P_DONE;
			if (dma->dma_sel == MDP3_DMA_S)
				irq_bit = MDP3_INTR_DMA_S_DONE;
			mdp3_irq_disable(irq_bit);
		}
	}
}

static int mdp3_dma_callback_setup(struct mdp3_dma *dma)
{
	int rc;
	struct mdp3_intr_cb vsync_cb = {
		.cb = mdp3_vsync_intr_handler,
		.data = dma,
	};

	struct mdp3_intr_cb dma_cb = {
		.cb = mdp3_dma_done_intr_handler,
		.data = dma,
	};

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO ||
		dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_LCDC)
		rc = mdp3_set_intr_callback(MDP3_INTR_LCDC_START_OF_FRAME,
					&vsync_cb);
	else if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		int irq_bit = MDP3_INTR_SYNC_PRIMARY_LINE;
		irq_bit += dma->dma_sel;
		rc = mdp3_set_intr_callback(irq_bit, &vsync_cb);
		irq_bit = MDP3_INTR_DMA_P_DONE;
		if (dma->dma_sel == MDP3_DMA_S)
			irq_bit = MDP3_INTR_DMA_S_DONE;
		rc |= mdp3_set_intr_callback(irq_bit, &dma_cb);
	} else {
		pr_err("mdp3_dma_callback_setup not suppported interface\n");
		rc = -ENODEV;
	}
	return rc;
}

static void mdp3_dma_vsync_enable(struct mdp3_dma *dma,
				struct mdp3_vsync_notification *vsync_client)
{
	unsigned long flag;
	int updated = 0;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;

	pr_debug("mdp3_dma_vsync_enable\n");

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (vsync_client) {
		if (dma->vsync_client.handler != vsync_client->handler) {
			dma->vsync_client = *vsync_client;
			updated = 1;
		}
	} else {
		if (dma->vsync_client.handler) {
			dma->vsync_client.handler = NULL;
			dma->vsync_client.arg = NULL;
			updated = 1;
		}
	}
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	if (updated) {
		if (vsync_client && vsync_client->handler)
			mdp3_dma_callback_enable(dma, cb_type);
		else
			mdp3_dma_callback_disable(dma, cb_type);
	}
}

static int mdp3_dmap_config(struct mdp3_dma *dma,
			struct mdp3_dma_source *source_config,
			struct mdp3_dma_output_config *output_config)
{
	u32 dma_p_cfg_reg, dma_p_size, dma_p_out_xy;

	dma_p_cfg_reg = source_config->format << 25;
	if (output_config->dither_en)
		dma_p_cfg_reg |= BIT(24);
	dma_p_cfg_reg |= output_config->out_sel << 19;
	dma_p_cfg_reg |= output_config->bit_mask_polarity << 18;
	dma_p_cfg_reg |= output_config->color_components_flip << 14;
	dma_p_cfg_reg |= output_config->pack_pattern << 8;
	dma_p_cfg_reg |= output_config->pack_align << 7;
	dma_p_cfg_reg |= output_config->color_comp_out_bits;

	dma_p_size = source_config->width | (source_config->height << 16);
	dma_p_out_xy = source_config->x | (source_config->y << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_P_CONFIG, dma_p_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_SIZE, dma_p_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_ADDR, (u32)source_config->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_Y_STRIDE, source_config->stride);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_OUT_XY, dma_p_out_xy);

	/*
	 * NOTE: MDP_DMA_P_FETCH_CFG: max_burst_size need to use value 4, not
	 * the default 16 for MDP hang issue workaround
	 */
	MDP3_REG_WRITE(MDP3_REG_DMA_P_FETCH_CFG, 0x20);
	MDP3_REG_WRITE(MDP3_REG_PRIMARY_RD_PTR_IRQ, 0x10);

	dma->source_config = *source_config;
	dma->output_config = *output_config;

	mdp3_dma_callback_setup(dma);
	return 0;
}

static int mdp3_dmas_config(struct mdp3_dma *dma,
			struct mdp3_dma_source *source_config,
			struct mdp3_dma_output_config *output_config)
{
	u32 dma_s_cfg_reg, dma_s_size, dma_s_out_xy;

	dma_s_cfg_reg = source_config->format << 25;
	if (output_config->dither_en)
		dma_s_cfg_reg |= BIT(24);
	dma_s_cfg_reg |= output_config->out_sel << 19;
	dma_s_cfg_reg |= output_config->bit_mask_polarity << 18;
	dma_s_cfg_reg |= output_config->color_components_flip << 14;
	dma_s_cfg_reg |= output_config->pack_pattern << 8;
	dma_s_cfg_reg |= output_config->pack_align << 7;
	dma_s_cfg_reg |= output_config->color_comp_out_bits;

	dma_s_size = source_config->width | (source_config->height << 16);
	dma_s_out_xy = source_config->x | (source_config->y << 16);

	MDP3_REG_WRITE(MDP3_REG_DMA_S_CONFIG, dma_s_cfg_reg);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_SIZE, dma_s_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_ADDR, (u32)source_config->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_Y_STRIDE, source_config->stride);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_OUT_XY, dma_s_out_xy);

	MDP3_REG_WRITE(MDP3_REG_SECONDARY_RD_PTR_IRQ, 0x10);

	dma->source_config = *source_config;
	dma->output_config = *output_config;

	mdp3_dma_callback_setup(dma);
	return 0;
}

static int mdp3_dmap_cursor_config(struct mdp3_dma *dma,
				struct mdp3_dma_cursor *cursor)
{
	u32 cursor_size, cursor_pos, blend_param, trans_mask;

	cursor_size = cursor->width | (cursor->height << 16);
	cursor_pos = cursor->x | (cursor->y << 16);
	trans_mask = 0;
	if (cursor->blend_config.mode == MDP3_DMA_CURSOR_BLEND_CONSTANT_ALPHA) {
		blend_param = cursor->blend_config.constant_alpha << 24;
	} else if (cursor->blend_config.mode ==
			MDP3_DMA_CURSOR_BLEND_COLOR_KEYING) {
		blend_param = cursor->blend_config.transparent_color;
		trans_mask = cursor->blend_config.transparency_mask;
	} else {
		blend_param = 0;
	}

	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_FORMAT, cursor->format);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_SIZE, cursor_size);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BUF_ADDR, (u32)cursor->buf);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_POS, cursor_pos);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_CONFIG,
			cursor->blend_config.mode);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_PARAM, blend_param);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_BLEND_TRANS_MASK, trans_mask);
	dma->cursor = *cursor;
	return 0;
}

static int mdp3_dmap_ccs_config(struct mdp3_dma *dma,
			struct mdp3_dma_color_correct_config *config,
			struct mdp3_dma_ccs *ccs,
			struct mdp3_dma_lut *lut)
{
	int i;
	u32 addr, cc_config, color;

	cc_config = config->lut_enable;
	if (config->ccs_enable)
		cc_config |= BIT(3);
	cc_config |= config->lut_position << 4;
	cc_config |= config->ccs_sel << 5;
	cc_config |= config->pre_bias_sel << 6;
	cc_config |= config->post_bias_sel << 7;
	cc_config |= config->pre_limit_sel << 8;
	cc_config |= config->post_limit_sel << 9;
	cc_config |= config->lut_sel << 10;

	MDP3_REG_WRITE(MDP3_REG_DMA_P_COLOR_CORRECT_CONFIG, cc_config);

	if (config->ccs_enable && ccs) {
		if (ccs->mv1) {
			addr = MDP3_REG_DMA_P_CSC_MV1;
			for (i = 0; i < 9; i++) {
				MDP3_REG_WRITE(addr, ccs->mv1[i]);
				addr += 4;
			}
		}

		if (ccs->mv2) {
			addr = MDP3_REG_DMA_P_CSC_MV2;
			for (i = 0; i < 9; i++) {
				MDP3_REG_WRITE(addr, ccs->mv2[i]);
				addr += 4;
			}
		}

		if (ccs->pre_bv1) {
			addr = MDP3_REG_DMA_P_CSC_PRE_BV1;
			for (i = 0; i < 3; i++) {
				MDP3_REG_WRITE(addr, ccs->pre_bv1[i]);
				addr += 4;
			}
		}

		if (ccs->pre_bv2) {
			addr = MDP3_REG_DMA_P_CSC_PRE_BV2;
			for (i = 0; i < 3; i++) {
				MDP3_REG_WRITE(addr, ccs->pre_bv2[i]);
				addr += 4;
			}
		}

		if (ccs->post_bv1) {
			addr = MDP3_REG_DMA_P_CSC_POST_BV1;
			for (i = 0; i < 3; i++) {
				MDP3_REG_WRITE(addr, ccs->post_bv1[i]);
				addr += 4;
			}
		}

		if (ccs->post_bv2) {
			addr = MDP3_REG_DMA_P_CSC_POST_BV2;
			for (i = 0; i < 3; i++) {
				MDP3_REG_WRITE(addr, ccs->post_bv2[i]);
				addr += 4;
			}
		}

		if (ccs->pre_lv1) {
			addr = MDP3_REG_DMA_P_CSC_PRE_LV1;
			for (i = 0; i < 6; i++) {
				MDP3_REG_WRITE(addr, ccs->pre_lv1[i]);
				addr += 4;
			}
		}

		if (ccs->pre_lv2) {
			addr = MDP3_REG_DMA_P_CSC_PRE_LV2;
			for (i = 0; i < 6; i++) {
				MDP3_REG_WRITE(addr, ccs->pre_lv2[i]);
				addr += 4;
			}
		}

		if (ccs->post_lv1) {
			addr = MDP3_REG_DMA_P_CSC_POST_LV1;
			for (i = 0; i < 6; i++) {
				MDP3_REG_WRITE(addr, ccs->post_lv1[i]);
				addr += 4;
			}
		}

		if (ccs->post_lv2) {
			addr = MDP3_REG_DMA_P_CSC_POST_LV2;
			for (i = 0; i < 6; i++) {
				MDP3_REG_WRITE(addr, ccs->post_lv2[i]);
				addr += 4;
			}
		}
	}

	if (config->lut_enable && lut) {
		if (lut->color0_lut1 && lut->color1_lut1 && lut->color2_lut1) {
			addr = MDP3_REG_DMA_P_CSC_LUT1;
			for (i = 0; i < 256; i++) {
				color = lut->color0_lut1[i];
				color |= lut->color1_lut1[i] << 8;
				color |= lut->color2_lut1[i] << 16;
				MDP3_REG_WRITE(addr, color);
				addr += 4;
			}
		}

		if (lut->color0_lut2 && lut->color1_lut2 && lut->color2_lut2) {
			addr = MDP3_REG_DMA_P_CSC_LUT2;
			for (i = 0; i < 256; i++) {
				color = lut->color0_lut2[i];
				color |= lut->color1_lut2[i] << 8;
				color |= lut->color2_lut2[i] << 16;
				MDP3_REG_WRITE(addr, color);
				addr += 4;
			}
		}
	}

	dma->ccs_config = *config;
	return 0;
}

static int mdp3_dmap_histo_config(struct mdp3_dma *dma,
			struct mdp3_dma_histogram_config *histo_config)
{
	u32 hist_bit_mask, hist_control;

	if (histo_config->bit_mask_polarity)
		hist_bit_mask = BIT(31);
	hist_bit_mask |= histo_config->bit_mask;

	if (histo_config->auto_clear_en)
		hist_control = BIT(0);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_FRAME_CNT,
			histo_config->frame_count);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_BIT_MASK, hist_bit_mask);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_CONTROL, hist_control);
	return 0;
}

static int mdp3_dmap_update(struct mdp3_dma *dma, void *buf)
{
	int wait_for_dma_done = 0;
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;

	pr_debug("mdp3_dmap_update\n");

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type |= MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		spin_lock_irqsave(&dma->dma_lock, flag);
		if (dma->busy)
			wait_for_dma_done = 1;
		spin_unlock_irqrestore(&dma->dma_lock, flag);

		if (wait_for_dma_done)
			wait_for_completion_killable(&dma->dma_comp);
	}

	spin_lock_irqsave(&dma->dma_lock, flag);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_IBUF_ADDR, (u32)buf);
	dma->source_config.buf = buf;
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		MDP3_REG_WRITE(MDP3_REG_DMA_P_START, 1);
		dma->busy = true;
	}
	wmb();
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	pr_debug("mdp3_dmap_update wait for vsync_comp in\n");
	wait_for_completion_killable(&dma->vsync_comp);
	pr_debug("mdp3_dmap_update wait for vsync_comp out\n");
	return 0;
}

static int mdp3_dmas_update(struct mdp3_dma *dma, void *buf)
{
	int wait_for_dma_done = 0;
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type |= MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		spin_lock_irqsave(&dma->dma_lock, flag);
		if (dma->busy)
			wait_for_dma_done = 1;
		spin_unlock_irqrestore(&dma->dma_lock, flag);

		if (wait_for_dma_done)
			wait_for_completion_killable(&dma->dma_comp);
	}

	spin_lock_irqsave(&dma->dma_lock, flag);
	MDP3_REG_WRITE(MDP3_REG_DMA_S_IBUF_ADDR, (u32)buf);
	dma->source_config.buf = buf;
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		MDP3_REG_WRITE(MDP3_REG_DMA_S_START, 1);
		dma->busy = true;
	}
	wmb();
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	wait_for_completion_killable(&dma->vsync_comp);
	return 0;
}

static int mdp3_dmap_cursor_update(struct mdp3_dma *dma, int x, int y)
{
	u32 cursor_pos;

	cursor_pos = x | (y << 16);
	MDP3_REG_WRITE(MDP3_REG_DMA_P_CURSOR_POS, cursor_pos);
	dma->cursor.x = x;
	dma->cursor.y = y;
	return 0;
}

static int mdp3_dmap_histo_get(struct mdp3_dma *dma,
			struct mdp3_dma_histogram_data *data)
{
	int i;
	u32 addr, extra;

	addr = MDP3_REG_DMA_P_HIST_R_DATA;
	for (i = 0; i < 32; i++) {
		data->r_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	addr = MDP3_REG_DMA_P_HIST_G_DATA;
	for (i = 0; i < 32; i++) {
		data->g_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	addr = MDP3_REG_DMA_P_HIST_B_DATA;
	for (i = 0; i < 32; i++) {
		data->b_data[i] = MDP3_REG_READ(addr);
		addr += 4;
	}

	extra = MDP3_REG_READ(MDP3_REG_DMA_P_HIST_EXTRA_INFO_0);
	data->r_min_value = (extra & 0x1F0000) >> 16;
	data->r_max_value = (extra & 0x1F000000) >> 24;
	extra = MDP3_REG_READ(MDP3_REG_DMA_P_HIST_EXTRA_INFO_1);
	data->g_min_value = extra & 0x1F;
	data->g_max_value = (extra & 0x1F00) >> 8;
	data->b_min_value = (extra & 0x1F0000) >> 16;
	data->b_max_value = (extra & 0x1F000000) >> 24;
	return 0;
}

static int mdp3_dmap_histo_op(struct mdp3_dma *dma, u32 op)
{
	switch (op) {
	case MDP3_DMA_HISTO_OP_START:
		MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_START, 1);
		break;
	case MDP3_DMA_HISTO_OP_STOP:
		MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_STOP_REQ, 1);
		break;
	case MDP3_DMA_HISTO_OP_CANCEL:
		MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_CANCEL_REQ, 1);
		break;
	case MDP3_DMA_HISTO_OP_RESET:
		MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_RESET_SEQ_START, 1);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int mdp3_dmap_histo_intr_status(struct mdp3_dma *dma, int *status)
{
	*status = MDP3_REG_READ(MDP3_REG_DMA_P_HIST_INTR_STATUS);
	return 0;
}

static int mdp3_dmap_histo_intr_enable(struct mdp3_dma *dma, u32 mask)
{
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_ENABLE, mask);
	return 0;
}

static int mdp3_dmap_histo_intr_clear(struct mdp3_dma *dma, u32 mask)
{
	MDP3_REG_WRITE(MDP3_REG_DMA_P_HIST_INTR_CLEAR, mask);
	return 0;
}

static int mdp3_dma_start(struct mdp3_dma *dma, struct mdp3_intf *intf)
{
	unsigned long flag;
	int cb_type = MDP3_DMA_CALLBACK_TYPE_VSYNC;
	u32 dma_start_offset = MDP3_REG_DMA_P_START;

	if (dma->dma_sel == MDP3_DMA_P)
		dma_start_offset = MDP3_REG_DMA_P_START;
	else if (dma->dma_sel == MDP3_DMA_S)
		dma_start_offset = MDP3_REG_DMA_S_START;
	else
		return -EINVAL;

	spin_lock_irqsave(&dma->dma_lock, flag);
	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_CMD) {
		cb_type |= MDP3_DMA_CALLBACK_TYPE_DMA_DONE;
		MDP3_REG_WRITE(dma_start_offset, 1);
		dma->busy = true;
	}

	intf->start(intf);
	wmb();
	init_completion(&dma->vsync_comp);
	spin_unlock_irqrestore(&dma->dma_lock, flag);

	mdp3_dma_callback_enable(dma, cb_type);
	pr_debug("mdp3_dma_start wait for vsync_comp in\n");
	wait_for_completion_killable(&dma->vsync_comp);
	pr_debug("mdp3_dma_start wait for vsync_comp out\n");
	return 0;
}

static int mdp3_dma_stop(struct mdp3_dma *dma, struct mdp3_intf *intf)
{
	int ret = 0;
	u32 status, display_status_bit;

	if (dma->dma_sel == MDP3_DMA_P)
		display_status_bit = BIT(6);
	else if (dma->dma_sel == MDP3_DMA_S)
		display_status_bit = BIT(7);
	else
		return -EINVAL;

	if (dma->output_config.out_sel == MDP3_DMA_OUTPUT_SEL_DSI_VIDEO)
		display_status_bit |= BIT(11);

	intf->stop(intf);
	ret = readl_poll_timeout((mdp3_res->mdp_base + MDP3_REG_DISPLAY_STATUS),
				status,
				((status & display_status_bit) == 0),
				DMA_STOP_POLL_SLEEP_US,
				DMA_STOP_POLL_TIMEOUT_US);

	mdp3_dma_callback_disable(dma, MDP3_DMA_CALLBACK_TYPE_VSYNC |
					MDP3_DMA_CALLBACK_TYPE_DMA_DONE);

	dma->busy = false;
	return ret;
}

int mdp3_dma_init(struct mdp3_dma *dma,
		struct mdp3_dma_source *source_config,
		struct mdp3_dma_output_config *output_config)
{
	int ret = 0;

	pr_debug("mdp3_dma_init\n");
	switch (dma->dma_sel) {
	case MDP3_DMA_P:
		dma->busy = 0;

		ret = mdp3_dmap_config(dma, source_config, output_config);
		if (ret < 0)
			return ret;

		dma->config_cursor = mdp3_dmap_cursor_config;
		dma->config_ccs = mdp3_dmap_ccs_config;
		dma->config_histo = mdp3_dmap_histo_config;
		dma->update = mdp3_dmap_update;
		dma->update_cursor = mdp3_dmap_cursor_update;
		dma->get_histo = mdp3_dmap_histo_get;
		dma->histo_op = mdp3_dmap_histo_op;
		dma->histo_intr_status = mdp3_dmap_histo_intr_status;
		dma->histo_intr_enable = mdp3_dmap_histo_intr_enable;
		dma->histo_intr_clear = mdp3_dmap_histo_intr_clear;
		dma->vsync_enable = mdp3_dma_vsync_enable;
		dma->start = mdp3_dma_start;
		dma->stop = mdp3_dma_stop;
		break;
	case MDP3_DMA_S:
		dma->busy = 0;
		ret = mdp3_dmas_config(dma, source_config, output_config);
		if (ret < 0)
			return ret;

		dma->config_cursor = NULL;
		dma->config_ccs = NULL;
		dma->config_histo = NULL;
		dma->update = mdp3_dmas_update;
		dma->update_cursor = NULL;
		dma->get_histo = NULL;
		dma->histo_op = NULL;
		dma->histo_intr_status = NULL;
		dma->histo_intr_enable = NULL;
		dma->histo_intr_clear = NULL;
		dma->vsync_enable = mdp3_dma_vsync_enable;
		dma->start = mdp3_dma_start;
		dma->stop = mdp3_dma_stop;
		break;
	case MDP3_DMA_E:
	default:
		ret = -ENODEV;
		break;
	}

	spin_lock_init(&dma->dma_lock);
	init_completion(&dma->vsync_comp);
	init_completion(&dma->dma_comp);
	dma->cb_type = 0;
	dma->vsync_client.handler = NULL;
	dma->vsync_client.arg = NULL;

	memset(&dma->cursor, 0, sizeof(dma->cursor));
	memset(&dma->ccs_config, 0, sizeof(dma->ccs_config));
	memset(&dma->histogram_config, 0, sizeof(dma->histogram_config));

	return ret;
}

int lcdc_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 temp;
	struct mdp3_video_intf_cfg *v = &cfg->video;
	temp = v->hsync_pulse_width | (v->hsync_period << 16);
	MDP3_REG_WRITE(MDP3_REG_LCDC_HSYNC_CTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_VSYNC_PERIOD, v->vsync_period);
	MDP3_REG_WRITE(MDP3_REG_LCDC_VSYNC_PULSE_WIDTH, v->vsync_pulse_width);
	temp = v->display_start_x | (v->display_end_x << 16);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_V_START, v->display_start_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_DISPLAY_V_END, v->display_end_y);
	temp = v->active_start_x | (v->active_end_x);
	if (v->active_h_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_V_START, v->active_start_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_ACTIVE_V_END, v->active_end_y);
	MDP3_REG_WRITE(MDP3_REG_LCDC_HSYNC_SKEW, v->hsync_skew);
	temp = 0;
	if (!v->hsync_polarity)
		temp = BIT(0);
	if (!v->vsync_polarity)
		temp = BIT(1);
	if (!v->de_polarity)
		temp = BIT(2);
	MDP3_REG_WRITE(MDP3_REG_LCDC_CTL_POLARITY, temp);

	return 0;
}

int lcdc_start(struct mdp3_intf *intf)
{
	MDP3_REG_WRITE(MDP3_REG_LCDC_EN, BIT(0));
	wmb();
	intf->active = true;
	return 0;
}

int lcdc_stop(struct mdp3_intf *intf)
{
	MDP3_REG_WRITE(MDP3_REG_LCDC_EN, 0);
	wmb();
	intf->active = false;
	return 0;
}

int dsi_video_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 temp;
	struct mdp3_video_intf_cfg *v = &cfg->video;

	pr_debug("dsi_video_config\n");

	temp = v->hsync_pulse_width | (v->hsync_period << 16);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_HSYNC_CTL, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_VSYNC_PERIOD, v->vsync_period);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_VSYNC_PULSE_WIDTH,
			v->vsync_pulse_width);
	temp = v->display_start_x | (v->display_end_x << 16);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_HCTL, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_V_START, v->display_start_y);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_DISPLAY_V_END, v->display_end_y);
	temp = v->active_start_x | (v->active_end_x << 16);
	if (v->active_h_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_HCTL, temp);

	temp = v->active_start_y;
	if (v->active_v_enable)
		temp |= BIT(31);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_V_START, temp);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_ACTIVE_V_END, v->active_end_y);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_HSYNC_SKEW, v->hsync_skew);
	temp = 0;
	if (!v->hsync_polarity)
		temp |= BIT(0);
	if (!v->vsync_polarity)
		temp |= BIT(1);
	if (!v->de_polarity)
		temp |= BIT(2);
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_CTL_POLARITY, temp);

	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_UNDERFLOW_CTL, 0x800000ff);
	return 0;
}

int dsi_video_start(struct mdp3_intf *intf)
{
	pr_debug("dsi_video_start\n");
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_EN, BIT(0));
	wmb();
	intf->active = true;
	return 0;
}

int dsi_video_stop(struct mdp3_intf *intf)
{
	pr_debug("dsi_video_stop\n");
	MDP3_REG_WRITE(MDP3_REG_DSI_VIDEO_EN, 0);
	wmb();
	intf->active = false;
	return 0;
}

int dsi_cmd_config(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	u32 id_map = 0;
	u32 trigger_en = 0;

	if (cfg->dsi_cmd.primary_dsi_cmd_id)
		id_map = BIT(0);
	if (cfg->dsi_cmd.secondary_dsi_cmd_id)
		id_map = BIT(4);

	if (cfg->dsi_cmd.dsi_cmd_tg_intf_sel)
		trigger_en = BIT(4);

	MDP3_REG_WRITE(MDP3_REG_DSI_CMD_MODE_ID_MAP, id_map);
	MDP3_REG_WRITE(MDP3_REG_DSI_CMD_MODE_TRIGGER_EN, trigger_en);

	return 0;
}

int dsi_cmd_start(struct mdp3_intf *intf)
{
	intf->active = true;
	return 0;
}

int dsi_cmd_stop(struct mdp3_intf *intf)
{
	intf->active = false;
	return 0;
}

int mdp3_intf_init(struct mdp3_intf *intf, struct mdp3_intf_cfg *cfg)
{
	int ret = 0;
	switch (cfg->type) {
	case MDP3_DMA_OUTPUT_SEL_LCDC:
		intf->config = lcdc_config;
		intf->start = lcdc_start;
		intf->stop = lcdc_stop;
		break;
	case MDP3_DMA_OUTPUT_SEL_DSI_VIDEO:
		intf->config = dsi_video_config;
		intf->start = dsi_video_start;
		intf->stop = dsi_video_stop;
		break;
	case MDP3_DMA_OUTPUT_SEL_DSI_CMD:
		intf->config = dsi_cmd_config;
		intf->start = dsi_cmd_start;
		intf->stop = dsi_cmd_stop;
		break;

	default:
		return -EINVAL;
	}

	intf->active = false;
	if (intf->config)
		ret = intf->config(intf, cfg);

	if (ret) {
		pr_err("MDP interface initialization failed\n");
		return ret;
	}

	intf->cfg = *cfg;
	return 0;
}