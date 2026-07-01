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
#include <linux/device.h>
#include <linux/file.h>
#include <linux/nvmem-provider.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/pagemap.h>
#include <linux/property.h>

#include "blk.h"

static int blk_nvmem_reg_read(void *priv, unsigned int from, void *val, size_t bytes)
{
	dev_t devt = (dev_t)(uintptr_t)priv;
	size_t bytes_left = bytes;
	loff_t pos = from;
	int ret = 0;

	struct file *bdev_file __free(fput) =
		bdev_file_open_by_dev(devt, BLK_OPEN_READ, NULL, NULL);
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
			break;
		}

		folio_off = offset_in_folio(folio, pos);
		to_read = min(bytes_left, folio_size(folio) - folio_off);
		memcpy_from_folio(val, folio, folio_off, to_read);
		pos += to_read;
		bytes_left -= to_read;
		val += to_read;
		folio_put(folio);
	}

	return ret;
}

int blk_nvmem_add(struct block_device *bdev)
{
	struct device *dev = &bdev->bd_device;
	struct nvmem_config config = {};

	/* skip devices which do not have a device tree node */
	if (!dev_of_node(dev))
		return 0;

	/* skip devices without an nvmem layout defined */
	struct device_node *child __free(device_node) =
		of_get_child_by_name(dev_of_node(dev), "nvmem-layout");
	if (!child && !of_device_is_compatible(dev_of_node(dev), "fixed-layout"))
		return 0;

	/*
	 * skip block device too large to be represented as NVMEM devices,
	 * nvmem_config.size is a signed int
	 */
	if (bdev_nr_bytes(bdev) > INT_MAX) {
		dev_warn(dev, "block device too large to be an NVMEM provider\n");
		return 0;
	}

	config.id = NVMEM_DEVID_NONE;
	config.dev = dev;
	config.name = dev_name(dev);
	config.owner = THIS_MODULE;
	config.priv = (void *)(uintptr_t)dev->devt;
	config.reg_read = blk_nvmem_reg_read;
	config.size = bdev_nr_bytes(bdev);
	config.word_size = 1;
	config.stride = 1;
	config.read_only = true;
	config.root_only = true;
	config.ignore_wp = true;
	config.of_node = to_of_node(dev->fwnode);

	bdev->bd_nvmem = nvmem_register(&config);
	if (IS_ERR(bdev->bd_nvmem))
		return dev_err_probe(dev, PTR_ERR(bdev->bd_nvmem),
				     "Failed to register NVMEM device\n");

	return 0;
}

void blk_nvmem_del(struct block_device *bdev)
{
	nvmem_unregister(bdev->bd_nvmem);
	bdev->bd_nvmem = NULL;
}
