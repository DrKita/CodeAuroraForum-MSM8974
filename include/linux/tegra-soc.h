/*
 * Copyright (c) 2012,2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __LINUX_TEGRA_SOC_H_
#define __LINUX_TEGRA_SOC_H_

extern int tegra_sku_id;

u32 tegra_read_chipid(void);
int tegra_get_cpu_process_id(void);
int tegra_get_core_process_id(void);
int tegra_get_gpu_process_id(void);
int tegra_get_cpu_speedo_id(void);
int tegra_get_soc_speedo_id(void);
int tegra_get_gpu_speedo_id(void);
int tegra_get_cpu_speedo_value(void);
int tegra_get_gpu_speedo_value(void);
int tegra_get_cpu_iddq_value(void);

#ifdef CONFIG_ARM_TEGRA_CPUFREQ
int tegra_cpufreq_init(void);
#else
static inline int tegra_cpufreq_init(void)
{
	return -EINVAL;
}
#endif

#endif /* __LINUX_TEGRA_SOC_H_ */
