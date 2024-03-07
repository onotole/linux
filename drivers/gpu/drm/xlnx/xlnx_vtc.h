/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Xilinx DRM VTC header
 *
 *  Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 *  Author: Anatoliy Klymenko <anatoliy.klymenko@amd.com>
 */

#ifndef _XLNX_VTC_H_
#define _XLNX_VTC_H_

#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/of.h>
#include <linux/types.h>
#include <video/videomode.h>

/**
 * struct xlnx_vtc_iface - Video Timing Controller interface
 * @list: VTC list entry
 * @of_node: Device tree node
 * @enable: Enable VTC callback
 * @disable: Disable VTC callback
 * @set_timing: Program VTC timing callback
 */
struct xlnx_vtc_iface {
	struct list_head list;
	struct device_node *of_node;
	int (*enable)(struct xlnx_vtc_iface *vtc);
	void (*disable)(struct xlnx_vtc_iface *vtc);
	int (*set_timing)(struct xlnx_vtc_iface *vtc, struct videomode *vm);
};

#if IS_ENABLED(CONFIG_DRM_XLNX_BRIDGE_VTC)

int xlnx_vtc_iface_enable(struct xlnx_vtc_iface *vtc);
void xlnx_vtc_iface_disable(struct xlnx_vtc_iface *vtc);
int xlnx_vtc_iface_set_timing(struct xlnx_vtc_iface *vtc,
			      struct videomode *vm);

int xlnx_vtc_list_init(void) __init;
void xlnx_vtc_list_fini(void) __exit;

int xlnx_vtc_register(struct xlnx_vtc_iface *vtc);
void xlnx_vtc_unregister(struct xlnx_vtc_iface *vtc);
int xlnx_of_find_vtc(const struct device_node *np,
		     struct xlnx_vtc_iface **vtc);

#else /* CONFIG_DRM_XLNX_BRIDGE_VTC */

static inline int xlnx_vtc_iface_enable(struct xlnx_vtc_iface *vtc)
{
	return vtc ? -ENODEV : 0;
}

static inline xlnx_vtc_iface_disable(struct xlnx_vtc_iface *vtc)
{
}

static inline int xlnx_vtc_iface_set_timing(struct xlnx_vtc_iface *vtc,
					    struct videomode *vm)
{
	return vtc ? -ENODEV : 0;
}

static inline int xlnx_of_find_vtc(const struct device_node *np,
				   struct xlnx_vtc_iface **vtc)
{
	*vtc = NULL;
	return -ENODEV;
}

static inline int xlnx_vtc_list_init(void)
{
	return 0;
}

static inline void xlnx_vtc_list_fini(void)
{
}

static inline int xlnx_vtc_register(struct xlnx_vtc_iface *vtc)
{
	return 0;
}

static inline void xlnx_vtc_unregister(struct xlnx_vtc_iface *vtc)
{
}

static inline int xlnx_of_find_vtc(const struct device_node *np,
				   struct xlnx_vtc_iface **vtc)
{
	*vtc = NULL;
	return -ENODEV;
}

#endif /* CONFIG_DRM_XLNX_BRIDGE_VTC */

#endif /* _XLNX_VTC_H_ */
