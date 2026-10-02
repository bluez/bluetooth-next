// SPDX-License-Identifier: GPL-2.0
/*
 ******************************************************************************
 *
 * Copyright (C) 2020 AIC semiconductor.
 *
 * @brief AIC8800D80 Bluetooth driver core
 *
 ******************************************************************************
 */

#include <linux/completion.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "btaic.h"

struct aic_bt_boot {
	struct kref ref;
	struct list_head node;
	struct completion ready;
	struct device *dev;
	int status;
	bool present;
};

static DEFINE_MUTEX(aic_bt_boot_lock);
static LIST_HEAD(aic_bt_boot_list);

static void aic_bt_boot_release(struct kref *ref)
{
	struct aic_bt_boot *boot = container_of(ref, struct aic_bt_boot, ref);

	put_device(boot->dev);
	kfree(boot);
}

struct aic_bt_boot *aic_bt_boot_register(struct device *dev)
{
	struct aic_bt_boot *boot;

	boot = kzalloc_obj(*boot);
	if (!boot)
		return ERR_PTR(-ENOMEM);

	kref_init(&boot->ref);
	INIT_LIST_HEAD(&boot->node);
	init_completion(&boot->ready);
	boot->dev = get_device(dev);
	boot->status = -EINPROGRESS;
	boot->present = true;

	mutex_lock(&aic_bt_boot_lock);
	list_add_tail(&boot->node, &aic_bt_boot_list);
	mutex_unlock(&aic_bt_boot_lock);

	return boot;
}

void aic_bt_boot_ready(struct aic_bt_boot *boot, int status)
{
	mutex_lock(&aic_bt_boot_lock);
	if (boot->present)
		boot->status = status;
	mutex_unlock(&aic_bt_boot_lock);

	complete_all(&boot->ready);
}

void aic_bt_boot_unregister(struct aic_bt_boot *boot)
{
	if (!boot)
		return;

	mutex_lock(&aic_bt_boot_lock);
	if (boot->present) {
		list_del_init(&boot->node);
		boot->present = false;
		boot->status = -ENODEV;
	}
	mutex_unlock(&aic_bt_boot_lock);

	complete_all(&boot->ready);
	kref_put(&boot->ref, aic_bt_boot_release);
}

struct aic_bt_boot *aic_bt_boot_get(const struct fwnode_handle *fwnode)
{
	struct aic_bt_boot *boot;
	struct aic_bt_boot *found = NULL;
	unsigned int count = 0;

	mutex_lock(&aic_bt_boot_lock);
	list_for_each_entry(boot, &aic_bt_boot_list, node) {
		if (!boot->present)
			continue;

		if (fwnode) {
			if (dev_fwnode(boot->dev) == fwnode) {
				found = boot;
				break;
			}
			continue;
		}

		found = boot;
		count++;
	}

	if (found && (fwnode || count == 1))
		kref_get(&found->ref);
	else if (count > 1)
		found = ERR_PTR(-EINVAL);
	else
		found = ERR_PTR(-EPROBE_DEFER);
	mutex_unlock(&aic_bt_boot_lock);

	return found;
}

void aic_bt_boot_put(struct aic_bt_boot *boot)
{
	if (!IS_ERR_OR_NULL(boot))
		kref_put(&boot->ref, aic_bt_boot_release);
}

int aic_bt_boot_wait(struct aic_bt_boot *boot, unsigned long timeout)
{
	int status;

	if (!wait_for_completion_timeout(&boot->ready, timeout))
		return -ETIMEDOUT;

	mutex_lock(&aic_bt_boot_lock);
	status = boot->present ? boot->status : -ENODEV;
	mutex_unlock(&aic_bt_boot_lock);

	return status;
}

struct device *aic_bt_boot_device(struct aic_bt_boot *boot)
{
	return boot->dev;
}

static int __init aic_bt_init(void)
{
	int err;

	err = aic_bt_sdio_register();
	if (err)
		return err;

	err = aic_bt_uart_register();
	if (err) {
		aic_bt_sdio_unregister();
		return err;
	}

	return 0;
}

static void __exit aic_bt_exit(void)
{
	aic_bt_uart_unregister();
	aic_bt_sdio_unregister();
}

module_init(aic_bt_init);
module_exit(aic_bt_exit);

MODULE_AUTHOR("AIC Semiconductor");
MODULE_DESCRIPTION("AIC8800D80 Bluetooth driver");
MODULE_LICENSE("GPL");
