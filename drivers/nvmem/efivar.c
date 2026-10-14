// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvmem-efivar - expose the contents of a UEFI variable as an NVMEM device
 *
 * Lets the kernel consume data provisioned in a UEFI variable through the
 * standard NVMEM cell bindings.  The variable is read at probe time through
 * the EFI runtime GetVariable service, which also works on platforms where
 * the variable store is emulated by firmware (for example the Qualcomm
 * uefisecapp backend) and where direct flash access is not available to the
 * non-secure world.  The variable contents are published as a read-only
 * NVMEM device, with cells described as child nodes in the device tree and
 * referenced by consumers through the standard "nvmem-cells" /
 * "nvmem-cell-names" properties.
 *
 * If the variable has not been provisioned yet, the device is still
 * registered, sized to cover the described cells and filled with zeroes,
 * the same way an erased EEPROM would read.  Consumers are expected to
 * validate the data they read so such systems keep working with their
 * defaults.
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/efi.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/ucs2_string.h>
#include <linux/uuid.h>

struct efivar_nvmem {
	void	*data;
	size_t	size;
};

static int efivar_nvmem_reg_read(void *priv, unsigned int offset,
				 void *val, size_t bytes)
{
	struct efivar_nvmem *p = priv;

	if (offset >= p->size || offset + bytes > p->size)
		return -EINVAL;

	memcpy(val, p->data + offset, bytes);
	return 0;
}

/* Size needed to cover all cells described as child nodes. */
static size_t efivar_nvmem_cells_extent(struct device *dev)
{
	struct device_node *child;
	size_t extent = 0;
	u32 reg[2];

	for_each_child_of_node(dev->of_node, child) {
		if (of_property_read_u32_array(child, "reg", reg, 2))
			continue;
		extent = max_t(size_t, extent, (size_t)reg[0] + reg[1]);
	}

	return extent;
}

/*
 * Find the vendor GUID of a variable given only its (UCS-2) name, by
 * enumerating the variable store.  Used when no 'efivar-guid' is provided.
 */
static int efivar_nvmem_find_guid(const efi_char16_t *want, efi_guid_t *out)
{
	unsigned long max = 512; /* in UCS-2 chars; variable names are short */
	efi_char16_t *name;
	efi_guid_t vendor = {};
	efi_status_t status;
	int ret = -ENODEV;

	name = kcalloc(max, sizeof(*name), GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	name[0] = 0;

	for (;;) {
		unsigned long name_size = max * sizeof(*name);

		status = efivar_get_next_variable(&name_size, name, &vendor);
		if (status == EFI_BUFFER_TOO_SMALL) {
			efi_char16_t *tmp;

			max = name_size / sizeof(*name) + 1;
			tmp = krealloc_array(name, max, sizeof(*name), GFP_KERNEL);
			if (!tmp) {
				ret = -ENOMEM;
				break;
			}
			name = tmp;
			continue;
		}
		if (status != EFI_SUCCESS) /* EFI_NOT_FOUND ends the walk */
			break;

		if (!ucs2_strncmp(name, want, ucs2_strlen(want) + 1)) {
			*out = vendor;
			ret = 0;
			break;
		}
	}

	kfree(name);
	return ret;
}

static int efivar_nvmem_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *name, *guid_str;
	struct nvmem_config cfg = {};
	struct efivar_nvmem *priv;
	struct nvmem_device *nvmem;
	efi_char16_t *name16;
	efi_guid_t guid;
	guid_t g;
	unsigned long size;
	efi_status_t status;
	u32 attr;
	int i, len, ret;
	void *buf;

	/*
	 * The EFI variable service may not be a native EFI runtime service;
	 * backends such as the Qualcomm uefisecapp driver register the
	 * efivars operations asynchronously.  Wait for them.
	 */
	if (!efivar_is_available())
		return -EPROBE_DEFER;

	ret = device_property_read_string(dev, "efivar-name", &name);
	if (ret)
		return dev_err_probe(dev, ret, "missing 'efivar-name'\n");

	/* ASCII variable name -> UCS-2 as expected by the EFI service */
	len = strlen(name);
	name16 = kcalloc(len + 1, sizeof(*name16), GFP_KERNEL);
	if (!name16)
		return -ENOMEM;
	for (i = 0; i < len; i++)
		name16[i] = name[i];

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		kfree(name16);
		return -ENOMEM;
	}

	/*
	 * The vendor GUID is optional: if it is not provided, look the
	 * variable up by name across the whole store.  A variable that has
	 * not been provisioned yet is reported as not found and handled
	 * below like an erased EEPROM.
	 */
	if (!device_property_read_string(dev, "efivar-guid", &guid_str)) {
		ret = guid_parse(guid_str, &g);
		if (ret) {
			kfree(name16);
			return dev_err_probe(dev, ret, "invalid 'efivar-guid'\n");
		}
		memcpy(&guid, &g, sizeof(guid));
		status = EFI_SUCCESS;
	} else {
		ret = efivar_nvmem_find_guid(name16, &guid);
		if (ret && ret != -ENODEV) {
			kfree(name16);
			return dev_err_probe(dev, ret,
					     "failed to look up variable '%s'\n",
					     name);
		}
		status = ret ? EFI_NOT_FOUND : EFI_SUCCESS;
	}

	/* First attempt with a modest buffer; grow if the firmware asks. */
	size = 512;
	buf = kzalloc(size, GFP_KERNEL);
	if (!buf) {
		kfree(name16);
		return -ENOMEM;
	}

	if (status == EFI_SUCCESS) {
		status = efivar_get_variable(name16, &guid, &attr, &size, buf);
		if (status == EFI_BUFFER_TOO_SMALL) {
			kfree(buf);
			buf = kzalloc(size, GFP_KERNEL);
			if (!buf) {
				kfree(name16);
				return -ENOMEM;
			}
			status = efivar_get_variable(name16, &guid, &attr,
						     &size, buf);
		}
	}
	kfree(name16);

	if (status == EFI_NOT_FOUND) {
		/*
		 * Variable not provisioned yet: behave like an erased
		 * EEPROM so that consumers still probe and fall back to
		 * their defaults.
		 */
		kfree(buf);
		size = efivar_nvmem_cells_extent(dev);
		if (!size)
			return dev_err_probe(dev, -ENODEV,
					     "variable '%s' not found and no cells described\n",
					     name);
		buf = kzalloc(size, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		dev_info(dev, "variable '%s' not provisioned, reading as zeroes\n",
			 name);
	} else if (status != EFI_SUCCESS) {
		kfree(buf);
		return dev_err_probe(dev, -ENODEV,
				     "GetVariable('%s') failed: 0x%lx\n",
				     name, status);
	} else {
		dev_dbg(dev, "variable '%s' attr=0x%x size=%lu\n",
			name, attr, size);
	}

	priv->data = devm_kmemdup(dev, buf, size, GFP_KERNEL);
	kfree(buf);
	if (!priv->data)
		return -ENOMEM;
	priv->size = size;

	cfg.dev			= dev;
	cfg.name		= "efivar";
	cfg.id			= NVMEM_DEVID_AUTO;
	cfg.owner		= THIS_MODULE;
	cfg.type		= NVMEM_TYPE_OTP;
	cfg.read_only		= true;
	cfg.word_size		= 1;
	cfg.stride		= 1;
	cfg.size		= priv->size;
	cfg.reg_read		= efivar_nvmem_reg_read;
	cfg.priv		= priv;
	cfg.of_node		= dev->of_node;
	cfg.add_legacy_fixed_of_cells = true;

	nvmem = devm_nvmem_register(dev, &cfg);
	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id efivar_nvmem_of_match[] = {
	{ .compatible = "nvmem-efivar" },
	{}
};
MODULE_DEVICE_TABLE(of, efivar_nvmem_of_match);

static struct platform_driver efivar_nvmem_driver = {
	.probe = efivar_nvmem_probe,
	.driver = {
		.name = "nvmem-efivar",
		.of_match_table = efivar_nvmem_of_match,
	},
};
module_platform_driver(efivar_nvmem_driver);

MODULE_DESCRIPTION("NVMEM provider backed by a UEFI variable");
MODULE_IMPORT_NS("EFIVAR");
MODULE_LICENSE("GPL");
