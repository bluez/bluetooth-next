// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * block device NVMEM provider
 *
 * Copyright (c) 2024 Daniel Golle <daniel@makrotopia.org>
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * Useful on devices using a whole disk or a partition (e.g. an eMMC boot
 * partition) to store MAC addresses, Bluetooth addresses or Wi-Fi
 * calibration EEPROM data.
 *
 * The NVMEM device is a side channel onto a block device that stays fully
 * usable. This is somewhat mitigated by opening the device exclusively.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/nvmem-provider.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/property.h>

#include "blk.h"

static int blk_nvmem_reg_read(void *priv, unsigned int from,
			      void *val, size_t bytes)
{
	struct block_device *bdev = priv;
	struct file *bdev_file;
	loff_t pos = from;
	ssize_t ret;

	/* open and prevent other exclusive openers */
	bdev_file = bdev_file_open_by_dev(bdev->bd_dev,
					  BLK_OPEN_READ | BLK_OPEN_EXCL,
					  blk_nvmem_reg_read, NULL);
	if (IS_ERR(bdev_file))
		return PTR_ERR(bdev_file);

	ret = kernel_read(bdev_file, val, bytes, &pos);
	if (ret >= 0 && ret != bytes)
		ret = -EIO;

	fput(bdev_file);

	return ret < 0 ? ret : 0;
}

int blk_nvmem_add(struct block_device *bdev)
{
	struct device *dev = &bdev->bd_device;
	struct device_node *np = dev_of_node(dev);
	struct nvmem_config config = {};
	struct nvmem_device *nvmem;

	/* skip devices which do not have a device tree node */
	if (!np)
		return 0;

	/*
	 * The layout is described either by an "nvmem-layout" child node or
	 * by the device node itself being a "fixed-layout" container.
	 */
	struct device_node *child __free(device_node) =
			of_get_child_by_name(np, "nvmem-layout");
	if (!child && !of_device_is_compatible(np, "fixed-layout"))
		return 0;

	if (bdev_nr_bytes(bdev) > INT_MAX) {
		dev_warn(dev, "block device too large to be an NVMEM\n");
		return 0;
	}

	config.id = NVMEM_DEVID_NONE;
	config.dev = dev;
	config.name = dev_name(dev);
	config.owner = THIS_MODULE;
	config.priv = bdev;
	config.reg_read = blk_nvmem_reg_read;
	config.size = bdev_nr_bytes(bdev);
	config.word_size = 1;
	config.stride = 1;
	config.read_only = true;
	config.root_only = true;
	config.ignore_wp = true;
	config.of_node = np;

	nvmem = nvmem_register(&config);
	if (IS_ERR(nvmem))
		return dev_err_probe(dev, PTR_ERR(nvmem),
				     "Failed to register NVMEM device\n");

	bdev->bd_nvmem = nvmem;

	return 0;
}

void blk_nvmem_del(struct block_device *bdev)
{
	nvmem_unregister(bdev->bd_nvmem);
	bdev->bd_nvmem = NULL;
}
