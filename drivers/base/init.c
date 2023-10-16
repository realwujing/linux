// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/of.h>

#include "base.h"

/**
 * driver_init - 初始化驱动模型。
 *
 * 调用驱动模型的初始化函数以初始化它们的子系统。在 init/main.c 中的早期阶段调用。
 */
void __init driver_init(void)
{
	/* 这些是核心部分 */
	devtmpfs_init();  // 初始化 devtmpfs
	devices_init();   // 初始化设备
	buses_init();     // 初始化总线
	classes_init();   // 初始化设备类
	firmware_init();  // 初始化固件
	hypervisor_init(); // 初始化虚拟化

	/* 这些也是核心部分，但必须在核心核心部分之后执行。 */
	of_core_init();       // 初始化 Open Firmware 核心
	platform_bus_init();  // 初始化平台总线
	cpu_dev_init();       // 初始化 CPU 设备
	memory_dev_init();    // 初始化内存设备
	container_dev_init();  // 初始化容器设备
}

