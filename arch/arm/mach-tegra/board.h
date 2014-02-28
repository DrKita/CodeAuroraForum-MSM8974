/*
 * arch/arm/mach-tegra/board.h
 *
 * Copyright (c) 2013 NVIDIA Corporation. All rights reserved.
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Erik Gilling <konkers@google.com>
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

#ifndef __MACH_TEGRA_BOARD_H
#define __MACH_TEGRA_BOARD_H

#include <linux/types.h>

void __init tegra_map_common_io(void);
void __init tegra_init_irq(void);
int __init tegra_pcie_init(bool init_port0, bool init_port1);

#ifdef CONFIG_DEBUG_FS
int tegra_clk_debugfs_init(void);
#else
static inline int tegra_clk_debugfs_init(void) { return 0; }
#endif

int __init tegra_powergate_init(void);
#ifdef CONFIG_DEBUG_FS
int __init tegra_powergate_debugfs_init(void);
#else
static inline int tegra_powergate_debugfs_init(void) { return 0; }
#endif

int __init harmony_regulator_init(void);
#ifdef CONFIG_TEGRA_PCI
int __init harmony_pcie_init(void);
#else
static inline int harmony_pcie_init(void) { return 0; }
#endif

void __init tegra_paz00_wifikill_init(void);

extern phys_addr_t tegra_fb_start;
extern size_t tegra_fb_size;
extern phys_addr_t tegra_fb2_start;
extern size_t tegra_fb2_size;
extern phys_addr_t tegra_carveout_start;
extern size_t tegra_carveout_size;

#endif
