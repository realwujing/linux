// SPDX-License-Identifier:  GPL-2.0
/*
 * Copyright (c) 2021 Phytium Limited.
 *
 * Phytium PCI DMA Controller driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dmapool.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/of_device.h>

#include <linux/pci.h>
#include <linux/msi.h>

#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "phytium-dmac-pci.h"
#include "../dmaengine.h"
#include "../virt-dma.h"

static const struct dma_slave_map ft_x100gpu_slave_map[] = {
	{"x100-dma", "tx", 0},
	{"x100-dma", "rx", 0},
};

bool x100_filter_fn(struct dma_chan *chan, void *param);

static inline void
phytium_dma_iowrite32(struct phytium_dma_chip *chip, u32 reg, u32 val)
{
	iowrite32(val, chip->regs + reg);
}

static inline u32 phytium_dma_ioread32(struct phytium_dma_chip *chip, u32 reg)
{
	return ioread32(chip->regs + reg);
}

static inline void
phytium_chan_iowrite32(struct phytium_dma_chan *chan, u32 reg, u32 val)
{
	iowrite32(val, chan->chan_regs + reg);
}

static inline u32 phytium_chan_ioread32(struct phytium_dma_chan *chan, u32 reg)
{
	return ioread32(chan->chan_regs + reg);
}

static inline void phytium_dma_irq_disable(struct phytium_dma_chip *chip)
{
	u32 val;

	val = 0;
	phytium_dma_iowrite32(chip, DMA_IRQ_MASK, val);
}

static inline void phytium_dma_irq_enable(struct phytium_dma_chip *chip)
{
	u32 val;

	val = 0x4077FFFF;
	phytium_dma_iowrite32(chip, DMA_IRQ_MASK, val);
}

static inline void phytium_dma_irq_clear(struct phytium_dma_chip *chip,
		u32 irq_status)
{
	phytium_dma_iowrite32(chip, DMA_IRQ_STATUS, irq_status);
}

static inline u32 phytium_dma_irq_read(struct phytium_dma_chip *chip)
{
	u32 val;

	val = phytium_dma_ioread32(chip, DMA_IRQ_STATUS);

	return val;
}

static inline u32 phytium_dma_irq_mask_read(struct phytium_dma_chip *chip)
{
	u32 val;

	val = phytium_dma_ioread32(chip, DMA_IRQ_MASK);

	return val;
}


static inline void phytium_chan_irq_disable(struct phytium_dma_chan *chan, u32 irq_mask)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_CONTROL);
	val &= ~irq_mask;
	phytium_chan_iowrite32(chan, DMA_CONTROL, val);
}

static inline void phytium_chan_irq_set(struct phytium_dma_chan *chan, u32 irq_mask)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_CONTROL);
	val &= ~DMA_CTRL_IRQ_MASK(0xF);
	val |= irq_mask;
	val |= DMA_CTRL_IRQ_ID(1);
	phytium_chan_iowrite32(chan, DMA_CONTROL, val);
}

static inline void phytium_chan_irq_clear(struct phytium_dma_chan *chan, u32 irq_mask)
{
	u32 val;

	val = phytium_dma_ioread32(chan->chip, DMA_IRQ_STATUS);
	val &= ~irq_mask;
	phytium_chan_iowrite32(chan, DMA_IRQ_STATUS, val);
}

static inline u32 phytium_chan_irq_read(struct phytium_dma_chan *chan)
{
	u32 val;

	val = phytium_dma_ioread32(chan->chip, DMA_IRQ_STATUS);
	val &= ((chan->id << 0) | (chan->id << 8));

	return val;
}

static inline u32 phytium_chan_status_read(struct phytium_dma_chan *chan)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_STATUS);

	return val;
}

static inline void phytium_chan_disable(struct phytium_dma_chan *chan)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_CONTROL);
	val &= ~DMA_CTRL_START;
	phytium_chan_iowrite32(chan, DMA_CONTROL, val);
}

static inline void phytium_chan_enable(struct phytium_dma_chan *chan)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_CONTROL);
	val |= DMA_CTRL_START;
	phytium_chan_iowrite32(chan, DMA_CONTROL, val);
}

static inline const char *phytium_chan_name(struct phytium_dma_chan *chan)
{
	return dma_chan_name(&chan->vc.chan);
}

static struct phytium_dma_desc *phytium_desc_get(struct phytium_dma_chan *chan)
{
	struct phytium_dma_desc *desc = NULL;

	desc = kmalloc(sizeof(*desc), GFP_KERNEL);
	if (unlikely(!desc)) {
		dev_err(chan2dev(chan), "%s: not enough descriptors available\n",
			phytium_chan_name(chan));
		return NULL;
	}

	memset(desc, 0, sizeof(*desc));

	INIT_LIST_HEAD(&desc->xfer_list);

	return desc;
}

static void phytium_desc_put(struct phytium_dma_desc *desc)
{
	unsigned int descs_put = 0;

	kfree(desc);

	descs_put++;
}

static void phytium_vchan_desc_put(struct virt_dma_desc *vdesc)
{
	phytium_desc_put(vd_to_desc(vdesc));
}

static inline bool phytium_chan_is_running(struct phytium_dma_chan *chan)
{
	u32 val;

	val = phytium_chan_ioread32(chan, DMA_CONTROL);

	if (val & DMA_CTRL_START)
		return true;
	else
		return false;
}

/* Called in chan locked context */
static void phytium_chan_block_xfer_start(struct phytium_dma_chan *chan,
				      struct phytium_dma_desc *desc)
{
	u32 irq_mask;
	u32 val;

	chan->is_idle = false;
	if (phytium_chan_is_running(chan)) {
		dev_err(chan2dev(chan), "%s is non-idle!\n", phytium_chan_name(chan));
		return;
	}

	phytium_chan_iowrite32(chan, DMA_SRCPARAM, desc->dma_srcparam);
	phytium_chan_iowrite32(chan, DMA_DESTPARAM, desc->dma_destparam);
	phytium_chan_iowrite32(chan, DMA_SRCADDR_L, desc->dma_srcaddr_l);
	phytium_chan_iowrite32(chan, DMA_SRCADDR_H, desc->dma_srcaddr_h);
	phytium_chan_iowrite32(chan, DMA_DESTADDR_L, desc->dma_destaddr_l);
	phytium_chan_iowrite32(chan, DMA_DESTADDR_H, desc->dma_destaddr_h);
	phytium_chan_iowrite32(chan, DMA_LENGTH, desc->dma_len);

	/* SG_TYPE & IRQ_ID & SE_COND & SG_EN */
	val = phytium_chan_ioread32(chan, DMA_CONTROL);
	val |= desc->dma_ctrl;
	val |= DMA_CTRL_COND_LEN | DMA_CTRL_COND_EOP | DMA_CTRL_COND_ERR;

	phytium_chan_iowrite32(chan, DMA_CONTROL, val);

	irq_mask = DMA_CTRL_IRQ_END | DMA_CTRL_IRQ_EOP | DMA_CTRL_IRQ_ERR;
	phytium_chan_irq_set(chan, irq_mask);

	phytium_chan_enable(chan);
}

static void phytium_chan_start_first_queued(struct phytium_dma_chan *chan)
{
	struct phytium_dma_desc *desc = NULL;
	struct virt_dma_desc *vd = NULL;

	vd = vchan_next_desc(&chan->vc);
	if (!vd)
		return;

	desc = vd_to_desc(vd);
	dev_dbg(chan2dev(chan), "%s: started %u\n", phytium_chan_name(chan),
		vd->tx.cookie);

	phytium_chan_block_xfer_start(chan, desc);
}

static void phytium_dma_hw_init(struct phytium_dma_chip *chip)
{
	u32 i;

	for (i = 0; i < chip->hdata->nr_channels; i++) {
		phytium_chan_irq_disable(&chip->dmac->chan[i], DMA_CTRL_IRQ_MASK(0xF));
		phytium_chan_disable(&chip->dmac->chan[i]);
	}
}


static enum dma_status phytium_dma_tx_status(struct dma_chan *dchan,
					   dma_cookie_t cookie,
					   struct dma_tx_state *state)
{
	enum dma_status ret;

	ret = dma_cookie_status(dchan, cookie, state);

	return ret;
}

static void phytium_dma_issue_pending(struct dma_chan *dchan)
{
	struct phytium_dma_chan *chan = dchan_to_dma_chan(dchan);
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);
	if (vchan_issue_pending(&chan->vc) && chan->is_idle)
		phytium_chan_start_first_queued(chan);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static void phytium_dma_synchronize(struct dma_chan *dchan)
{
	struct phytium_dma_chan *chan = dchan_to_dma_chan(dchan);

	vchan_synchronize(&chan->vc);
}

static int phytium_dma_terminate_all(struct dma_chan *dchan)
{
	struct phytium_dma_chan *chan = dchan_to_dma_chan(dchan);
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&chan->vc.lock, flags);

	phytium_chan_disable(chan);

	vchan_get_all_descriptors(&chan->vc, &head);

	/*
	 * As vchan_dma_desc_free_list can access to desc_allocated list
	 * we need to call it in vc.lock context.
	 */
	vchan_dma_desc_free_list(&chan->vc, &head);

	spin_unlock_irqrestore(&chan->vc.lock, flags);

	dev_vdbg(dchan2dev(dchan), "terminated: %s\n", phytium_chan_name(chan));

	return 0;
}

static int phytium_dma_alloc_chan_resources(struct dma_chan *dchan)
{
	struct phytium_dma_chan *chan = dchan_to_dma_chan(dchan);

	/* ASSERT: channel is idle */
	if (chan->is_used) {
		dev_err(chan2dev(chan), "%s is non-idle!\n",
			phytium_chan_name(chan));
		return -EBUSY;
	}

	chan->is_used = true;
	atomic_set(&chan->releasing, 0);

	return 0;
}

static void phytium_dma_free_chan_resources(struct dma_chan *dchan)
{
	struct phytium_dma_chan *chan = dchan_to_dma_chan(dchan);

	atomic_set(&chan->releasing, 1);
	/* ASSERT: channel is idle */
	if (chan->is_used)
		dev_err(dchan2dev(dchan), "%s is non-idle!\n",
			phytium_chan_name(chan));

	phytium_chan_disable(chan);
	phytium_chan_irq_disable(chan, DMA_CTRL_IRQ_MASK(0xF));

	vchan_free_chan_resources(&chan->vc);

	dev_vdbg(dchan2dev(dchan),
		 "%s: free resources, descriptor still allocated: %u\n",
		 phytium_chan_name(chan), atomic_read(&chan->descs_allocated));

	chan->is_used = false;
}

static int phytium_dma_slave_config(struct dma_chan *dchan,
				  struct dma_slave_config *sconfig)
{
	struct phytium_dma_chan	*chan = dchan_to_dma_chan(dchan);

	/* Check if chan will be configured for slave transfers */
	if (!is_slave_direction(sconfig->direction))
		return -EINVAL;

	memcpy(&chan->dma_sconfig, sconfig, sizeof(*sconfig));

	return 0;
}

static struct dma_async_tx_descriptor *phytium_dma_prep_dma_memcpy(
	struct dma_chan *dchan, dma_addr_t dst_adr,
	dma_addr_t src_adr, size_t len, unsigned long flags)
{
	struct phytium_dma_desc *desc = NULL;
	struct phytium_dma_chan *chan  = dchan_to_dma_chan(dchan);
	struct dma_slave_config	*sconfig = &chan->dma_sconfig;
	size_t block_ts, max_block_ts, xfer_len;

	chan = dchan_to_dma_chan(dchan);

	dev_info(chan2dev(chan), "%s: memcpy: src: %pad dst: %pad length: %zd flags: %#lx",
		phytium_chan_name(chan), &src_adr, &dst_adr, len, flags);

	max_block_ts = chan->chip->hdata->block_size[chan->id];
	if (len > max_block_ts) {
		dev_err(chan2dev(chan), "The data length(%zd) is too long!\n", len);
		return NULL;
	}

	xfer_len = len;
	block_ts = xfer_len >> DMA_SLAVE_BUSWIDTH_4_BYTES;
	xfer_len = block_ts << DMA_SLAVE_BUSWIDTH_4_BYTES;

	desc = phytium_desc_get(chan);
	if (unlikely(!desc))
		goto err_desc_get;

	switch (sconfig->direction) {
	case DMA_MEM_TO_DEV:
		desc->dma_srcparam = DMA_SRCP_SRC_ID(0);
		desc->dma_destparam = DMA_DESTP_DEST_ID(5);
		break;
	case DMA_DEV_TO_MEM:
		desc->dma_srcparam = DMA_SRCP_SRC_ID(5);
		desc->dma_destparam = DMA_DESTP_DEST_ID(0);
		break;
	default:
		return NULL;
	}

	desc->dma_ctrl |= DMA_CTRL_SG_EN;
	desc->dma_ctrl |= DMA_CTRL_SG_TYPE(0);

	desc->dma_ctrl |= DMA_CTRL_SG_ID(0);
	desc->dma_ctrl |= DMA_CTRL_SG2_ID(0);
	desc->dma_ctrl |= DMA_CTRL_IRQ_ID(0);

	desc->dma_srcaddr_l = src_adr & 0xFFFFFFFF;
	desc->dma_srcaddr_h = (src_adr >> 32) & 0xFFFFFFFF;
	desc->dma_destaddr_l = dst_adr & 0xFFFFFFFF;
	desc->dma_destaddr_h = (dst_adr >> 32) & 0xFFFFFFFF;
	desc->dma_len = xfer_len;

	return vchan_tx_prep(&chan->vc, &desc->vd, flags);

err_desc_get:
	phytium_desc_put(desc);
	return NULL;
}

static struct dma_async_tx_descriptor *phytium_dma_prep_slave_sg(
	struct dma_chan *dchan, struct scatterlist *sgl,
	u32 sg_len, enum dma_transfer_direction direction,
	unsigned long flags, void *context)
{
	struct phytium_dma_chan	*chan = dchan_to_dma_chan(dchan);
	struct dma_slave_config	*sconfig = &chan->dma_sconfig;
	struct phytium_dma_desc *desc = NULL;
	struct phytium_chan_desc *chan_desc = NULL;
	dma_addr_t		reg;
	unsigned int		i = 0;
	struct scatterlist	*sg = NULL;
	size_t			total_len = 0;
	u32 offset = 0;

	if (unlikely(!is_slave_direction(direction) || !sg_len)) {
		dev_err(chan2dev(chan), "%s: bad direction\n", __func__);
		return NULL;
	}

	if (atomic_read(&chan->releasing)) {
		dev_err(chan2dev(chan), "%s: channel is scheduled for release\n", __func__);
		return NULL;
	}

	desc = phytium_desc_get(chan);
	if (unlikely(!desc))
		goto err_desc_get;

	chan_desc = kzalloc((sg_len + 1) * sizeof(struct phytium_chan_desc),
			GFP_KERNEL | GFP_DMA);
	if (unlikely(!chan_desc)) {
		dev_err(chan->chip->dev, "Malloc space for chan_desc failed\n");
		goto err_desc_get;
	}

	desc->cdesc = chan_desc;

	/* Must be aligned on 32-byte boundary */
	offset = (u64)chan_desc & 0x1F;
	if (offset)
		chan_desc = chan_desc + 32 - offset;

	for_each_sg(sgl, sg, sg_len, i) {
		if (!sg)
			break;

		if (direction == DMA_MEM_TO_DEV) {
			chan_desc[i].desc_src_addr_l = sg_dma_address(sg) & 0xFFFFFFFF;
			chan_desc[i].desc_src_addr_h = (sg_dma_address(sg) >> 32) & 0xFFFFFFFF;
		} else if (direction == DMA_DEV_TO_MEM) {
			chan_desc[i].desc_dst_addr_l = sg_dma_address(sg) & 0xFFFFFFFF;
			chan_desc[i].desc_dst_addr_h = (sg_dma_address(sg) >> 32) & 0xFFFFFFFF;
		} else {
			kfree(desc->cdesc);
			goto err_desc_get;
		}

		chan_desc[i].desc_control = 0;
		chan_desc[i].desc_control |= sg_dma_len(sg) << 8;

		if (sg_is_last(sg)) {
			chan_desc[i].desc_next_addr_l = 0x01;
			chan_desc[i].desc_next_addr_h = 0;
			chan_desc[i].desc_control |= 0x03 << 4;
		} else {
			chan_desc[i].desc_next_addr_l = virt_to_phys(&chan_desc[i+1]) & 0xFFFFFFFF;
			chan_desc[i].desc_next_addr_l |= 0x01 << 4;
			chan_desc[i].desc_next_addr_l |= 0x02;
			chan_desc[i].desc_next_addr_h =
				(virt_to_phys(&chan_desc[i+1]) >> 32) & 0xFFFFFFFF;
		}
		total_len += sg_dma_len(sg);
	}

	switch (direction) {
	case DMA_MEM_TO_DEV:
		reg = sconfig->dst_addr;

		desc->dma_srcparam = DMA_SRCP_SRC_ID(0);
		desc->dma_destparam = DMA_DESTP_DEST_ID(5);
		desc->dma_ctrl |= DMA_CTRL_SG_TYPE(1);

		desc->dma_srcaddr_l = virt_to_phys(chan_desc) & 0xFFFFFFFF;
		desc->dma_srcaddr_h = (virt_to_phys(chan_desc) >> 32) & 0xFFFFFFFF;
		desc->dma_destaddr_l = sconfig->dst_addr & 0xFFFFFFFF;
		desc->dma_destaddr_h = (sconfig->dst_addr >> 32) & 0xFFFFFFFF;
		break;
	case DMA_DEV_TO_MEM:
		reg = sconfig->src_addr;

		/* memcpy from device */
		desc->dma_srcparam = DMA_SRCP_SRC_ID(5);
		desc->dma_destparam = DMA_DESTP_DEST_ID(0);
		desc->dma_ctrl |= DMA_CTRL_SG_TYPE(2);

		desc->dma_srcaddr_l = sconfig->src_addr & 0xFFFFFFFF;
		desc->dma_srcaddr_h = (sconfig->src_addr >> 32) & 0xFFFFFFFF;
		desc->dma_destaddr_l = virt_to_phys(chan_desc) & 0xFFFFFFFF;
		desc->dma_destaddr_h = (virt_to_phys(chan_desc) >> 32) & 0xFFFFFFFF;
		break;
	default:
		return NULL;
	}

	desc->dma_ctrl |= DMA_CTRL_SG_EN;

	desc->dma_ctrl |= DMA_CTRL_SG_ID(0);
	desc->dma_ctrl |= DMA_CTRL_SG2_ID(0);
	desc->dma_ctrl |= DMA_CTRL_IRQ_ID(0);

	desc->dma_len = total_len;

	return vchan_tx_prep(&chan->vc, &desc->vd, flags);

err_desc_get:
	dev_err(chan2dev(chan),
		"not enough descriptors available. Direction %d\n", direction);
	phytium_desc_put(desc);
	return NULL;
}

static void phytium_chan_block_xfer_complete_msi(struct phytium_dma_chan *chan)
{
	struct virt_dma_desc *vd = NULL;
	struct phytium_dma_desc *desc = NULL;

	chan->is_idle = true;
	if (phytium_chan_is_running(chan)) {
		dev_err(chan2dev(chan), "DMA %s is not idle!\n",
			phytium_chan_name(chan));
		phytium_chan_disable(chan);
	}

	/* The completed descriptor currently is in the head of vc list */
	vd = vchan_next_desc(&chan->vc);
	if (!vd) {
		dev_err(chan2dev(chan), "complete vchan next desc failed\n");
		return;
	}

	desc = vd_to_desc(vd);
	if (!desc) {
		dev_err(chan2dev(chan), "vd to desc failed\n");
		return;
	}

	if (desc->cdesc)
		kfree(desc->cdesc);

	/* Remove the completed descriptor from issued list before completing */
	list_del(&vd->node);
	vchan_cookie_complete(vd);

	/* Submit queued descriptors after processing the completed ones */
	phytium_chan_start_first_queued(chan);
}

static void phytium_chan_block_xfer_complete_intx(struct phytium_dma_chan *chan)
{
	struct virt_dma_desc *vd = NULL;
	struct phytium_dma_desc *desc = NULL;
	unsigned long flags;

	spin_lock_irqsave(&chan->vc.lock, flags);
	chan->is_idle = true;
	if (phytium_chan_is_running(chan)) {
		dev_err(chan2dev(chan), "DMA %s is not idle!\n",
			phytium_chan_name(chan));
		phytium_chan_disable(chan);
	}

	/* The completed descriptor currently is in the head of vc list */
	vd = vchan_next_desc(&chan->vc);
	if (!vd) {
		dev_err(chan2dev(chan), "complete vchan next desc failed\n");
		return;
	}

	desc = vd_to_desc(vd);
	if (!desc) {
		dev_err(chan2dev(chan), "vd to desc failed\n");
		return;
	}

	if (desc->cdesc)
		kfree(desc->cdesc);

	/* Remove the completed descriptor from issued list before completing */
	list_del(&vd->node);
	vchan_cookie_complete(vd);

	/* Submit queued descriptors after processing the completed ones */
	phytium_chan_start_first_queued(chan);
	spin_unlock_irqrestore(&chan->vc.lock, flags);
}

static irqreturn_t phytium_dma_interrupt_msi(int irq, void *dev_id)
{
	unsigned long flags;
	struct phytium_dma_chan *chan = dev_id;
	struct phytium_dma_chip *chip = chan->chip;

	u32 irq_status;
	u32 irq_mask = 0xffff;
	u32 chan_status;
	u32 mask = DMA_CTRL_IRQ_END | DMA_CTRL_IRQ_EOP | DMA_CTRL_IRQ_ERR;

	spin_lock_irqsave(&chan->vc.lock, flags);

	chan_status = phytium_chan_status_read(chan);
	irq_status = phytium_dma_irq_read(chip);

	if (!(irq_status & irq_mask)) {
		dev_err(chan2dev(chan), "irq_status = 0x%x, irq_mask = 0x%x, chan_status = 0x%x",
				irq_status, irq_mask, chan_status);
		if ((chan_status & DMA_STAT_END) | (chan_status & DMA_STAT_EOP) |
				(chan_status & DMA_STAT_EOC) |
				(chan_status & DMA_STAT_ERR)) {
			irq_status = phytium_dma_irq_read(chip);
			goto next;
		} else {
			return IRQ_NONE;
		}
	}

next:
	/* Disable DMAC inerrupts. We'll enable them after processing chanels */
	phytium_dma_irq_disable(chip);
	phytium_chan_irq_disable(chan, mask);
	phytium_chan_irq_clear(chan, mask);
	phytium_dma_irq_clear(chip, irq_status);

	if ((chan_status & DMA_STAT_ERR)) {
		dev_err(chan2dev(chan), "chan handle err, chan status is %x\n",
				chan_status);
	} else if ((chan_status & DMA_STAT_END) | (chan_status & DMA_STAT_EOP) |
			(chan_status & DMA_STAT_EOC)) {
		phytium_chan_block_xfer_complete_msi(chan);
	} else {
		dev_err(chan2dev(chan), "irq_status = 0x%x, ----unknown reason:%x\n",
				irq_status, chan_status);
	}

	/* Re-enable interrupts */
	phytium_dma_irq_enable(chip);

	spin_unlock_irqrestore(&chan->vc.lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t phytium_dma_interrupt_intx(int irq, void *dev_id)
{
	struct phytium_dma_chip *chip = dev_id;
	struct phytium_dma *dmac = chip->dmac;
	struct phytium_dma_chan *chan;
	u32 irq_status = 0;
	u32 irq_mask = 0;
	u32 chan_status = 0;
	u32 mask = DMA_CTRL_IRQ_END | DMA_CTRL_IRQ_EOP | DMA_CTRL_IRQ_ERR;
	u32 i;

	irq_status = phytium_dma_irq_read(chip);
	irq_status = irq_status & 0xffff;
	irq_mask = phytium_dma_irq_mask_read(chip);

	if (!(irq_status & irq_mask))
		return IRQ_NONE;

	/* Disable DMAC inerrupts. We'll enable them after processing chanels */
	phytium_dma_irq_disable(chip);
	phytium_dma_irq_clear(chip, irq_status);

	/* Poll, clear and process every chanel interrupt status */
	for (i = 0; i < chip->hdata->nr_channels; i++) {
		if (!(irq_status & BIT(i)) && !(irq_status & BIT(i + 8)))
			continue;

		chan = &dmac->chan[i];

		chan_status = phytium_chan_status_read(chan);

		phytium_chan_irq_disable(chan, mask);
		if ((chan_status & DMA_STAT_ERR))
			dev_err(chan2dev(chan), "chan handle err, chan status is %x\n",
					chan_status);
		else if ((chan_status & DMA_STAT_END) | (chan_status & DMA_STAT_EOP) |
				(chan_status & DMA_STAT_EOC))
			phytium_chan_block_xfer_complete_intx(chan);
		else
			dev_err(chan2dev(chan), "irq_status = 0x%x, ----unknown reason:%x\n",
					irq_status, chan_status);

		phytium_chan_irq_set(chan, mask);
	}

	/* Re-enable interrupts */
	phytium_dma_irq_enable(chip);

	return IRQ_HANDLED;
}

struct phytium_dma_hcfg phytium_octopus_dma_hdata = {
	.nr_channels = 4,
	.nr_masters  = 1,
	.m_data_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
	.block_size[0] = 0x2000000,
	.block_size[1] = 0x2000000,
	.priority[0] = 0,
	.priority[1] = 1,
	.axi_rw_burst_len = 0,
	.restrict_axi_burst_len = false,
};

static int phytium_dma_probe(struct phytium_dma_chip *chip)
{
	struct phytium_dma_chan *chan;
	struct pci_dev *pdev = chip->pdev;
	struct phytium_dma *dmac;
	struct phytium_dma_hcfg *hdata;
	u32 i;
	int ret = -1;

	chip->dev = &pdev->dev;
	dmac = devm_kzalloc(chip->dev, sizeof(*dmac), GFP_KERNEL);
	if (!dmac) {
		dev_err(chip->dev, "dmac zalloc failed\n");
		return -ENOMEM;
	}

	hdata = devm_kzalloc(chip->dev, sizeof(*hdata), GFP_KERNEL);
	if (!hdata) {
		dev_err(chip->dev, "hdata zalloc failed\n");
		return -ENOMEM;
	}

	hdata->nr_channels = 4;
	memcpy(hdata, &phytium_octopus_dma_hdata, sizeof(phytium_octopus_dma_hdata));

	chip->dmac = dmac;
	chip->hdata = hdata;

	dmac->chan = devm_kcalloc(chip->dev, hdata->nr_channels,
				sizeof(*dmac->chan), GFP_KERNEL);
	if (!dmac->chan) {
		dev_err(chip->dev, "hdata zalloc failed\n");
		return -ENOMEM;
	}

	/* Set capabilities */
	dma_cap_set(DMA_MEMCPY, dmac->dma.cap_mask);
	dma_cap_set(DMA_SLAVE, dmac->dma.cap_mask);
	dma_cap_set(DMA_PRIVATE, dmac->dma.cap_mask);

	/* DMA capabilities */
	dmac->dma.chancnt = hdata->nr_channels;
	dmac->dma.src_addr_widths = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmac->dma.dst_addr_widths = DMA_SLAVE_BUSWIDTH_4_BYTES;
	dmac->dma.directions = BIT(DMA_MEM_TO_MEM) | BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV) ;
	dmac->dma.residue_granularity = DMA_RESIDUE_GRANULARITY_DESCRIPTOR;

	dmac->dma.dev = chip->dev;
	dmac->dma.device_tx_status = phytium_dma_tx_status;
	dmac->dma.device_issue_pending = phytium_dma_issue_pending;
	dmac->dma.device_terminate_all = phytium_dma_terminate_all;
	dmac->dma.device_synchronize = phytium_dma_synchronize;

	dmac->dma.device_alloc_chan_resources = phytium_dma_alloc_chan_resources;
	dmac->dma.device_free_chan_resources = phytium_dma_free_chan_resources;

	dmac->dma.device_prep_dma_memcpy = phytium_dma_prep_dma_memcpy;
	dmac->dma.device_config = phytium_dma_slave_config;
	dmac->dma.device_prep_slave_sg = phytium_dma_prep_slave_sg;


	INIT_LIST_HEAD(&dmac->dma.channels);
	for (i = 0; i < chip->hdata->nr_channels; i++) {
		chan = &dmac->chan[i];
		chan->chip = chip;
		chan->id = i;
		chan->chan_regs = chip->regs + i * CHAN_REG_LEN;
		chan->is_used = false;
		chan->is_idle = true;
		chan->is_paused = false;

		chan->vc.desc_free = phytium_vchan_desc_put;
		vchan_init(&chan->vc, &dmac->dma);
	}

	/* ----lt add begin--- */
	dmac->dma.filter.map = ft_x100gpu_slave_map;
	dmac->dma.filter.mapcnt = ARRAY_SIZE(ft_x100gpu_slave_map);
	dmac->dma.filter.fn = x100_filter_fn;
	/*----lt add end----*/

	phytium_dma_hw_init(chip);

	ret = dma_async_device_register(&dmac->dma);
	if (ret)
		goto err_pm_disable;

	ret = pci_alloc_irq_vectors(pdev, 4, 4, PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err(&pdev->dev, "fialed to allocate MSI entry");
		goto intx;
	}

	for (i = 0; i < chip->hdata->nr_channels; i++) {
		chan = &dmac->chan[i];
		chan->irq = pci_irq_vector(pdev, i);
		dev_err(&pdev->dev, "desc->irq = 0x%x", chan->irq);

		ret = devm_request_irq(&pdev->dev, chan->irq, phytium_dma_interrupt_msi, 0, dev_name(chan2dev(chan)), chan);
		if (ret) {
			dev_err(&pdev->dev, "request irq faild %d channal with err %d", i, ret);
			goto intx;
		}
	}
	goto irq_done;

intx:
	chip->irq = pdev->irq;
	ret = devm_request_irq(chip->dev, chip->irq, phytium_dma_interrupt_intx,
			IRQF_SHARED, dev_name(chip->dev), chip);
	if (ret) {
		dev_err(&pdev->dev, "no interrupt used\n");
		return ret;
	}

irq_done:
	return 0;

err_pm_disable:
	pm_runtime_disable(chip->dev);

	return ret;
}

static int phytium_dma_remove(struct phytium_dma_chip *chip)
{
	struct phytium_dma *dmac = chip->dmac;
	struct phytium_dma_chan *chan, *_chan;

	list_for_each_entry_safe(chan, _chan, &dmac->dma.channels,
			vc.chan.device_node) {
		list_del(&chan->vc.chan.device_node);
		tasklet_kill(&chan->vc.task);
	}

	dma_async_device_unregister(&dmac->dma);

	return 0;
}

static int phytium_pci_probe(struct pci_dev *pdev, const struct pci_device_id *pid)
{
	struct phytium_dma_chip *chip;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, 1 << 0, pci_name(pdev));
	if (ret) {
		dev_err(&pdev->dev, "I/O memory remapping failed\n");
		return ret;
	}

	pci_set_master(pdev);
	pci_try_set_mwi(pdev);

	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	ret = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64));
	if (ret)
		return ret;

	chip = devm_kzalloc(&pdev->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->pdev = pdev;
	chip->dev = &pdev->dev;
	chip->id = pdev->devfn;
	chip->regs = pcim_iomap_table(pdev)[0] + COMMON_REG_LEN;

	ret = phytium_dma_probe(chip);
	if (ret)
		return ret;

	pci_set_drvdata(pdev, chip);

	dev_dbg(&pdev->dev, "Phytium dma device at 0x%p 0x%llx\n",
		chip->regs, pdev->resource[0].start);
	return 0;
}

static void phytium_pci_remove(struct pci_dev *pdev)
{
	struct phytium_dma_chip *chip = pci_get_drvdata(pdev);
	int ret;

	ret = phytium_dma_remove(chip);
	if (ret)
		dev_warn(&pdev->dev, "can't remove device properly: %d\n", ret);
}

static const struct pci_device_id phytium_pci_id_table[] = {
	{
		vendor: 0x1DB7,
		device : 0xDC3C,
		subvendor : PCI_ANY_ID,
		subdevice : PCI_ANY_ID,
		class : 0,
		class_mask : 0,
		driver_data : (kernel_ulong_t)&phytium_octopus_dma_hdata,
	},
	{ }
};
MODULE_DEVICE_TABLE(pci, phytium_pci_id_table);
#ifdef CONFIG_PM_SLEEP

static int phytium_pci_prepare(struct device *dev)
{
	u32 i;
	struct pci_dev *pci = to_pci_dev(dev);
	struct phytium_dma_chip *chip = pci_get_drvdata(pci);
	struct phytium_dma_chan *chan;

	phytium_dma_hw_init(chip);

	for (i = 0; i < chip->hdata->nr_channels; i++) {
		chan = &chip->dmac->chan[i];
		if (phytium_chan_is_running(chan)) {
			dev_err(chan2dev(chan), "DMA driver would suspend,"
				"DMA client need to wait data transfer done!\n");
			return -1;
		}

	}
	return 0;
};

static int phytium_pci_suspend_late(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct phytium_dma_chip *chip = pci_get_drvdata(pci);

	phytium_dma_hw_init(chip);
	return 0;
};

static int phytium_pci_resume_early(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct phytium_dma_chip *chip = pci_get_drvdata(pci);

	phytium_dma_hw_init(chip);
	return 0;
};

#endif

static const struct dev_pm_ops phytium_pci_dev_pm_ops = {
	.prepare = phytium_pci_prepare,
	SET_LATE_SYSTEM_SLEEP_PM_OPS(phytium_pci_suspend_late,
			phytium_pci_resume_early)
};

static struct pci_driver phytium_pci_driver = {
	.name		= "phytium_dmac_pci",
	.id_table	= phytium_pci_id_table,
	.probe		= phytium_pci_probe,
	.remove		= phytium_pci_remove,
	.driver	= {
		.pm	= &phytium_pci_dev_pm_ops,
	},
};

bool x100_filter_fn(struct dma_chan *chan, void *param)
{
	if (chan->device->dev->driver != &phytium_pci_driver.driver)
		return false;

	return true;
}

module_pci_driver(phytium_pci_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Phytium DMA Controller platform driver");
MODULE_AUTHOR("Zhu mingshuai <zhumingshuai@phytium.com.cn>");
MODULE_AUTHOR("Li Wenxiang <liwenxiang@phytium.com.cn>");
