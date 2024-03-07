// SPDX-License-Identifier: GPL-2.0
/*
 * Video Timing Controller List
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Author: Anatoliy Klymenko <anatoliy.klymenko@amd.com>
 */

#include "xlnx_vtc.h"

#include <linux/mutex.h>

/**
 * struct xlnx_vtc_list - List of registered Video Timing Controllers
 * @head: Head of the list of registered VTC instances
 * @lock: Mutex protecting the list
 * @initialized: Initialization flag
 */
struct xlnx_vtc_list {
	struct list_head head;
	struct mutex lock;
	bool initialized;
};

static struct xlnx_vtc_list vtc_list;

/**
 * xlnx_vtc_list_init - Initialize VTC list
 *
 * Return 0 on success, or error code otherwise
 */
int xlnx_vtc_list_init(void)
{
	if (!vtc_list.initialized) {
		INIT_LIST_HEAD(&vtc_list.head);
		mutex_init(&vtc_list.lock);
		vtc_list.initialized = true;
	}

	return 0;
}

/**
 * xlnx_vtc_list_fini - Deinitialize VTC list, free resources
 */
void xlnx_vtc_list_fini(void)
{
	if (vtc_list.initialized) {
		mutex_destroy(&vtc_list.lock);
		vtc_list.initialized = false;
	}
}

/**
 * xlnx_vtc_register - Register new VTC instance
 * @vtc: Pointer to VTC interface instance to register
 *
 * Return 0 on success, or error code otherwise
 */
int xlnx_vtc_register(struct xlnx_vtc_iface *vtc)
{
	if (!vtc || !vtc->of_node)
		return -EINVAL;

	if (!vtc_list.initialized)
		return -EFAULT;

	mutex_lock(&vtc_list.lock);
	list_add_tail(&vtc->list, &vtc_list.head);
	mutex_unlock(&vtc_list.lock);

	return 0;
}

/**
 * xlnx_vtc_unregister - Register new VTC instance
 * @vtc: The VTC interface instance
 */
void xlnx_vtc_unregister(struct xlnx_vtc_iface *vtc)
{
	if (!vtc || !vtc_list.initialized)
		return;

	mutex_lock(&vtc_list.lock);
	list_del(&vtc->list);
	mutex_unlock(&vtc_list.lock);
}

/**
 * xlnx_of_find_vtc - Lookup VTC instance by OF node pointer
 * @np: Pointer to VTC device node
 * @vtc: Output vtc instance pointer
 *
 * Return 0 on success, or error code otherwise
 */
int xlnx_of_find_vtc(const struct device_node *np, struct xlnx_vtc_iface **vtc)
{
	struct xlnx_vtc_iface *vtc_pos;
	int ret = -EPROBE_DEFER;

	*vtc = NULL;

	if (!vtc_list.initialized)
		return ret;

	mutex_lock(&vtc_list.lock);
	list_for_each_entry(vtc_pos, &vtc_list.head, list) {
		if (vtc_pos->of_node == np) {
			*vtc = vtc_pos;
			ret = 0;
			break;
		}
	}
	mutex_unlock(&vtc_list.lock);

	return ret;
}

/**
 * xlnx_vtc_iface_enable - Enable VTC
 * @vtc: The VTC
 *
 * Return 0 on success, or error code otherwise
 */
int xlnx_vtc_iface_enable(struct xlnx_vtc_iface *vtc)
{
	if (!vtc || !vtc->enable)
		return -EINVAL;

	return vtc->enable(vtc);
}

/**
 * xlnx_vtc_iface_disable - Disable VTC
 * @vtc: The VTC
 */
void xlnx_vtc_iface_disable(struct xlnx_vtc_iface *vtc)
{
	if (!vtc || !vtc->disable)
		return;

	vtc->disable(vtc);
}

/**
 * xlnx_vtc_iface_set_timing - Program VTC video timing
 * @vtc: The VTC
 * @vm: Video mode to program timing for
 *
 * Return 0 on success, or error code otherwise
 */
int xlnx_vtc_iface_set_timing(struct xlnx_vtc_iface *vtc,
			      struct videomode *vm)
{
	if (!vtc || !vtc->set_timing)
		return -EINVAL;

	return vtc->set_timing(vtc, vm);
}
