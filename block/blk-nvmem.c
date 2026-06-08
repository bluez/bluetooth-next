// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * block device NVMEM provider
 *
 * Copyright (c) 2024 Daniel Golle <daniel@makrotopia.org>
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Useful on devices using a partition on an eMMC for MAC addresses or
 * Wi-Fi calibration EEPROM data.
 */

#include <linux/cleanup.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pagemap.h>
#include <linux/property.h>

#include "blk.h"


/* List of all NVMEM devices */
static LIST_HEAD(nvmem_devices);
static DEFINE_MUTEX(devices_mutex);

struct blk_nvmem {
	struct nvmem_device	*nvmem;
	dev_t			devt;
	struct list_head	list;
};

static int blk_nvmem_reg_read(void *priv, unsigned int from,
			      void *val, size_t bytes)
{
	blk_mode_t mode = BLK_OPEN_READ | BLK_OPEN_RESTRICT_WRITES;
	struct blk_nvmem *bnv = priv;
	size_t bytes_left = bytes;
	struct file *bdev_file;
	loff_t pos = from;
	int ret = 0;

	bdev_file = bdev_file_open_by_dev(bnv->devt, mode, priv, NULL);
	if (!bdev_file)
		return -ENODEV;

	if (IS_ERR(bdev_file))
		return PTR_ERR(bdev_file);

	while (bytes_left) {
		pgoff_t f_index = pos >> PAGE_SHIFT;
		struct folio *folio;
		size_t folio_off;
		size_t to_read;

		folio = read_mapping_folio(bdev_file->f_mapping, f_index, NULL);
		if (IS_ERR(folio)) {
			ret = PTR_ERR(folio);
			goto err_release_bdev;
		}

		folio_off = offset_in_folio(folio, pos);
		to_read = min(bytes_left, folio_size(folio) - folio_off);
		memcpy_from_folio(val, folio, folio_off, to_read);
		pos += to_read;
		bytes_left -= to_read;
		val += to_read;
		folio_put(folio);
	}

err_release_bdev:
	fput(bdev_file);

	return ret;
}

static int blk_nvmem_register(struct device *dev)
{
	struct device_node *child, *np = dev_of_node(dev);
	struct block_device *bdev = dev_to_bdev(dev);
	struct nvmem_config config = {};
	struct blk_nvmem *bnv;

	/* skip devices which do not have a device tree node */
	if (!np)
		return 0;

	/* skip devices without an nvmem layout defined */
	child = of_get_child_by_name(np, "nvmem-layout");
	if (!child)
		return 0;
	of_node_put(child);

	/*
	 * skip block device too large to be represented as NVMEM devices,
	 * the NVMEM reg_read callback uses an unsigned int offset
	 */
	if (bdev_nr_bytes(bdev) > UINT_MAX)
		return -EFBIG;

	bnv = kzalloc_obj(*bnv);
	if (!bnv)
		return -ENOMEM;

	config.id = NVMEM_DEVID_NONE;
	config.dev = &bdev->bd_device;
	config.name = dev_name(&bdev->bd_device);
	config.owner = THIS_MODULE;
	config.priv = bnv;
	config.reg_read = blk_nvmem_reg_read;
	config.size = bdev_nr_bytes(bdev);
	config.word_size = 1;
	config.stride = 1;
	config.read_only = true;
	config.root_only = true;
	config.ignore_wp = true;
	config.of_node = to_of_node(dev->fwnode);

	bnv->devt = bdev->bd_device.devt;
	bnv->nvmem = nvmem_register(&config);
	if (IS_ERR(bnv->nvmem)) {
		dev_err_probe(&bdev->bd_device, PTR_ERR(bnv->nvmem),
			      "Failed to register NVMEM device\n");
		kfree(bnv);
		return PTR_ERR(bnv->nvmem);
	}

	scoped_guard(mutex, &devices_mutex)
		list_add_tail(&bnv->list, &nvmem_devices);

	return 0;
}

static void blk_nvmem_unregister(struct device *dev)
{
	struct blk_nvmem *bnv_c, *bnv_t, *bnv = NULL;

	scoped_guard(mutex, &devices_mutex) {
		list_for_each_entry_safe(bnv_c, bnv_t, &nvmem_devices, list) {
			if (bnv_c->devt == dev_to_bdev(dev)->bd_device.devt) {
				bnv = bnv_c;
				list_del(&bnv->list);
				break;
			}
		}

		if (!bnv)
			return;
	}

	nvmem_unregister(bnv->nvmem);
	kfree(bnv);
}

static struct class_interface blk_nvmem_bus_interface __refdata = {
	.class = &block_class,
	.add_dev = &blk_nvmem_register,
	.remove_dev = &blk_nvmem_unregister,
};

static int __init blk_nvmem_init(void)
{
	int ret;

	ret = class_interface_register(&blk_nvmem_bus_interface);
	if (ret)
		return ret;

	return 0;
}
device_initcall(blk_nvmem_init);
