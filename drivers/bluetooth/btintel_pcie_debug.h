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

#define BTINTEL_PCIE_DBG_INFO_TLV_VERSION	2
#define BTINTEL_PCIE_BUF_ALLOC_TLV_VERSION	1
#define BTINTEL_PCIE_CONFIG_NAME_LEN		64
#define BTINTEL_PCIE_DOMAIN_BT			0
#define BTINTEL_PCIE_CORE_TYPE_BT		1
#define BTINTEL_PCIE_MAX_BUF_ALLOC		3
#define BTINTEL_PCIE_MAX_BUF_FRAGS		16
#define BTINTEL_PCIE_MIN_BUF_SIZE		(32 * 1024)
#define BTINTEL_PCIE_MAX_BUF_SIZE		(4 * 1024 * 1024)

enum btintel_pcie_image_tlv_type {
	BTINTEL_PCIE_IMAGE_TLV_TYPE_EXTERNAL = 0x1,
	BTINTEL_PCIE_IMAGE_TLV_TYPE_INTERNAL = 0x2,
	BTINTEL_PCIE_IMAGE_TLV_TYPE_UNKNOWN = 0xff,
};

enum btintel_dbg_alloc_id {
	BTINTEL_PCIE_ALLOC_ID_INVALID = 0,
	BTINTEL_PCIE_ALLOC_ID_DBGC1 = 1,
	BTINTEL_PCIE_ALLOC_ID_DBGC2 = 2,
	BTINTEL_PCIE_ALLOC_ID_DBGC3 = 3,
	BTINTEL_PCIE_ALLOC_ID_MAX,
};

enum btintel_dbg_buf_location {
	BTINTEL_PCIE_BUF_LOC_INVALID = 0,
	BTINTEL_PCIE_BUF_LOC_SRAM = 1,
	BTINTEL_PCIE_BUF_LOC_DRAM = 2,
	BTINTEL_PCIE_BUF_LOC_NPK = 3,
	BTINTEL_PCIE_BUF_LOC_MAX,
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

struct btintel_pcie_tlv_dbg_info {
	struct btintel_pcie_tlv_hdr hdr;
	struct btintel_pcie_dbg_hdr dbg_hdr;
	__le32 image_type;
	__le32 core_type;
	u8 config_name[BTINTEL_PCIE_CONFIG_NAME_LEN];
} __packed;

struct btintel_pcie_tlv_buf_alloc {
	struct btintel_pcie_tlv_hdr hdr;
	struct btintel_pcie_dbg_hdr dbg_hdr;
	__le32 alloc_id;
	__le32 buf_location;
	__le32 req_buf_size;
	__le32 max_num_frags;
	__le32 min_buf_size;
} __packed;

struct btintel_pcie_buf_alloc {
	u32 alloc_id;
	u32 buf_location;
	u32 req_buf_size;
	u32 max_num_frags;
	u32 min_buf_size;
	bool present;
};

struct btintel_pcie_yoyo_ctx {
	bool enabled;
	bool dbg_info_present;
	struct btintel_pcie_buf_alloc buf_alloc[BTINTEL_PCIE_MAX_BUF_ALLOC];
	u32 num_buf_allocs;
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
const struct btintel_pcie_buf_alloc *
btintel_pcie_yoyo_get_buf_alloc(struct btintel_pcie_yoyo_ctx *ctx, u32 alloc_id);

#endif /* __BTINTEL_PCIE_DEBUG_H__ */
