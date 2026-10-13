/* SPDX-License-Identifier: GPL-2.0-only */
/* Intel Bluetooth PCIe YoYo debug configuration support. */

#ifndef __BTINTEL_PCIE_DEBUG_H__
#define __BTINTEL_PCIE_DEBUG_H__

#include <linux/device.h>
#include <linux/list.h>
#include <linux/types.h>

#define BTINTEL_PCIE_YOYO_FW_NAME "intel/ibt_debug_yoyo.bin"

#define BTINTEL_PCIE_TLV_TYPE_DBG_INFO		0x01000005
#define BTINTEL_PCIE_TLV_TYPE_DBG_BUF_ALLOC	0x01000006
#define BTINTEL_PCIE_TLV_TYPE_DBG_HCMD		0x01000007
#define BTINTEL_PCIE_TLV_TYPE_DBG_REGION		0x01000008
#define BTINTEL_PCIE_TLV_TYPE_DBG_TRIGGER	0x01000009
#define BTINTEL_PCIE_TLV_TYPE_DBG_CONFIG_SET	0x0100000a
#define BTINTEL_PCIE_TLV_TYPE_DBG_PRESET_OVERRIDE 0x0100000b

#define BTINTEL_PCIE_CONFIG_NAME_LEN		64
#define BTINTEL_PCIE_DOMAIN_BT			0
#define BTINTEL_PCIE_CORE_TYPE_BT		1

enum btintel_pcie_image_tlv_type {
	BTINTEL_PCIE_IMAGE_TLV_TYPE_EXTERNAL = 0x1,
	BTINTEL_PCIE_IMAGE_TLV_TYPE_INTERNAL = 0x2,
	BTINTEL_PCIE_IMAGE_TLV_TYPE_UNKNOWN = 0xff,
};

struct btintel_pcie_tlv_hdr {
	__le32 type;
	__le32 length;
} __packed;

struct btintel_pcie_dbg_hdr {
	__le32 tlv_data_version;
	__le32 domain;
} __packed;

struct btintel_pcie_addr_value {
	__le32 address;
	__le32 value;
} __packed;

struct btintel_pcie_yoyo_ctx {
	bool enabled;
	struct list_head region_list;
	struct list_head trigger_list;
	struct list_head hcmd_list;
	struct list_head config_set_list;
	struct hci_dev *hdev;
};

int btintel_pcie_yoyo_init(struct btintel_pcie_yoyo_ctx *ctx);
void btintel_pcie_yoyo_cleanup(struct btintel_pcie_yoyo_ctx *ctx);
int btintel_pcie_yoyo_load_and_parse(struct btintel_pcie_yoyo_ctx *ctx,
				     struct device *dev);

#endif /* __BTINTEL_PCIE_DEBUG_H__ */
