/**
 * system/src/bld/spi_nor.c
 *
 * Flash controller functions with spi nor  chips.
 *
 * History:
 *    2013/10/12 - [cddiao] creat
 *
 * Copyright 2008-2015 Ambarella Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "spinor.h"

struct spinorcmd_s {
	int (*init)(void);
	int (*erase_sector)(u32 sector);
	int (*write_sector)(u32 sector, u8 *src);
	int (*write_bst)(u32 sector, u8 *src, u32 *page_size);
	int (*read)(u32 addr, u32 data_len, const u8 *dst);
} spinorcmd;

int ambspi_send_cmd(struct amb_norflash *flash, u32 cmd, u32 dummy_len,
	u32 data, u32 len)
{
	u32 tmp = 0;

	tmp = readl_relaxed(flash->regbase + REG00);
	REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, 0);
	REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, dummy_len);
	REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, len);
	REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG00);

	tmp = readl_relaxed(flash->regbase + REG04);
	REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 1);
	REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 0);
	REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 0);
	REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
	REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
	REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
	REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG04);

	tmp = readl_relaxed(flash->regbase + REG0C);
	REGPREP(tmp, REG0C_CMD0_MASK, REG0C_CMD0_SHIFT, cmd);
	writel_relaxed(tmp, flash->regbase + REG0C);

	if (len) {
		tmp = data;
		writel_relaxed(tmp, flash->regbase + REG14);
	}

	tmp = 0x0;
	writel_relaxed(tmp, flash->regbase + REG30);

	tmp = 0x7f;
	writel_relaxed(tmp, flash->regbase + REG3C);

	tmp = readl_relaxed(flash->regbase + REG50);
	REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG50);

	for (;;) {
		tmp = readl_relaxed(flash->regbase + REG38);
		if (REGDUMP(tmp, REG38_DATALENREACHINTR_MASK,
			REG38_DATALENREACHINTR_SHIFT) == 1) {
			return 0;
		}
	}

    return -1;
}
EXPORT_SYMBOL_GPL(ambspi_send_cmd);

int ambspi_read_reg(struct amb_norflash *flash, u32 datalen, u32 reg, u8 *value)
{
	u32 tmp = 0;
	int i;

	tmp = readl_relaxed(flash->regbase + REG28);
	for (; REGDUMP(tmp, REG28_RXFIFOLV_MASK, REG28_RXFIFOLV_SHIFT) != 0; ) {
		readb_relaxed(flash->regbase + REG200);
		tmp = readl_relaxed(flash->regbase + REG28);
	}

	tmp = readl_relaxed(flash->regbase + REG00);
	REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
	REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, 0);
	REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, 0);
	REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, datalen);
	writel_relaxed(tmp, flash->regbase + REG00);

	tmp = readl_relaxed(flash->regbase + REG04);
	REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 1);
	REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 0);
	REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 1);
	REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 1);
	REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
	REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
	REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
	REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG04);

	tmp = reg;
	writel_relaxed(tmp, flash->regbase + REG0C);

	tmp = 0x0;
	writel_relaxed(tmp, flash->regbase + REG10);
	tmp = 0x0;
	writel_relaxed(tmp, flash->regbase + REG14);

	tmp = 0;
	REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG50);

	for (;;) {
		tmp = readl_relaxed(flash->regbase + REG28);
		if (REGDUMP(tmp, REG28_RXFIFOLV_MASK,
			REG28_RXFIFOLV_SHIFT) == datalen) {
			break;
		}
	}

	for (i = 0; i < datalen; i++) {
		*(value + i) = readb_relaxed(flash->regbase + REG200);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ambspi_read_reg);

int ambspi_dma_config(struct amb_norflash *flash)
{
	flash->dmabuf = dma_alloc_coherent(flash->dev, AMBA_SPINOR_DMA_BUFF_SIZE,
				&flash->dmaaddr, GFP_KERNEL);

	if (flash->dmabuf == NULL) {
		dev_err(flash->dev, "dma_alloc_coherent failed!\n");
		return -ENOMEM;
	}

	return 0;
}

int ambspi_progdata_dma(struct amb_norflash *flash, u32 srcoffset,
	u32 dst, u32 len)
{
	int done;
	u32 tmp = 0;

	// DMA_ch0_control
	writel_relaxed(0x1a800000, flash->dmaregbase + 0x10);
	writel_relaxed(0x0, flash->dmaregbase + 0x1c);
	// DMA_ch0_src_addr
	writel_relaxed(flash->dmaaddr + srcoffset, flash->dmaregbase + 0x14);
	// DMA_ch0_dest_addr
	writel_relaxed(flash->reg_phy_base + REG100, flash->dmaregbase + 0x18);
	// DMA_ch0_control
	writel_relaxed(0x9a800000 | len, flash->dmaregbase + 0x10);

	tmp = readl_relaxed(flash->regbase + REG00);
	REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, len);
	REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, 0);
	REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, flash->addr_width);
	REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG00);

	tmp = readl_relaxed(flash->regbase + REG04);
	REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 1);
	REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 0);
	REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 0);
	REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
	REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
	REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
	REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG04);

	tmp = flash->command[0];
	writel_relaxed(tmp, flash->regbase + REG0C);

	tmp = 0;
	REGPREP(tmp, REG30_DATAREACH_MASK, REG30_DATAREACH_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG30);

	tmp = dst;
	writel_relaxed(tmp, flash->regbase + REG14);

	tmp = 0;
	REGPREP(tmp, REG1C_TXFIFOLV_MASK, REG1C_TXFIFOLV_SHIFT, (256-32));
	writel_relaxed(tmp, flash->regbase + REG1C);

	tmp = readl_relaxed(flash->regbase + REG18);
	REGPREP(tmp, REG18_TXDMAEN_MASK, REG18_TXDMAEN_SHIFT, 1);
	REGPREP(tmp, REG18_RXDMAEN_MASK, REG18_RXDMAEN_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG18);

	do {
		tmp = readl_relaxed(flash->regbase + REG2C);
		done = REGDUMP(tmp, REG2C_TXFIFOEMP_MASK, REG2C_TXFIFOEMP_SHIFT);
	} while (done != 0x0);

	tmp = 0x7f;
	writel_relaxed(tmp, flash->regbase + REG3C);

	tmp = readl_relaxed(flash->regbase + REG50);
	REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG50);

	for (;;) {
		tmp = readl_relaxed(flash->regbase + REG38);
		if (REGDUMP(tmp, REG38_DATALENREACHINTR_MASK,
			REG38_DATALENREACHINTR_SHIFT) == 1) {
			return 0;
		}
	}

	return -1;
}

int ambspi_prog_data(struct amb_norflash *flash, u32 srcoffset, u32 dst, u32 len)
{
	u32 tmp = 0;
	int ret = 0;
	u32 tail = 0, bulk = 0, i = 0;

	tail = len % 32;
	bulk = len / 32;

	if (bulk >= 1 ) {
		ret = ambspi_progdata_dma(flash, srcoffset, dst, len-tail);
	}

	if (ret) {
		return ret;
	}

	if (tail) {
		if(bulk >= 1) {
			flash->wait_till_ready(flash);
			flash->write_enable(flash);
		}

		tmp = readl_relaxed(flash->regbase + REG00);
		REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, tail);
		REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, 0);
		REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, flash->addr_width);
		REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
		writel_relaxed(tmp, flash->regbase + REG00);

		tmp = readl_relaxed(flash->regbase + REG04);
		REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 1);
		REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 0);
		REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 0);
		REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
		REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
		REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
		REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
		writel_relaxed(tmp, flash->regbase + REG04);

		tmp = flash->command[0];
		writel_relaxed(tmp, flash->regbase + REG0C);

		tmp = 0;
		REGPREP(tmp, REG30_DATAREACH_MASK, REG30_DATAREACH_SHIFT, 1);
		writel_relaxed(tmp, flash->regbase + REG30);

		tmp = dst + bulk*32;
		writel_relaxed(tmp, flash->regbase + REG14);

		tmp = 0;
		REGPREP(tmp, REG1C_TXFIFOLV_MASK, REG1C_TXFIFOLV_SHIFT, (256-32));
		writel_relaxed(tmp, flash->regbase + REG1C);

		tmp = readl_relaxed(flash->regbase + REG18);
		REGPREP(tmp, REG18_TXDMAEN_MASK, REG18_TXDMAEN_SHIFT, 0);
		REGPREP(tmp, REG18_RXDMAEN_MASK, REG18_RXDMAEN_SHIFT, 0);
		writel_relaxed(tmp, flash->regbase + REG18);

		for (i = 0; i < tail; i++) {
			writeb_relaxed(*(flash->dmabuf + srcoffset + bulk * 32 + i),
				flash->regbase + REG100);
		}

		tmp = 0x7f;
		writel_relaxed(tmp, flash->regbase + REG3C);

		tmp = readl_relaxed(flash->regbase + REG50);
		REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
		writel_relaxed(tmp, flash->regbase + REG50);

		for (;;) {
			tmp = readl_relaxed(flash->regbase + REG24);
			if(REGDUMP(tmp, REG24_TXFIFOLV_MASK,
				REG24_TXFIFOLV_SHIFT) == 0){
				return 0;
			}
			udelay(100);	//must delay
		}
	}

	return -1;
}
EXPORT_SYMBOL_GPL(ambspi_prog_data);

int ambspi_readdata_dma(struct amb_norflash *flash, u32 from,
                            u32 len)
{
	u32 tmp = 0;

	// DMA_ch1_control
	writel_relaxed(0x2a800000, flash->dmaregbase + 0x0);
	writel_relaxed(0x0, flash->dmaregbase + 0xc);
	// DMA_ch1_src_addr
	writel_relaxed(flash->reg_phy_base + REG200, flash->dmaregbase + 0x4);
	// DMA_ch1_dest_addr
	writel_relaxed(flash->dmaaddr, flash->dmaregbase + 0x8);
	// DMA_ch1_control
	writel_relaxed(0xaa800000 | len, flash->dmaregbase + 0x0);

	tmp = readl_relaxed(flash->regbase + REG00);
	REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, len);
	REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, flash->dummy);
	REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, flash->addr_width);
	REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG00);

	tmp = readl_relaxed(flash->regbase + REG04);
	REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 1);
	REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 0);

	if (flash->read_opcode == 0xbb) {
		REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 1);
		REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 1);
		REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 0);
	} else if (flash->read_opcode == 0xeb) {
		REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 2);
		REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 2);
		REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 0);
	} else if (flash->read_opcode == 0x6b) {
		REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 2);
		REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
		REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 0);
	} else {
		REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 1);
		REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
		REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 1);
	}

	REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
	REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
	REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG04);

	tmp = flash->command[0];
	writel_relaxed(tmp, flash->regbase + REG0C);

	tmp = from;
	writel_relaxed(tmp, flash->regbase + REG14);

	tmp = 0x7f;
	writel_relaxed(tmp, flash->regbase + REG3C);

	tmp = (32 - 1);		// must use word.can't use rxfifothlv
	writel_relaxed(tmp, flash->regbase + REG20);

	tmp = readl_relaxed(flash->regbase + REG18);
	REGPREP(tmp, REG18_RXDMAEN_MASK, REG18_RXDMAEN_SHIFT, 1);
	REGPREP(tmp, REG18_TXDMAEN_MASK, REG18_TXDMAEN_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG18);

	tmp = readl_relaxed(flash->regbase + REG50);
	REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG50);

	for (;;) {
		tmp = readl_relaxed(flash->regbase + REG38);
		if(REGDUMP(tmp, REG38_DATALENREACHINTR_MASK,
			REG38_DATALENREACHINTR_SHIFT) == 1) {
			return 0;
		}

		udelay(10);
	}

	return -1;
}

int ambspi_read_data(struct amb_norflash *flash, u32 from, u32 len)
{

	u32 tmp = 0;
	int ret = 0;
	u32 tail = 0, bulk = 0, i = 0;
	tail = len % 32;
	bulk = len / 32;

	if (bulk >= 1){
		ret = ambspi_readdata_dma(flash, from, len-tail);
	}

	if (ret){
		return ret;
	}

	if (tail){
		tmp = readl_relaxed(flash->regbase + REG00);
		REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, tail);
		REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, flash->dummy);
		REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, flash->addr_width);
		REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
		writel_relaxed(tmp, flash->regbase + REG00);

		tmp = readl_relaxed(flash->regbase + REG04);
		REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 1);
		REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 0);

		if (flash->read_opcode == 0xbb) {
			REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 1);
			REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 1);
			REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 0);
		} else if (flash->read_opcode == 0xeb) {
			REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 2);
			REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 2);
			REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 0);
		} else {
			REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 1);
			REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
			REGPREP(tmp, REG04_RXLANE_MASK, REG04_RXLANE_SHIFT, 1);
		}

		REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
		REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
		REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
		writel_relaxed(tmp, flash->regbase + REG04);

		tmp = flash->command[0];
		writel_relaxed(tmp, flash->regbase + REG0C);

		tmp = from + bulk * 32;
		writel_relaxed(tmp, flash->regbase + REG14);

		tmp = 0x7f;
		writel_relaxed(tmp, flash->regbase + REG3C);

		tmp = 31;	// the reserved bits must set 0
		writel_relaxed(tmp, flash->regbase + REG20);

		tmp = readl_relaxed(flash->regbase + REG18);
		REGPREP(tmp, REG18_RXDMAEN_MASK, REG18_RXDMAEN_SHIFT, 0);
		REGPREP(tmp, REG18_TXDMAEN_MASK, REG18_TXDMAEN_SHIFT, 0);
		writel_relaxed(tmp, flash->regbase + REG18);

		tmp = readl_relaxed(flash->regbase + REG50);
		REGPREP(tmp, REG50_STRTRX_MASK, REG50_STRTRX_SHIFT, 1);
		writel_relaxed(tmp, flash->regbase + REG50);

		for (;;) {
			tmp = readl_relaxed(flash->regbase + REG28);
			if (REGDUMP(tmp, REG28_RXFIFOLV_MASK,
				REG28_RXFIFOLV_SHIFT) == tail) {
				break;
			}
		}

		for (i = 0; i < tail; i++) {
			*(flash->dmabuf + bulk * 32 + i) =
				readb_relaxed(flash->regbase + REG200);
		}

		ret = 0;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(ambspi_read_data);

#if !defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
static u32 get_ssi3_freq_hz(void)
{
#define PLL_OUT_ENET   300000000
	u32 val;

	val = amba_rct_readl(CG_SSI3_REG);

#if defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	val &= 0xffffff;

	return (clk_get_rate(clk_get(NULL, "gclk_core")) << 1) / val;
#else
	if (val & 0x01000000)
		return 0;

	if (val == 0)
		val = 1;

	//return (clk_get_rate(clk_get(NULL, "gclk_core")) << 1) / val;
	return (PLL_OUT_ENET) / val;
#endif
}
#endif

int ambspi_init(struct amb_norflash *flash)
{
	u32 tmp = 0;

#if !defined(CONFIG_ARCH_AMBARELLA_AMBALINK)
	u32 divider;

	/* RTOS will help us to do the clock/delay related setting. */
	divider = get_ssi3_freq_hz() / flash->clk;
	tmp = readl_relaxed(flash->regbase + REG08);
	REGPREP(tmp, REG08_CHIPSEL_MASK, REG08_CHIPSEL_SHIFT, ~(1 << SPINOR_DEV));
	REGPREP(tmp, REG08_CLKDIV_MASK, REG08_CLKDIV_SHIFT, divider);
	REGPREP(tmp, REG08_FLOWCON_MASK, REG08_FLOWCON_SHIFT, 1);
	REGPREP(tmp, REG08_HOLDPIN_MASK, REG08_HOLDPIN_SHIFT, 3);
	writel_relaxed(tmp, flash->regbase + REG08);
#endif

	tmp = readl_relaxed(flash->regbase + REG00);
	REGPREP(tmp, REG00_DATALEN_MASK, REG00_DATALEN_SHIFT, 0);
	REGPREP(tmp, REG00_DUMMYLEN_MASK, REG00_DUMMYLEN_SHIFT, 0);
	REGPREP(tmp, REG00_ADDRLEN_MASK, REG00_ADDRLEN_SHIFT, 0);
	REGPREP(tmp, REG00_CMDLEN_MASK, REG00_CMDLEN_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG00);

	tmp = readl_relaxed(flash->regbase + REG04);
	REGPREP(tmp, REG04_WRITEEN_MASK, REG04_WRITEEN_SHIFT, 0);
	REGPREP(tmp, REG04_READEN_MASK, REG04_READEN_SHIFT, 0);
	REGPREP(tmp, REG04_DATALANE_MASK, REG04_DATALANE_SHIFT, 0);
	REGPREP(tmp, REG04_ADDRLANE_MASK, REG04_ADDRLANE_SHIFT, 0);
	REGPREP(tmp, REG04_CMDLANE_MASK, REG04_CMDLANE_SHIFT, 0);
	REGPREP(tmp, REG04_LSBFRT_MASK, REG04_LSBFRT_SHIFT, 0);
	REGPREP(tmp, REG04_CMDDTR_MASK, REG04_CMDDTR_SHIFT, 0);
	writel_relaxed(tmp, flash->regbase + REG04);

	tmp = 0x20;
	writel_relaxed(tmp, flash->regbase + REG30);

	tmp = 0;
	REGPREP(tmp, REG40_TXFIFORESET_MASK, REG40_TXFIFORESET_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG40);

	tmp = 0;
	REGPREP(tmp, REG44_RXFIFORESET_MASK, REG44_RXFIFORESET_SHIFT, 1);
	writel_relaxed(tmp, flash->regbase + REG44);

	/*
	 * after reset fifo, the 0x28 will become 0x10,
	 * so , read REG200 times to clear the 0x28,  this is a bug in hardware
	 */
	while (readl_relaxed(flash->regbase + REG28) != 0) {
		tmp = readl_relaxed(flash->regbase + REG200);
	}

	tmp = 0;
	REGPREP(tmp, REG1C_TXFIFOLV_MASK, REG1C_TXFIFOLV_SHIFT, 0x7f);
	writel_relaxed(tmp, flash->regbase + REG1C);

	tmp = 0;
	REGPREP(tmp, REG20_RXFIFOLV_MASK, REG20_RXFIFOLV_SHIFT, 0x7f);
	writel_relaxed(tmp, flash->regbase + REG20);

	return 0;
}
EXPORT_SYMBOL_GPL(ambspi_init);
