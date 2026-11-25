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

#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/nvmem-provider.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "blk.h"

/**
 * struct blk_nvmem - NVMEM registration state of a block device
 * @bdev: block device backing the NVMEM device
 * @nvmem: registered NVMEM device, only accessed from the work items
 * @size: size of @bdev in bytes, sampled when the registration is queued
 * @add_work: work item registering @nvmem
 * @del_work: work item unregistering @nvmem and freeing this structure
 */
struct blk_nvmem {
	struct block_device	*bdev;
	struct nvmem_device	*nvmem;
	loff_t			size;
	struct work_struct	add_work;
	struct work_struct	del_work;
};

/*
 * The queue must stay ordered: it is what guarantees that the removal of a
 * block device completes before the registration of its replacement, which
 * otherwise would clash on the NVMEM device name, e.g. when re-reading a
 * partition table.
 */
static struct workqueue_struct *blk_nvmem_wq;

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

	/* the minor may have been recycled since the NVMEM was registered */
	if (file_bdev(bdev_file) != bdev) {
		bdev_fput(bdev_file);
		return -ENODEV;
	}

	ret = kernel_read(bdev_file, val, bytes, &pos);

	/* yield the claim synchronously, the file release is deferred */
	bdev_fput(bdev_file);

	if (ret < 0)
		return ret;
	if (ret != bytes)
		return -EIO;
	return 0;
}

static void blk_nvmem_add_work(struct work_struct *work)
{
	struct blk_nvmem *bn = container_of(work, struct blk_nvmem, add_work);
	struct block_device *bdev = bn->bdev;
	struct device *dev = &bdev->bd_device;
	struct nvmem_config config = {
		.id		= NVMEM_DEVID_NONE,
		.owner		= THIS_MODULE,
		.word_size	= 1,
		.stride		= 1,
		.read_only	= true,
		.root_only	= true,
		.ignore_wp	= true,
		.dev		= dev,
		.name		= dev_name(dev),
		.of_node	= dev_of_node(dev),
		.priv		= bdev,
		.reg_read	= blk_nvmem_reg_read,
		.size		= bn->size,
	};
	struct nvmem_device *nvmem;

	nvmem = nvmem_register(&config);
	if (IS_ERR(nvmem)) {
		dev_err_probe(dev, PTR_ERR(nvmem),
			      "Failed to register NVMEM device\n");
		return;
	}

	bn->nvmem = nvmem;
}

static void blk_nvmem_del_work(struct work_struct *work)
{
	struct blk_nvmem *bn = container_of(work, struct blk_nvmem, del_work);

	nvmem_unregister(bn->nvmem);
	put_device(&bn->bdev->bd_device);
	kfree(bn);
}

void blk_nvmem_add(struct block_device *bdev)
{
	struct device *dev = &bdev->bd_device;
	struct device_node *np = dev_of_node(dev);
	struct blk_nvmem *bn;
	struct device_node *child;
	loff_t size;

	/* skip devices which do not have a device tree node */
	if (!np)
		return;

	/*
	 * The layout is described either by an "nvmem-layout" child node or
	 * by the device node itself being a "fixed-layout" container.
	 */
	child = of_get_child_by_name(np, "nvmem-layout");
	if (child)
		of_node_put(child);
	else if (!of_device_is_compatible(np, "fixed-layout"))
		return;

	/*
	 * Sample the size here so that the value validated against the
	 * nvmem_config.size type is the one actually handed to the NVMEM core,
	 * even if the device is resized before the registration runs.
	 */
	size = bdev_nr_bytes(bdev);
	if (size > INT_MAX) {
		dev_warn(dev, "block device too large to be an NVMEM\n");
		return;
	}

	if (!blk_nvmem_wq)
		return;

	bn = kzalloc_obj(*bn);
	if (!bn)
		return;

	bn->bdev = bdev;
	bn->size = size;
	INIT_WORK(&bn->add_work, blk_nvmem_add_work);
	INIT_WORK(&bn->del_work, blk_nvmem_del_work);

	get_device(dev);
	bdev->bd_nvmem = bn;

	queue_work(blk_nvmem_wq, &bn->add_work);
}

void blk_nvmem_del(struct block_device *bdev)
{
	struct blk_nvmem *bn = bdev->bd_nvmem;

	if (!bn)
		return;

	bdev->bd_nvmem = NULL;

	queue_work(blk_nvmem_wq, &bn->del_work);
}

static int __init blk_nvmem_init(void)
{
	blk_nvmem_wq = alloc_ordered_workqueue("blk_nvmem", 0);
	if (!blk_nvmem_wq)
		return -ENOMEM;

	return 0;
}
subsys_initcall(blk_nvmem_init);
