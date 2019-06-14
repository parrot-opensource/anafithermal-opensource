/*
 * arch/arm/plat-ambarella/generic/pm.c
 * Power Management Routines
 * Author: Anthony Ginger <hfjiang@ambarella.com>
 *
 * Copyright (C) 2004-2009, Ambarella, Inc.
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
#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/suspend.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/cpu.h>
#include <linux/power_supply.h>
#include <linux/of.h>
#include <asm/cacheflush.h>
#include <asm/io.h>
#include <asm/suspend.h>
#include <mach/hardware.h>
#include <mach/init.h>
#include <plat/drctl.h>
#include <plat/rtc.h>
#include <plat/rct.h>
#include <plat/fio.h>

#define SREF_MAGIC_PATTERN		0x43525230
#define SREF_CPU_JUMP_ADDR		0x000f1000

static int dram_reset_ctrl = -1;
static int gpio_notify_mcu = -1;

u32 gpio_regbase[] = {GPIO0_BASE, GPIO1_BASE, GPIO2_BASE, GPIO3_BASE,
	GPIO4_BASE, GPIO5_BASE, GPIO6_BASE};

/* ==========================================================================*/
void ambarella_pm_gpiomux(int gpio)
{
	u32 bank, offset;

	bank = PINID_TO_BANK(gpio);
	offset = PINID_TO_OFFSET(gpio);

#if (IOMUX_SUPPORT > 0)
	/* configure the pin as GPIO mode */
	amba_clrbitsl(IOMUX_REG(IOMUX_REG_OFFSET(bank, 0)), 0x1 << offset);
	amba_clrbitsl(IOMUX_REG(IOMUX_REG_OFFSET(bank, 1)), 0x1 << offset);
	amba_clrbitsl(IOMUX_REG(IOMUX_REG_OFFSET(bank, 2)), 0x1 << offset);
	amba_writel(IOMUX_REG(IOMUX_CTRL_SET_OFFSET), 0x1);
	amba_writel(IOMUX_REG(IOMUX_CTRL_SET_OFFSET), 0x0);
#endif

}
void ambarella_pm_gpio_output(int gpio, int value)
{
	u32 bank, offset;

	bank = PINID_TO_BANK(gpio);
	offset = PINID_TO_OFFSET(gpio);

	amba_writel(gpio_regbase[bank] + GPIO_ENABLE_OFFSET, 0xffffffff);
	amba_clrbitsl(gpio_regbase[bank] + GPIO_AFSEL_OFFSET, 0x1 << offset);
	amba_setbitsl(gpio_regbase[bank] + GPIO_MASK_OFFSET, 0x1 << offset);
	amba_setbitsl(gpio_regbase[bank] + GPIO_DIR_OFFSET, 0x1 << offset);

	if (!!value)
		amba_setbitsl(gpio_regbase[bank] + GPIO_DATA_OFFSET, 0x1 << offset);
	else
		amba_clrbitsl(gpio_regbase[bank] + GPIO_DATA_OFFSET, 0x1 << offset);
}

void ambarella_pm_gpio_input(int gpio, int *value)
{
	u32 bank, offset;

	bank = PINID_TO_BANK(gpio);
	offset = PINID_TO_OFFSET(gpio);

	amba_writel(gpio_regbase[bank] + GPIO_ENABLE_OFFSET, 0xffffffff);
	amba_clrbitsl(gpio_regbase[bank] + GPIO_AFSEL_OFFSET, 0x1 << offset);
	amba_setbitsl(gpio_regbase[bank] + GPIO_MASK_OFFSET, 0x1 << offset);
	amba_clrbitsl(gpio_regbase[bank] + GPIO_DIR_OFFSET, 0x1 << offset);

	*value = !!(amba_readl(gpio_regbase[bank] + GPIO_DATA_OFFSET) & 0x1 << offset);
}

/* ==========================================================================*/
void ambarella_power_off(void)
{
	amba_rct_setbitsl(ANA_PWR_REG, ANA_PWR_POWER_DOWN);
}

void ambarella_power_off_prepare(void)
{

}

/* ==========================================================================*/
static void ambarella_disconnect_dram_reset(void)
{

	if (dram_reset_ctrl == -1)
		return;

	ambarella_pm_gpiomux(dram_reset_ctrl);

	ambarella_pm_gpio_output(dram_reset_ctrl, 0);
}

static unsigned long ambarella_notify_mcu_prepare(void)
{
	unsigned long gpio_info;
	u32 bank, offset;

	if (gpio_notify_mcu == -1)
		return 0;

	bank = PINID_TO_BANK(gpio_notify_mcu);
	offset = PINID_TO_OFFSET(gpio_notify_mcu);

	gpio_info = gpio_regbase[bank] | offset;

	/* mux GPIO mode, set output, and pull high 100ms */
	ambarella_pm_gpiomux(gpio_notify_mcu);
	ambarella_pm_gpio_output(gpio_notify_mcu, 1);
	mdelay(100);

	return gpio_info;
}

static void ambarella_set_cpu_jump(int cpu, void *jump_fn)
{
	u32 addr_phys;
	u32 *addr_virt;

	/* must keep consistent with self_refresh.c in bst. */
	addr_phys = get_ambarella_ppm_phys() + SREF_CPU_JUMP_ADDR;
	addr_virt = (u32 *)ambarella_phys_to_virt(addr_phys);
	*addr_virt++ = SREF_MAGIC_PATTERN;
	*addr_virt = virt_to_phys(jump_fn);

	__cpuc_flush_dcache_area(addr_virt, sizeof(u32) * 2);
	outer_clean_range(addr_phys, addr_phys + sizeof(u32) * 2);
}

static int ambarella_cpu_do_idle(unsigned long unused)
{
	cpu_do_idle();
	return 0;
}

static int ambarella_pm_enter_standby(void)
{
	cpu_suspend(0, ambarella_cpu_do_idle);
	return 0;
}

static int ambarella_pm_enter_mem(void)
{
	u32 time_val;
	u32 alarm_val;
	unsigned long arg = 0;
#ifdef CONFIG_AMBARELLA_SREF_FIFO_EXEC
	void *ambarella_suspend_exec = NULL;
#endif

	ambarella_disconnect_dram_reset();

	/* ensure the power for DRAM keeps on when power off PWC */
	amba_writel(RTC_REG(PWC_ENP3_OFFSET), 0x1);
	amba_writel(RTC_REG(PWC_POS0_OFFSET), 0x10);
	amba_writel(RTC_REG(PWC_POS1_OFFSET), 0x10);
	amba_writel(RTC_REG(PWC_POS2_OFFSET), 0x10);
	amba_writel(RTC_REG(PWC_POS3_OFFSET), 0x10);
	amba_setbitsl(RTC_REG(PWC_SET_STATUS_OFFSET), 0x04);

	alarm_val = amba_readl(RTC_REG(RTC_ALAT_READ_OFFSET));
	amba_writel(RTC_REG(RTC_ALAT_WRITE_OFFSET), alarm_val);
	time_val  = amba_readl(RTC_REG(RTC_CURT_READ_OFFSET));
	amba_writel(RTC_REG(RTC_CURT_WRITE_OFFSET), time_val);

	amba_writel(RTC_REG(PWC_RESET_OFFSET), 0x1);
	mdelay(3);
	amba_writel(RTC_REG(PWC_RESET_OFFSET), 0x0);

	ambarella_set_cpu_jump(0, ambarella_cpu_resume);

#ifdef CONFIG_AMBARELLA_SREF_FIFO_EXEC
	ambarella_suspend_exec = ambarella_fio_push(ambarella_optimize_suspend,
			ambarella_optimize_suspend_sz);
#endif

	outer_flush_all();
	outer_disable();

	/* XXX special design for S3L */
	arg = ambarella_notify_mcu_prepare();

#ifdef CONFIG_AMBARELLA_SREF_FIFO_EXEC
	cpu_suspend(arg, ambarella_suspend_exec);
#else
	cpu_suspend(0, ambarella_finish_suspend);
#endif

	outer_resume();

	/* ensure to power off all powers when power off PWC */
	amba_writel(RTC_REG(PWC_ENP3_OFFSET), 0x0);
	amba_clrbitsl(RTC_REG(PWC_SET_STATUS_OFFSET), 0x04);

	alarm_val = amba_readl(RTC_REG(RTC_ALAT_READ_OFFSET));
	amba_writel(RTC_REG(RTC_ALAT_WRITE_OFFSET), alarm_val);
	time_val  = amba_readl(RTC_REG(RTC_CURT_READ_OFFSET));
	amba_writel(RTC_REG(RTC_CURT_WRITE_OFFSET), time_val);

	amba_writel(RTC_REG(PWC_RESET_OFFSET), 0x1);
	while(amba_readl(RTC_REG(PWC_REG_STA_OFFSET)) & 0x04);
	amba_writel(RTC_REG(PWC_RESET_OFFSET), 0x0);

	return 0;
}

static int ambarella_pm_suspend_enter(suspend_state_t state)
{
	int rval = 0;

	switch (state) {
	case PM_SUSPEND_STANDBY:
		rval = ambarella_pm_enter_standby();
		break;
	case PM_SUSPEND_MEM:
		rval = ambarella_pm_enter_mem();
		break;
	case PM_SUSPEND_ON:
	default:
		break;
	}

	return rval;
}

static int ambarella_pm_suspend_valid(suspend_state_t state)
{
	int valid;

	switch (state) {
	case PM_SUSPEND_ON:
	case PM_SUSPEND_STANDBY:
	case PM_SUSPEND_MEM:
		valid = 1;
		break;
	default:
		valid = 0;
		break;
	}

	pr_debug("%s: state[%d]=%d\n", __func__, state, valid);

	return valid;
}

static struct platform_suspend_ops ambarella_pm_suspend_ops = {
	.valid		= ambarella_pm_suspend_valid,
	.enter		= ambarella_pm_suspend_enter,
};

static int __init ambarella_init_pm(void)
{
	pm_power_off = ambarella_power_off;
	pm_power_off_prepare = ambarella_power_off_prepare;

	suspend_set_ops(&ambarella_pm_suspend_ops);

	of_property_read_u32(of_chosen, "ambarella,dram-reset-ctrl", &dram_reset_ctrl);
	of_property_read_u32(of_chosen, "ambarella,gpio-notify-mcu", &gpio_notify_mcu);
	WARN(dram_reset_ctrl >= GPIO_MAX_LINES, "Invalid GPIO: %d\n", dram_reset_ctrl);

	return 0;
}

arch_initcall(ambarella_init_pm);

