// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026 The Linux Foundation. All rights reserved.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/elf.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/soc/qcom/mdt_loader.h>

#include "qcom_common.h"

#define BTSS_PAS_ID	0xc

struct m0_btss {
	struct device *dev;
	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	void __iomem *mem_region;
	size_t mem_size;
	struct reset_control *btss_reset;
};

static int m0_btss_start(struct rproc *rproc)
{
	int ret;

	if (!qcom_scm_pas_supported(BTSS_PAS_ID)) {
		dev_err(rproc->dev.parent,
			"PAS is not available for peripheral: 0x%x\n",
			BTSS_PAS_ID);
		return -ENODEV;
	}

	ret = qcom_scm_pas_auth_and_reset(BTSS_PAS_ID);
	if (ret) {
		dev_err(rproc->dev.parent, "Failed to start rproc: %d\n", ret);
		return ret;
	}

	return 0;
}

static int m0_btss_stop(struct rproc *rproc)
{
	int ret;

	if (rproc->state == RPROC_RUNNING || rproc->state == RPROC_CRASHED) {
		ret = qcom_scm_pas_shutdown(BTSS_PAS_ID);
		if (ret) {
			dev_err(rproc->dev.parent, "Failed to stop rproc: %d\n",
				ret);
			return ret;
		}

		dev_info(rproc->dev.parent, "Successfully stopped rproc\n");
	}

	return 0;
}

static int m0_btss_load(struct rproc *rproc, const struct firmware *fw)
{
	struct m0_btss *desc = rproc->priv;
	const struct elf32_phdr *phdrs;
	const struct firmware *seg_fw;
	const struct elf32_phdr *phdr;
	const struct elf32_hdr *ehdr;
	void __iomem *metadata;
	size_t metadata_size;
	int i, ret;

	ehdr = (const struct elf32_hdr *)fw->data;
	phdrs = (const struct elf32_phdr *)(ehdr + 1);

	ret = request_firmware(&fw, rproc->firmware, rproc->dev.parent);
	if (ret) {
		dev_err(rproc->dev.parent, "Failed to request firmware: %d\n",
			ret);
		return ret;
	}

	metadata = qcom_mdt_read_metadata(fw, &metadata_size, rproc->firmware,
					  rproc->dev.parent);
	if (IS_ERR(metadata)) {
		ret = PTR_ERR(metadata);
		dev_err(rproc->dev.parent,
			"Failed to read firmware metadata: %d\n", ret);
		goto release_fw;
	}

	ret = qcom_scm_pas_init_image(BTSS_PAS_ID, metadata,
				      metadata_size, NULL);
	if (ret) {
		dev_err(rproc->dev.parent, "PAS init image failed: %d\n", ret);
		goto free_metadata;
	}

	for (i = 0; i < ehdr->e_phnum; i++) {
		char *seg_name __free(kfree) = kstrdup(rproc->firmware,
						       GFP_KERNEL);
		if (!seg_name)
			return -ENOMEM;

		phdr = &phdrs[i];

		/* Only process valid loadable data segments */
		if (phdr->p_type != PT_LOAD || !phdr->p_memsz)
			continue;

		if (phdr->p_vaddr + phdr->p_filesz > desc->mem_size) {
			dev_err(rproc->dev.parent,
				"Segment data exceeds the reserved memory area!\n");
			goto free_metadata;
		}

		/* Check if firmware is split across multiple segment files */
		if (phdr->p_offset > fw->size ||
		    phdr->p_offset + phdr->p_filesz > fw->size) {
			sprintf(seg_name + strlen(seg_name) - 3, "b%02d", i);
			ret = request_firmware(&seg_fw, seg_name,
					       rproc->dev.parent);
			if (ret) {
				dev_err(rproc->dev.parent,
					"Could not find split segment binary: %s\n",
					seg_name);
				goto free_metadata;
			}

			/*
			 * Use the virtual instead of the physical address as
			 * the offset
			 */
			memcpy_toio(desc->mem_region + phdr->p_vaddr,
				    seg_fw->data, phdr->p_filesz);

			release_firmware(seg_fw);
		} else {
			memcpy_toio(desc->mem_region + phdr->p_vaddr,
				    fw->data + phdr->p_offset, phdr->p_filesz);
		}
	}

	return 0;

free_metadata:
	kfree(metadata);
release_fw:
	release_firmware(fw);
	return ret;
}

static const struct rproc_ops m0_btss_ops = {
	.start = m0_btss_start,
	.stop = m0_btss_stop,
	.load = m0_btss_load,
	.get_boot_addr = rproc_elf_get_boot_addr,
};

static int m0_btss_alloc_memory_region(struct m0_btss *desc)
{
	struct device *dev = desc->dev;
	struct resource res;
	int ret;

	ret = of_reserved_mem_region_to_resource(dev->of_node, 0, &res);
	if (ret) {
		dev_err(dev, "unable to acquire memory-region resource\n");
		return ret;
	}

	desc->mem_phys = res.start;
	desc->mem_reloc = res.start;
	desc->mem_size = resource_size(&res);
	desc->mem_region = devm_ioremap(dev, desc->mem_phys, desc->mem_size);
	if (!desc->mem_region) {
		dev_err(dev, "unable to map memory region: %pR\n", &res);
		return -ENOMEM;
	}

	return 0;
}

static int m0_btss_pil_probe(struct platform_device *pdev)
{
	// struct reset_control *btss_reset;
	struct device *dev = &pdev->dev;
	const char *fw_name = NULL;
	struct m0_btss *desc;
	struct clk *lpo_clk;
	struct rproc *rproc;
	int ret;

	ret = of_property_read_string(dev->of_node, "firmware-name",
				      &fw_name);
	if (ret < 0)
		return ret;

	rproc = devm_rproc_alloc(dev, "m0btss", &m0_btss_ops,
				 fw_name, sizeof(*desc));
	if (!rproc) {
		dev_err(dev, "failed to allocate rproc\n");
		return -ENOMEM;
	}

	desc = rproc->priv;
	desc->dev = dev;

	ret = m0_btss_alloc_memory_region(desc);
	if (ret)
		return ret;

	lpo_clk = devm_clk_get_enabled(dev, "btss_lpo_clk");
	if (IS_ERR(lpo_clk))
		return dev_err_probe(dev, PTR_ERR(lpo_clk),
				     "Failed to get lpo clock\n");

	desc->btss_reset = devm_reset_control_get(dev, "btss_reset");
	if (IS_ERR_OR_NULL(desc->btss_reset))
		return dev_err_probe(dev, PTR_ERR(desc->btss_reset),
				     "unable to acquire btss_reset\n");

	ret = reset_control_deassert(desc->btss_reset);
	if (ret)
		return dev_err_probe(rproc->dev.parent, ret,
				     "Failed to deassert reset\n");

	rproc->auto_boot = false;
	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, rproc);

	return 0;
}

static const struct of_device_id m0_btss_of_match[] = {
	{ .compatible = "qcom,ipq5018-btss-pil" },
	{ },
};
MODULE_DEVICE_TABLE(of, m0_btss_of_match);

static struct platform_driver m0_btss_pil_driver = {
	.probe = m0_btss_pil_probe,
	.driver = {
		.name = "qcom-m0-btss-pil",
		.of_match_table = m0_btss_of_match,
	},
};

module_platform_driver(m0_btss_pil_driver);

MODULE_DESCRIPTION("Qualcomm M0 Bluetooth Subsystem Peripheral Image Loader");
MODULE_LICENSE("GPL");
