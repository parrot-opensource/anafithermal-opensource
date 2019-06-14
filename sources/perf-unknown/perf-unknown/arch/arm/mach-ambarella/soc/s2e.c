/*
 * arch/arm/mach-ambarella/soc/s2e.c
 *
 * Author: Cao Rongrong <rrcao@ambarella.com>
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_platform.h>
#include <linux/irqchip.h>
#include <asm/mach/arch.h>
#include <asm/hardware/cache-l2x0.h>
#include <mach/init.h>

static const char * const s2e_dt_board_compat[] = {
	"ambarella,s2e",
	NULL,
};

DT_MACHINE_START(S2E_DT, "Ambarella S2E (Flattened Device Tree)")
	.l2c_aux_val    = L310_AUX_CTRL_DATA_PREFETCH |
			  L310_AUX_CTRL_INSTR_PREFETCH,
	.l2c_aux_mask   = ~0,
	.smp		= smp_ops(ambarella_smp_ops),
	.map_io		= ambarella_map_io,
	.restart	= ambarella_restart_machine,
	.dt_compat	= s2e_dt_board_compat,
MACHINE_END

