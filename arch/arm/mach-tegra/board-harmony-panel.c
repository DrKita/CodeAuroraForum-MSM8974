/*
 * arch/arm/mach-tegra/board-harmony-panel.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/resource.h>
#include <linux/platform_device.h>
#include <linux/pwm_backlight.h>
#include <linux/nvhost.h>
#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/dc.h>
#include <mach/fb.h>
#include <mach/gpio.h>

#include "devices.h"
#include "gpio-names.h"
#include "board-harmony.h"

static int harmony_backlight_init(struct device *dev)
{
	int ret;

	ret = gpio_request(TEGRA_GPIO_BACKLIGHT_VDD, "backlight vdd");
	if (ret < 0)
		return ret;

	ret = gpio_direction_output(TEGRA_GPIO_BACKLIGHT_VDD, 1);
	if (ret < 0)
		goto free_bl_vdd;
	else
		tegra_gpio_enable(TEGRA_GPIO_BACKLIGHT_VDD);

	ret = gpio_request(TEGRA_GPIO_EN_VDD_PNL, "enable VDD to panel");
	if (ret < 0)
		goto free_bl_vdd;

	ret = gpio_direction_output(TEGRA_GPIO_EN_VDD_PNL, 1);
	if (ret < 0)
		goto free_en_vdd_pnl;
	else
		tegra_gpio_enable(TEGRA_GPIO_EN_VDD_PNL);

	ret = gpio_request(TEGRA_GPIO_BACKLIGHT, "backlight_enb");
	if (ret < 0)
		goto free_en_vdd_pnl;

	ret = gpio_direction_output(TEGRA_GPIO_BACKLIGHT, 1);
	if (ret < 0)
		goto free_bl_enb;
	else
		tegra_gpio_enable(TEGRA_GPIO_BACKLIGHT);

	return ret;

free_bl_enb:
	gpio_free(TEGRA_GPIO_BACKLIGHT);
free_en_vdd_pnl:
	gpio_free(TEGRA_GPIO_EN_VDD_PNL);
free_bl_vdd:
	gpio_free(TEGRA_GPIO_BACKLIGHT_VDD);

	return ret;
};

static void harmony_backlight_exit(struct device *dev)
{
	gpio_set_value(TEGRA_GPIO_BACKLIGHT, 0);
	gpio_free(TEGRA_GPIO_BACKLIGHT);
	tegra_gpio_disable(TEGRA_GPIO_BACKLIGHT);

	gpio_set_value(TEGRA_GPIO_BACKLIGHT_VDD, 0);
	gpio_free(TEGRA_GPIO_BACKLIGHT_VDD);
	tegra_gpio_disable(TEGRA_GPIO_BACKLIGHT_VDD);

	gpio_set_value(TEGRA_GPIO_EN_VDD_PNL, 0);
	gpio_free(TEGRA_GPIO_EN_VDD_PNL);
	tegra_gpio_disable(TEGRA_GPIO_EN_VDD_PNL);
}

static int harmony_backlight_notify(struct device *unused, int brightness)
{
	gpio_set_value(TEGRA_GPIO_BACKLIGHT_VDD, !!brightness);
	gpio_set_value(TEGRA_GPIO_EN_VDD_PNL, !!brightness);
	gpio_set_value(TEGRA_GPIO_BACKLIGHT, !!brightness);
	return brightness;
}

static struct platform_pwm_backlight_data harmony_backlight_data = {
	.pwm_id		= 0,
	.max_brightness	= 255,
	.dft_brightness	= 224,
	.pwm_period_ns	= 5000000,
	.init		= harmony_backlight_init,
	.exit		= harmony_backlight_exit,
	.notify		= harmony_backlight_notify,
};

static struct platform_device harmony_backlight_device = {
	.name	= "pwm-backlight",
	.id	= -1,
	.dev	= {
		.platform_data = &harmony_backlight_data,
	},
};

/* Display Controller */
static struct resource harmony_panel_resources[] = {
	{
		.name   = "irq",
		.start  = INT_DISPLAY_GENERAL,
		.end    = INT_DISPLAY_GENERAL,
		.flags  = IORESOURCE_IRQ,
	},
	{
		.name   = "regs",
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
	{
		.name   = "fbmem",
		.start	= 0x1c012000,
		.end	= 0x1c012000 + 0x500000 - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct tegra_dc_mode harmony_panel_modes[] = {
	{
		.pclk = 79500000,
		.h_ref_to_sync = 4,
		.v_ref_to_sync = 2,
		.h_sync_width = 136,
		.v_sync_width = 4,
		.h_back_porch = 138,
		.v_back_porch = 21,
		.h_active = 1024,
		.v_active = 600,
		.h_front_porch = 34,
		.v_front_porch = 4,
	},
};

static struct tegra_fb_data harmony_fb_data = {
	.win            = 0,
	.xres           = 1024,
	.yres           = 600,
	.bits_per_pixel = 24,
};

static struct tegra_dc_out harmony_panel_out = {
	.type = TEGRA_DC_OUT_RGB,

	.align = TEGRA_DC_ALIGN_MSB,
	.order = TEGRA_DC_ORDER_RED_BLUE,

	.modes = harmony_panel_modes,
	.n_modes = ARRAY_SIZE(harmony_panel_modes),
};

static struct tegra_dc_platform_data harmony_panel_pdata = {
	.flags       = TEGRA_DC_FLAG_ENABLED,
	.default_out = &harmony_panel_out,
	.fb          = &harmony_fb_data,
};

static struct nvhost_device harmony_panel_device = {
	.name          = "tegradc",
	.id            = 0,
	.resource      = harmony_panel_resources,
	.num_resources = ARRAY_SIZE(harmony_panel_resources),
	.dev = {
		.platform_data = &harmony_panel_pdata,
	},
};

static struct platform_device *harmony_panel_devices[] __initdata = {
	&tegra_pwfm0_device,
	&harmony_backlight_device,
};

int __init harmony_panel_init(void) 
{
	int err;

	tegra_gpio_enable(TEGRA_GPIO_LVDS_SHUTDOWN);

	err = gpio_request(TEGRA_GPIO_LVDS_SHUTDOWN, "lvds shutdown");
	if (err < 0) {
		pr_err("could not acquire LVDS shutdown GPIO\n");
	} else {
		gpio_direction_output(TEGRA_GPIO_LVDS_SHUTDOWN, 1);
		gpio_free(TEGRA_GPIO_LVDS_SHUTDOWN);
	}

	err = platform_add_devices(harmony_panel_devices,
				ARRAY_SIZE(harmony_panel_devices));

	if (!err)
		err = nvhost_device_register(&harmony_panel_device);

	return err;
}

