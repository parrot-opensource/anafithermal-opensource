/*
 * linux/drivers/spi/spi-ambarella.c
 *
 * History:
 *	2014/08/29 - [Zhenwu Xue]

 *
 * Copyright (C) 2012-2016, Ambarella, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/dmapool.h>
#include <plat/spi.h>
#include <plat/dma.h>
#include <plat/rct.h>

#define	AMBARELLA_SPI_BUF_MAX_LEN			4096
#define	AMBARELLA_SPI_MAX_CS_NUM			8

#define SPI_FIFO_RX_TH (SPI_DATA_FIFO_SIZE_32 - 8)

struct ambarella_spi {
	struct device			*dev;
	struct dma_chan		*tx_dma_chan;
	struct dma_chan		*rx_dma_chan;
	struct spi_transfer		*xfer;
	struct spi_device *spi;
	struct clk			*clk;
	void __iomem			*virt;

	u8 					*tx_dma_buf;
	u8 					*rx_dma_buf;
	dma_addr_t 			tx_dma_phys;
	dma_addr_t 			rx_dma_phys;

	u32					dma_buf_size;
	u32					phys;
	u32					clk_freq;
	u32					dma_used:1;
	u32					msb_first_only:1;
	u32					ridx, widx;
	u32					cspol;
	int					cs_pins[AMBARELLA_SPI_MAX_CS_NUM];
	int					cs_active;
	int					rw;
	int					irq;

	int					tasklet_count;
};

static void ambarella_spi_next_transfer(void *args);

static int ambarella_spi_of_parse(struct platform_device *pdev,
			struct spi_master *master)
{
	struct device_node *np = pdev->dev.of_node;
	struct ambarella_spi *ambspi = spi_master_get_devdata(master);
	int rval;

	ambspi->clk = clk_get(NULL, "gclk_ssi");
	if (IS_ERR(ambspi->clk)) {
		dev_err(&pdev->dev, "Get PLL failed!\n");
		return PTR_ERR(ambspi->clk);
	}

	rval = of_property_read_u32(np, "amb,clk-freq", &ambspi->clk_freq);
	if (rval < 0) {
		dev_err(&pdev->dev, "invalid clk-freq! %d\n", rval);
		return rval;
	}

	return 0;
}

static void ambarella_spi_setup(struct ambarella_spi *bus, struct spi_device *spi, u32 speed_hz)
{
	spi_ctrl0_reg_t cr0;
	u32 ssi_clk, sckdv;

	cr0.w		= 0;
	cr0.s.dfs	= spi->bits_per_word - 1;
	if (spi->mode & SPI_CPHA) {
		cr0.s.scph	= 1;
	} else {
		cr0.s.scph	= 0;
	}
	if (spi->mode & SPI_CPOL) {
		cr0.s.scpol	= 1;
	} else {
		cr0.s.scpol	= 0;
	}
	if (spi->mode & SPI_LOOP) {
		cr0.s.srl = 1;
	} else {
		cr0.s.srl = 0;
	}
	if (spi->mode & SPI_LSB_FIRST) {
		cr0.s.tx_lsb = 1;
		cr0.s.rx_lsb = 1;
	} else {
		cr0.s.tx_lsb = 0;
		cr0.s.rx_lsb = 0;
	}

	cr0.s.hold = 1;
	cr0.s.byte_ws = 0;
	cr0.s.fc_en = 0;
	cr0.s.residue = 1;
	writel_relaxed(cr0.w, bus->virt + SPI_CTRLR0_OFFSET);

	ssi_clk = bus->clk_freq;
	if (!speed_hz)
		speed_hz = spi->max_speed_hz;
	if(speed_hz > ssi_clk / 2) {
	    speed_hz = ssi_clk / 2;
	}
	/* max_speed_hz is default speed or max speed ?
	   In case of per transfert speed, other drivers
	   don't check it is < max speed
	 */
	sckdv = (ssi_clk / speed_hz + 1) & 0xfffe;
	writel_relaxed(sckdv, bus->virt + SPI_BAUDR_OFFSET);
}

static void ambarella_spi_stop(struct ambarella_spi *bus)
{
	/* mask irq */
	writel_relaxed(0, bus->virt + SPI_IMR_OFFSET);

	readl_relaxed(bus->virt + SPI_ICR_OFFSET);
	readl_relaxed(bus->virt + SPI_ISR_OFFSET);

	writel_relaxed(0, bus->virt + SPI_SSIENR_OFFSET);
	writel_relaxed(0, bus->virt + SPI_SER_OFFSET);

	if (bus->dma_used)
		writel_relaxed(0, bus->virt + SPI_DMAC_OFFSET);
}

static void ambarella_spi_enable_irq(struct ambarella_spi *bus, int last)
{
	/* SPI_TXEIS_MASK = 1 : tx empty fifo
	 * 0x10 rx fifo full
	 * 0x20 : muti master contention
	 * 0x200 : transfert done
	 * 0xe : overflow/underflow irq
	 */
	if (last)
		writel_relaxed(0x20|0xe|0x200, bus->virt + SPI_IMR_OFFSET);
	else
		writel_relaxed(0x1|0xe|0x20, bus->virt + SPI_IMR_OFFSET);
}

static void ambarella_spi_prepare_transfer(struct ambarella_spi *bus)
{
	struct spi_transfer *xfer;
	const void *wbuf, *rbuf;
	spi_ctrl0_reg_t cr0;
	u32 len;

	bus->widx = 0;
	bus->ridx = 0;

	xfer = bus->xfer;

	wbuf = xfer->tx_buf;
	rbuf = xfer->rx_buf;
	if (wbuf && !rbuf) {
		bus->rw	= SPI_WRITE_ONLY;
	}
	if (!wbuf && rbuf) {
		bus->rw	= SPI_READ_ONLY;
	}
	if (wbuf && rbuf) {
		bus->rw	= SPI_WRITE_READ;
	}

	cr0.w = readl_relaxed(bus->virt + SPI_CTRLR0_OFFSET);
	cr0.s.tmod = SPI_WRITE_READ;
	writel_relaxed(cr0.w, bus->virt + SPI_CTRLR0_OFFSET);

	writel_relaxed(0, bus->virt + SPI_SSIENR_OFFSET);
	writel_relaxed(0, bus->virt + SPI_SER_OFFSET);
	if (bus->dma_used) {
		if (bus->spi->bits_per_word <= 8) {
			writel_relaxed(8 - 1, bus->virt + SPI_RXFTLR_OFFSET);
		} else {
			writel_relaxed(4 - 1, bus->virt + SPI_RXFTLR_OFFSET);
		}
		writel_relaxed(0x3, bus->virt + SPI_DMAC_OFFSET);
	} else {
		writel_relaxed(0x2, bus->virt + SPI_DMAC_OFFSET);
		/* tx fifo near empty */
		writel_relaxed(8, bus->virt + SPI_TXFTLR_OFFSET);
		/* rx fifo 2 entries */
		writel_relaxed(SPI_FIFO_RX_TH-1, bus->virt + SPI_RXFTLR_OFFSET);
	}

	len = xfer->len;
	if (!bus->dma_used) {
		if (bus->spi->bits_per_word > 8)
			len >>= 1;
	}
	/* number of tx frame to send */
	writel_relaxed(len - 1, bus->virt + SPI_TX_FRAME_COUNT);

	writel_relaxed(1, bus->virt + SPI_SSIENR_OFFSET);
}

static void fill_tx_fifo(struct ambarella_spi *bus, int len)
{
	struct spi_transfer *xfer;
	u32 widx, xfer_len, i;
	void *wbuf;
	u16 tmp;

	xfer = bus->xfer;

	wbuf = (void *)xfer->tx_buf;
	widx = bus->widx;

	switch (bus->rw) {
	case SPI_WRITE_ONLY:
	case SPI_WRITE_READ:
			xfer_len = min_t(int, len, SPI_DATA_FIFO_SIZE_32);
			for(i = 0; i < xfer_len; i++) {
				if (bus->spi->bits_per_word <= 8)
					tmp = ((u8 *)wbuf)[widx++];
				else
					tmp = ((u16 *)wbuf)[widx++];
				writel_relaxed(tmp, bus->virt + SPI_DR_OFFSET);
			}
		break;
	case SPI_READ_ONLY:
			xfer_len = min_t(int, len, SPI_DATA_FIFO_SIZE_32);
			for(i = 0; i < xfer_len; i++) {
				writel_relaxed(SPI_DUMMY_DATA, bus->virt + SPI_DR_OFFSET);
				widx++;
			}
		break;
	default:
		break;
	}

	bus->widx = widx;
}

static void ambarella_spi_start_transfer(struct ambarella_spi *bus)
{
	struct spi_device *spi;
	struct spi_transfer *xfer;
	struct dma_slave_config tx_cfg, rx_cfg;
	struct dma_async_tx_descriptor *txd, *rxd;
	u32 len;

	xfer = bus->xfer;
	spi = bus->spi;

	len = xfer->len;
	if (!bus->dma_used) {
		if (bus->spi->bits_per_word > 8)
			len >>= 1;
	}
	else {
		dma_sync_single_for_cpu(bus->dev, bus->tx_dma_phys, len, DMA_TO_DEVICE);
	}

	switch (bus->rw) {
	case SPI_WRITE_ONLY:
	case SPI_WRITE_READ:
		if (bus->dma_used) {
			memcpy(bus->tx_dma_buf, xfer->tx_buf, len);
		}
		break;
	case SPI_READ_ONLY:
		if (bus->dma_used) {
			memset(bus->tx_dma_buf, 0xFF, len);
		}

		break;
	default:
		break;
	}
	if (!bus->dma_used) {
		fill_tx_fifo(bus, len - bus->widx);
		ambarella_spi_enable_irq(bus, bus->widx == len);
	}

	if (bus->dma_used) {
		/* TX DMA */
		tx_cfg.dst_addr			= bus->phys + SPI_DR_OFFSET;
		if (spi->bits_per_word <= 8) {
			tx_cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_1_BYTE;
		} else {
			tx_cfg.dst_addr_width	= DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
		tx_cfg.dst_maxburst		= 8;
		tx_cfg.direction		= DMA_MEM_TO_DEV;

		BUG_ON(dmaengine_slave_config(bus->tx_dma_chan, &tx_cfg) < 0);

		dma_sync_single_for_device(bus->dev, bus->tx_dma_phys, len, DMA_TO_DEVICE);

		txd = dmaengine_prep_slave_single(bus->tx_dma_chan, bus->tx_dma_phys, len,
			DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		BUG_ON (!txd);

		txd->callback		= NULL;
		txd->callback_param	= NULL;
		dmaengine_submit(txd);

		dma_async_issue_pending(bus->tx_dma_chan);

		/* RX DMA */
		rx_cfg.src_addr			= bus->phys + SPI_DR_OFFSET;
		if (spi->bits_per_word <= 8) {
			rx_cfg.src_addr_width	= DMA_SLAVE_BUSWIDTH_1_BYTE;
		} else {
			rx_cfg.src_addr_width	= DMA_SLAVE_BUSWIDTH_2_BYTES;
		}
		rx_cfg.src_maxburst		= 8;
		rx_cfg.direction		= DMA_DEV_TO_MEM;

		BUG_ON(dmaengine_slave_config(bus->rx_dma_chan, &rx_cfg) < 0);

		rxd = dmaengine_prep_slave_single(bus->rx_dma_chan, bus->rx_dma_phys, len,
			DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		BUG_ON(!rxd);

		rxd->callback		= ambarella_spi_next_transfer;
		rxd->callback_param	= bus;

		dma_sync_single_for_device(bus->dev, bus->rx_dma_phys, len, DMA_FROM_DEVICE);

		dmaengine_submit(rxd);
		dma_async_issue_pending(bus->rx_dma_chan);
	}

	writel_relaxed(1 << spi->chip_select, bus->virt + SPI_SER_OFFSET);
}

static void ambarella_spi_next_transfer(void *args)
{
	struct ambarella_spi	*bus = args;
	struct spi_transfer	*xfer;

	xfer = bus->xfer;

	if (bus->dma_used) {
		switch (bus->rw) {
		case SPI_WRITE_READ:
		case SPI_READ_ONLY:
			dma_sync_single_for_cpu(bus->dev, bus->rx_dma_phys, xfer->len, DMA_FROM_DEVICE);
			memcpy(xfer->rx_buf, bus->rx_dma_buf, xfer->len);
			break;
		default:
			break;
		}
	}

	ambarella_spi_stop(bus);
	spi_finalize_current_transfer(bus->spi->master);
}

static void ambarella_spi_isr_data(struct ambarella_spi *bus, int complete)
{
	struct spi_transfer *xfer;
	void *rbuf;
	u32 ridx, len, rxflr, xfer_len;
	u16 i, tmp;

	xfer = bus->xfer;

	rbuf	= (void *)xfer->rx_buf;
	ridx	= bus->ridx;
	len	= xfer->len;

	if (bus->spi->bits_per_word > 8)
		len >>= 1;

	/* we are trigger by fifo tx irq or complete irq */

	/* first read rx fifo */
	/* Fetch data from FIFO */
	switch (bus->rw) {
	case SPI_READ_ONLY:
	case SPI_WRITE_READ:
		xfer_len = len - ridx;
		rxflr = readl_relaxed(bus->virt + SPI_RXFLR_OFFSET);
		if (xfer_len > rxflr)
			xfer_len = rxflr;
		for(i = 0; i < xfer_len; i++) {
			tmp	= readl_relaxed(bus->virt + SPI_DR_OFFSET);
			if (bus->spi->bits_per_word <= 8)
				((u8 *)rbuf)[ridx++] = tmp & 0xff;
			else
				((u16 *)rbuf)[ridx++] = tmp;
		}
		/* in complete irq, we should have read everything */
		if (complete) {
			if (ridx != len) {
				dev_err(bus->dev,
					"incomplete read %d instead of %d. Status 0x%x\n",
						ridx, len, readl_relaxed(bus->virt + SPI_SR_OFFSET));
			}
		}
		bus->ridx = ridx;
		break;
	default:
		rxflr = 0;
		break;
	}

	/* fill tx fifo */
	if (bus->widx < len) {
		int size;
		int remaining = len - bus->widx;
		if (rxflr) {
			size = rxflr;
			if (size > remaining)
				size = remaining;
		}
		else {
			/* write only case */
			size = remaining;
		}
		/* in complete irq, we should have nothing to send */
		if (complete) {
			dev_err(bus->dev,
					"filling tx fifo in complete irq current=%d total=%d! Status 0x%x\n",
					bus->widx, len, readl_relaxed(bus->virt + SPI_SR_OFFSET));
		}

		fill_tx_fifo(bus, size);
		ambarella_spi_enable_irq(bus, bus->widx == len);
	}
}

static int ambarella_spi_transfer_one(struct spi_master *master,
			      struct spi_device *spi,
			      struct spi_transfer *xfer)
{
	int				err = 0;
	struct ambarella_spi		*bus;
	u32 speed_hz = UINT_MAX;

	bus = spi_master_get_devdata(master);

	if (spi->bits_per_word < 4 || spi->bits_per_word > 16) {
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	if (!spi->max_speed_hz) {
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	if (spi->bits_per_word > 8) {
		if (xfer->len & 0x1) {
			err = -EINVAL;
			goto ambarella_spi_transfer_exit;
		}
	}

	if ((spi->mode & SPI_LSB_FIRST) && bus->msb_first_only) {
		dev_err(&master->dev, "SPI[%d] only supports msb first tx-rx", master->bus_num);
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}

	if (xfer->len > AMBARELLA_SPI_BUF_MAX_LEN) {
		err = -EINVAL;
		goto ambarella_spi_transfer_exit;
	}
	bus->xfer = xfer;
	bus->spi = spi;
	speed_hz = min(speed_hz, xfer->speed_hz);

	ambarella_spi_setup(bus, spi, speed_hz);
	ambarella_spi_prepare_transfer(bus);
	ambarella_spi_start_transfer(bus);

	/* wait completion */
	err = 1;

ambarella_spi_transfer_exit:
	return err;

}


static irqreturn_t ambarella_spi_isr(int irq, void *dev_data)
{
	struct ambarella_spi *bus = dev_data;
	u32 stat = readl_relaxed(bus->virt + SPI_ISR_OFFSET);
	if (stat & 1) {
		/* ack */
		readl_relaxed(bus->virt + SPI_TXOICR_OFFSET);

		ambarella_spi_isr_data(bus, 0);
		stat &= ~1;
	}
	if (stat & 0x200) {
		u32 status;
		/* transfert done */
		/* ack */
		readl_relaxed(bus->virt + 0x28c);

		/* read fifo */
		ambarella_spi_isr_data(bus, 1);

		status = readl_relaxed(bus->virt + SPI_SR_OFFSET);
		if (status & 1)
			dev_err(bus->dev,
				"busy in complete\n");
		/* complete transfert */
		ambarella_spi_next_transfer((void *) bus);

		stat &= ~0x200;
	}
	if (stat != 0) {
		/* clear all irq */
		readl_relaxed(bus->virt + SPI_ICR_OFFSET);
		dev_err(bus->dev,
			"spi bad irq 0x%x\n", stat);
	}

	return IRQ_HANDLED;
}

static int ambarella_spi_dma_channel_allocate(struct ambarella_spi *bus,
			bool dma_to_memory)
{
	struct dma_chan *dma_chan;
	dma_addr_t dma_phys;
	u8 *dma_buf;
	int ret = 0;

	dma_chan = dma_request_slave_channel(bus->dev,
					dma_to_memory ? "rx" : "tx");
	if (IS_ERR(dma_chan)) {
		ret = PTR_ERR(dma_chan);
		if (ret != -EPROBE_DEFER)
			dev_err(bus->dev,
				"Dma channel is not available: %d\n", ret);
		return ret;
	}

	dma_buf = dma_alloc_coherent(bus->dev, bus->dma_buf_size,
				&dma_phys, GFP_KERNEL);
	if (!dma_buf) {
		dev_err(bus->dev, " Not able to allocate the dma buffer\n");
		dma_release_channel(dma_chan);
		return -ENOMEM;
	}

	if (dma_to_memory) {
		bus->rx_dma_chan = dma_chan;
		bus->rx_dma_buf = dma_buf;
		bus->rx_dma_phys = dma_phys;
	} else {
		bus->tx_dma_chan = dma_chan;
		bus->tx_dma_buf = dma_buf;
		bus->tx_dma_phys = dma_phys;
	}

	return ret;
}

static void ambarella_spi_dma_channel_free(struct ambarella_spi *bus,
	bool dma_to_memory)
{
	u8 *dma_buf;
	dma_addr_t dma_phys;
	struct dma_chan *dma_chan;

	if (dma_to_memory) {
		dma_buf = bus->rx_dma_buf;
		dma_chan = bus->rx_dma_chan;
		dma_phys = bus->rx_dma_phys;
		bus->rx_dma_chan = NULL;
		bus->rx_dma_buf = NULL;
	} else {
		dma_buf = bus->tx_dma_buf;
		dma_chan = bus->tx_dma_chan;
		dma_phys = bus->tx_dma_phys;
		bus->tx_dma_buf = NULL;
		bus->tx_dma_chan = NULL;
	}
	if (!dma_chan)
		return;

	dma_free_coherent(bus->dev, bus->dma_buf_size, dma_buf, dma_phys);
	dma_release_channel(dma_chan);
}

static int ambarella_spi_hw_setup(struct spi_device *spi)
{
	struct ambarella_spi *bus = spi_master_get_devdata(spi->master);
	int err = 0;

	if (gpio_is_valid(spi->cs_gpio) &&
		(bus->cs_pins[spi->chip_select] != spi->cs_gpio)) {
		err = gpio_request(spi->cs_gpio, dev_name(&spi->dev));
		if (err < 0) {
			dev_err(&spi->dev, "can't get CS: %d\n", err);
			return -EINVAL;
		}
		gpio_direction_output(spi->cs_gpio, (spi->mode & SPI_CS_HIGH) ? 0 : 1);
		bus->cs_pins[spi->chip_select] = spi->cs_gpio;
	}

	return 0;
}

static ssize_t amba_spi_read_tasket_count(
		struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct spi_master *master = dev_get_drvdata(dev);
	struct ambarella_spi *bus = spi_master_get_devdata(master);

	return sprintf(buf, "%d\n",
			bus->tasklet_count);
}

static DEVICE_ATTR(amba_spi_tasklet, S_IRUSR, amba_spi_read_tasket_count, NULL);

static struct attribute *amba_spi_attributes[] = {
	&dev_attr_amba_spi_tasklet.attr,
	NULL
};

static struct attribute_group dev_attr_group = {
	.attrs = amba_spi_attributes,
};

static int ambarella_spi_probe(struct platform_device *pdev)
{
	struct resource 			*res;
	void __iomem				*reg;
	struct ambarella_spi			*bus;
	struct spi_master			*master;
	int					i;
	int					err = 0;
	int					irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		err = -EINVAL;
		goto exit_spi_probe;
	}

	reg = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!reg) {
		err = -ENOMEM;
		goto exit_spi_probe;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "no irq resource!\n");
		err = -ENODEV;
		goto exit_spi_probe;
	}

	master = spi_alloc_master(&pdev->dev, sizeof(*bus));
	if (!master) {
		err = -ENOMEM;
		goto exit_spi_probe;
	}

	bus = spi_master_get_devdata(master);
	bus->phys = res->start;
	bus->virt = reg;
	bus->irq = irq;
	bus->dev = &pdev->dev;

	for (i = 0; i < AMBARELLA_SPI_MAX_CS_NUM; i++)
		bus->cs_pins[i] = -1;

	err = ambarella_spi_of_parse(pdev, master);
	if (err < 0) {
		goto exit_free_master;
	}

	clk_set_rate(bus->clk, bus->clk_freq);
	writel_relaxed(0, reg + SPI_SSIENR_OFFSET);
	writel_relaxed(0, reg + SPI_IMR_OFFSET);

	master->dev.of_node		= pdev->dev.of_node;
	master->mode_bits		= SPI_CPHA | SPI_CPOL | SPI_CS_HIGH;
	master->transfer_one	= ambarella_spi_transfer_one;
	master->setup = ambarella_spi_hw_setup;
	platform_set_drvdata(pdev, master);

	/* check if hw only supports msb first tx/rx */
	if (of_find_property(pdev->dev.of_node, "amb,msb-first-only", NULL)) {
		bus->msb_first_only = 1;
		dev_info(&pdev->dev,"Only supports msb first tx-rx\n");
	} else {
		bus->msb_first_only = 0;
		master->mode_bits |= SPI_LSB_FIRST;
	}

	/* check if using dma */
	if (of_find_property(pdev->dev.of_node, "amb,dma-used", NULL)) {
		bus->dma_used = 1;
		dev_info(&pdev->dev,"DMA is used\n");

		bus->dma_buf_size = AMBARELLA_SPI_BUF_MAX_LEN;
		err = ambarella_spi_dma_channel_allocate(bus, false);
		if (err < 0)
			goto exit_free_master;
		err = ambarella_spi_dma_channel_allocate(bus, true);
		if (err < 0)
			goto exit_tx_dma_irq_free;
	} else {
		bus->dma_used = 0;
		/* request IRQ */
		err = devm_request_irq(&pdev->dev, irq, ambarella_spi_isr,
				IRQF_TRIGGER_HIGH, dev_name(&pdev->dev), bus);
		if (err)
			goto exit_free_master;

	}

	err = sysfs_create_group(&pdev->dev.kobj, &dev_attr_group);
	if (err) {
		dev_err(&pdev->dev, "Failed to create sysfs group !\n");
		goto exit_rx_dma_free;
	}


	err = spi_register_master(master);
	if (err)
		goto exit_sysfs_free;

	dev_info(&pdev->dev, "Ambarella spi controller %d created.\r\n", master->bus_num);

	return err;

exit_sysfs_free:
	sysfs_remove_group(&pdev->dev.kobj, &dev_attr_group);
exit_rx_dma_free:
	if (bus->dma_used)
		ambarella_spi_dma_channel_free(bus, true);
exit_tx_dma_irq_free:
	if (bus->dma_used)
		ambarella_spi_dma_channel_free(bus, false);
	else
		free_irq(irq, bus);
exit_free_master:
	spi_master_put(master);
exit_spi_probe:
	return err;
}

static int ambarella_spi_remove(struct platform_device *pdev)
{
	struct spi_master		*master;
	struct ambarella_spi		*bus;
	int i;

	master	= platform_get_drvdata(pdev);
	bus	= spi_master_get_devdata(master);

	if (bus->dma_used) {
		if (bus->tx_dma_chan)
			ambarella_spi_dma_channel_free(bus, false);

		if (bus->rx_dma_chan)
			ambarella_spi_dma_channel_free(bus, true);
	}

	ambarella_spi_stop(bus);
	sysfs_remove_group(&pdev->dev.kobj, &dev_attr_group);

	for (i = 0; i < AMBARELLA_SPI_MAX_CS_NUM; i++) {
		if (gpio_is_valid(bus->cs_pins[i]))
			gpio_free(bus->cs_pins[i]);
	}

	spi_unregister_master(master);

	return 0;
}

#if 0
int ambarella_spi_write(amba_spi_cfg_t *spi_cfg, amba_spi_write_t *spi_w)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			spi;
	struct ambarella_spi			*bus;

	master = spi_busnum_to_master(spi_w->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_exit;
	}

	bus = spi_master_get_devdata(master);

	spi.master		= master;
	spi.bits_per_word	= spi_cfg->cfs_dfs;
	spi.mode		= spi_cfg->spi_mode;
	spi.max_speed_hz	= spi_cfg->baud_rate;
	spi.chip_select		= spi_w->cs_id;
	spi.cs_gpio		= bus->cs_pins[spi.chip_select];

	spin_lock_init(&spi.statistics.lock);

	err = spi_write_then_read(&spi, spi_w->buffer, spi_w->n_size, NULL, 0);

ambarella_spi_write_exit:
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write);

int ambarella_spi_read(amba_spi_cfg_t *spi_cfg, amba_spi_read_t *spi_r)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			spi;
	struct ambarella_spi			*bus;

	master = spi_busnum_to_master(spi_r->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi.master		= master;
	spi.bits_per_word	= spi_cfg->cfs_dfs;
	spi.mode		= spi_cfg->spi_mode;
	spi.max_speed_hz	= spi_cfg->baud_rate;
	spi.chip_select		= spi_r->cs_id;
	spi.cs_gpio		= bus->cs_pins[spi.chip_select];

	spin_lock_init(&spi.statistics.lock);

	err = spi_write_then_read(&spi, NULL, 0, spi_r->buffer, spi_r->n_size);

ambarella_spi_read_exit:
	return err;
}
EXPORT_SYMBOL(ambarella_spi_read);

int ambarella_spi_write_then_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_then_read_t *spi_wtr)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			spi;
	struct ambarella_spi			*bus;

	master = spi_busnum_to_master(spi_wtr->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_then_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi.master		= master;
	spi.bits_per_word	= spi_cfg->cfs_dfs;
	spi.mode		= spi_cfg->spi_mode;
	spi.max_speed_hz	= spi_cfg->baud_rate;
	spi.chip_select		= spi_wtr->cs_id;
	spi.cs_gpio		= bus->cs_pins[spi.chip_select];

	spin_lock_init(&spi.statistics.lock);

	err = spi_write_then_read(&spi,
		spi_wtr->w_buffer, spi_wtr->w_size,
		spi_wtr->r_buffer, spi_wtr->r_size);

ambarella_spi_write_then_read_exit:
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write_then_read);

int ambarella_spi_write_and_read(amba_spi_cfg_t *spi_cfg,
	amba_spi_write_and_read_t *spi_war)
{
	int					err = 0;
	struct spi_master			*master;
	struct spi_device			spi;
	struct ambarella_spi			*bus;
	struct spi_message			msg;
	struct spi_transfer			x[1];

	master = spi_busnum_to_master(spi_war->bus_id);
	if (!master) {
		err = -EINVAL;
		goto ambarella_spi_write_and_read_exit;
	}

	bus = spi_master_get_devdata(master);

	spi.master		= master;
	spi.bits_per_word	= spi_cfg->cfs_dfs;
	spi.mode		= spi_cfg->spi_mode;
	spi.max_speed_hz	= spi_cfg->baud_rate;
	spi.chip_select		= spi_war->cs_id;
	spi.cs_gpio		= bus->cs_pins[spi.chip_select];

	spin_lock_init(&spi.statistics.lock);

	spi_message_init(&msg);
	memset(x, 0, sizeof x);
	x->tx_buf	= spi_war->w_buffer;
	x->rx_buf	= spi_war->r_buffer;
	x->len		= spi_war->n_size;
	spi_message_add_tail(x, &msg);

	err = spi_sync(&spi, &msg);

ambarella_spi_write_and_read_exit:
	return err;
}
EXPORT_SYMBOL(ambarella_spi_write_and_read);
#endif

static const struct of_device_id ambarella_spi_dt_ids[] = {
	{.compatible = "ambarella,spi", },
	{},
};
MODULE_DEVICE_TABLE(of, ambarella_spi_dt_ids);

static struct platform_driver ambarella_spi_driver = {
	.probe		= ambarella_spi_probe,
	.remove		= ambarella_spi_remove,
	.driver		= {
		.name	= "ambarella-spi",
		.of_match_table = ambarella_spi_dt_ids,
	},
};

static int ambarella_spi_init(void)
{
	return platform_driver_register(&ambarella_spi_driver);
}

static void ambarella_spi_exit(void)
{
	platform_driver_unregister(&ambarella_spi_driver);
}

module_init(ambarella_spi_init);
module_exit(ambarella_spi_exit);

MODULE_DESCRIPTION("Ambarella Media Processor SPI Bus Controller");
MODULE_AUTHOR("Zhenwu Xue, <zwxue@ambarella.com>");
MODULE_LICENSE("GPL");
