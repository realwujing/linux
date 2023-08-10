/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Synopsys DesignWare AXI DMA Controller driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _AXI_DMA_PLATFORM_H
#define _AXI_DMA_PLATFORM_H

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/types.h>

#include "../virt-dma.h"

#define DMAC_MAX_CHANNELS	4
#define DMAC_MAX_MASTERS	1
#define DMAC_MAX_BLK_SIZE	(1024 * 1024 * 2)

struct phytium_dma_hcfg {
	u32	nr_channels;
	u32	nr_masters;
	u32	m_data_width;
	u32	block_size[DMAC_MAX_CHANNELS];
	u32	priority[DMAC_MAX_CHANNELS];
	/* maximum supported axi burst length */
	u32	axi_rw_burst_len;
	bool	restrict_axi_burst_len;
};

struct phytium_dma_chan {
	struct phytium_dma_chip		*chip;
	void __iomem			*chan_regs;
	u8				id;
	unsigned int			irq;
	atomic_t			descs_allocated;
	atomic_t			releasing;

	struct virt_dma_chan		vc;

	struct dma_slave_config dma_sconfig;

	bool                is_used;
	bool                is_idle;
	/* these other elements are all protected by vc.lock */
	bool				is_paused;
};

struct phytium_dma {
	struct dma_device	dma;
	//const struct phytium_dma_hcfg	*hdata;

	/* channels */
	struct phytium_dma_chan	*chan;
};

struct phytium_dma_chip {
	struct pci_dev		*pdev;
	struct device		*dev;
	void __iomem		*regs;
	struct clk		*core_clk;
	struct phytium_dma	*dmac;

	int			irq;
	int         id;
	struct completion complete;

	const struct phytium_dma_hcfg	*hdata;
	struct phytium_dma_chan chan[4];
};

struct phytium_chan_desc {
	u32 desc_status;
	u32 desc_control;
	u32 desc_next_addr_l;
	u32 desc_next_addr_h;
	u32 desc_src_addr_l;
	u32 desc_src_addr_h;
	u32 desc_dst_addr_l;
	u32 desc_dst_addr_h;
} __attribute__ ((__packed__));

struct phytium_dma_desc {
	struct virt_dma_desc		vd;
	struct phytium_dma_chan		*chan;
	struct list_head		xfer_list;
	struct dma_async_tx_descriptor *txd;
	struct phytium_chan_desc *cdesc;
	struct scatterlist *end_sg;
	u32		dma_srcparam;
	u32		dma_destparam;
	u32		dma_srcaddr_l;
	u32		dma_srcaddr_h;
	u32		dma_destaddr_l;
	u32		dma_destaddr_h;
	u32		dma_len;
	u32		dma_ctrl;
	u32		sg_len;
};

static inline struct device *dchan2dev(struct dma_chan *dchan)
{
	return &dchan->dev->device;
}

static inline struct device *chan2dev(struct phytium_dma_chan *chan)
{
	return &chan->vc.chan.dev->device;
}

static inline struct phytium_dma_desc *vd_to_desc(struct virt_dma_desc *vd)
{
	return container_of(vd, struct phytium_dma_desc, vd);
}

static inline struct phytium_dma_chan *vc_to_dma_chan(struct virt_dma_chan *vc)
{
	return container_of(vc, struct phytium_dma_chan, vc);
}

static inline struct phytium_dma_chan *dchan_to_dma_chan(struct dma_chan *dchan)
{
	return vc_to_dma_chan(to_virt_chan(dchan));
}

#define TX_SG_LEN			(1024 * 32)
#define PAGE_SIZE2			(PAGE_SIZE << 1)

#define COMMON_REG_LEN			0
#define CHAN_REG_LEN			0x40

#define DMA_IRQ_MASK			0x180
#define DMA_IRQ_STATUS			0x184
#define DMA_SRCPARAM			0x400
#define DMA_DESTPARAM			0x404
#define DMA_SRCADDR_L			0x408
#define DMA_SRCADDR_H			0x40C
#define DMA_DESTADDR_L			0x410
#define DMA_DESTADDR_H			0x414
#define DMA_LENGTH			0x418
#define DMA_CONTROL			0x41C
#define DMA_STATUS			0x420
#define DMA_PRC_LEN			0x424
#define DMA_SHARE_ACCESS		0x428

#define DMA_SRCP_SRC_ID(x)		min_t(unsigned int, x, 0xF)
#define DMA_SRCP_TRSF_PARAM(x)		(min_t(unsigned int, x, 0xFFF) << 16)
#define DMA_DESTP_DEST_ID(x)		min_t(unsigned int, x, 0xF)
#define DMA_DESTP_TRSF_PARAM(x)		(min_t(unsigned int, x, 0xFFF) << 16)
#define DMA_CTRL_START			BIT(0)
#define DMA_CTRL_PAUSE			BIT(1)
#define DMA_CTRL_SG_EN			BIT(3)
#define DMA_CTRL_COND_LEN		BIT(5)
#define DMA_CTRL_COND_EOP		BIT(6)
#define DMA_CTRL_COND_ERR		BIT(7)
#define DMA_CTRL_COND_MASK(x)		(min_t(unsigned int, x, 0xF) << 4)
#define DMA_CTRL_IRQ_END		BIT(8)
#define DMA_CTRL_IRQ_ERR		BIT(9)
#define DMA_CTRL_IRQ_EOP		BIT(10)
#define DMA_CTRL_IRQ_MASK(x)		(min_t(unsigned int, x, 0xF) << 8)
#define DMA_CTRL_IRQ_ID(x)		(min_t(unsigned int, x, 0x3) << 12)
#define DMA_CTRL_DESC_UPDT		BIT(23)
#define DMA_CTRL_SG_TYPE(x)		(min_t(unsigned int, x, 0x3) << 24)
#define DMA_CTRL_SG_ID(x)		(min_t(unsigned int, x, 0x5) << 26)
#define DMA_CTRL_SG2_ID(x)		(min_t(unsigned int, x, 0x5) << 29)
#define DMA_STAT_MASK(x)		(min_t(unsigned int, x, 0xFF) << 0)
#define DMA_STAT_END			BIT(0)
#define DMA_STAT_EOP			BIT(1)
#define DMA_STAT_EOC			BIT(2)
#define DMA_STAT_ERR			BIT(3)
#define DMA_STAT_OVER			BIT(4)
#define DMA_STAT_STOP			BIT(6)
#define DMA_STAT_INCORRECT		BIT(7)
#define DMA_SRC_ERROR_MASK(x)		(min_t(unsigned int, x, 0xFF) << 8)
#define DMA_DEST_ERROR_MASK(x)		(min_t(unsigned int, x, 0xFF) << 16)
#define DMA_DESC_ERROR_MASK(x)		(min_t(unsigned int, x, 0xFF) << 24)

#define DMA_TIMEOUT  200

#endif /* _AXI_DMA_PLATFORM_H */
