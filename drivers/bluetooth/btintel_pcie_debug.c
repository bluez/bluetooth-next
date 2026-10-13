// SPDX-License-Identifier: GPL-2.0-only
/* Intel Bluetooth PCIe YoYo debug configuration support. */

#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/bluetooth/bluetooth.h>

#include "btintel_pcie_debug.h"

static int btintel_pcie_yoyo_parse_tlv_buffer(struct btintel_pcie_yoyo_ctx *ctx,
					      const u8 *buf, size_t buf_len)
{
	const u8 *ptr = buf;
	size_t remaining = buf_len;
	u32 count = 0;

	if (!ctx || !buf)
		return -EINVAL;

	while (remaining >= sizeof(struct btintel_pcie_tlv_hdr)) {
		u32 type = get_unaligned_le32(ptr);
		u32 len = get_unaligned_le32(ptr + sizeof(__le32));

		if (len > remaining - sizeof(struct btintel_pcie_tlv_hdr)) {
			BT_ERR("YoYo: TLV %u truncated (len=%u remaining=%zu)",
			       count, len, remaining);
			return -EBADMSG;
		}

		switch (type) {
		case BTINTEL_PCIE_TLV_TYPE_DBG_INFO:
		case BTINTEL_PCIE_TLV_TYPE_DBG_REGION:
		case BTINTEL_PCIE_TLV_TYPE_DBG_TRIGGER:
		case BTINTEL_PCIE_TLV_TYPE_DBG_HCMD:
		case BTINTEL_PCIE_TLV_TYPE_DBG_CONFIG_SET:
		case BTINTEL_PCIE_TLV_TYPE_DBG_BUF_ALLOC:
		case BTINTEL_PCIE_TLV_TYPE_DBG_PRESET_OVERRIDE:
			BT_DBG("YoYo: recognized TLV type 0x%x len=%u", type, len);
			break;
		default:
			BT_DBG("YoYo: unknown TLV type 0x%x len=%u", type, len);
			break;
		}

		ptr += sizeof(struct btintel_pcie_tlv_hdr) + len;
		remaining -= sizeof(struct btintel_pcie_tlv_hdr) + len;
		count++;
	}

	if (remaining || !count) {
		BT_ERR("YoYo: invalid TLV stream (%zu trailing bytes, %u TLVs)",
		       remaining, count);
		return -EBADMSG;
	}
	return 0;
}

int btintel_pcie_yoyo_init(struct btintel_pcie_yoyo_ctx *ctx)
{
	if (!ctx)
		return -EINVAL;

	memset(ctx, 0, sizeof(*ctx));
	INIT_LIST_HEAD(&ctx->region_list);
	INIT_LIST_HEAD(&ctx->trigger_list);
	INIT_LIST_HEAD(&ctx->hcmd_list);
	INIT_LIST_HEAD(&ctx->config_set_list);
	return 0;
}
EXPORT_SYMBOL_GPL(btintel_pcie_yoyo_init);

void btintel_pcie_yoyo_cleanup(struct btintel_pcie_yoyo_ctx *ctx)
{
	if (!ctx)
		return;

	ctx->enabled = false;
}
EXPORT_SYMBOL_GPL(btintel_pcie_yoyo_cleanup);

int btintel_pcie_yoyo_load_and_parse(struct btintel_pcie_yoyo_ctx *ctx,
				     struct device *dev)
{
	const struct firmware *fw;
	int err;

	if (!ctx || !dev)
		return -EINVAL;

	err = request_firmware_direct(&fw, BTINTEL_PCIE_YOYO_FW_NAME, dev);
	if (err) {
		BT_ERR("YoYo: failed to load %s (%d)",
		       BTINTEL_PCIE_YOYO_FW_NAME, err);
		return err;
	}

	if (fw->size < sizeof(struct btintel_pcie_tlv_hdr))
		err = -EBADMSG;
	else
		err = btintel_pcie_yoyo_parse_tlv_buffer(ctx, fw->data, fw->size);

	release_firmware(fw);
	if (err) {
		BT_ERR("YoYo: TLV parsing failed (%d)", err);
		return err;
	}
	ctx->enabled = true;
	BT_INFO("YoYo: configuration loaded and validated");
	return 0;
}
EXPORT_SYMBOL_GPL(btintel_pcie_yoyo_load_and_parse);

MODULE_DESCRIPTION("Intel Bluetooth PCIe YoYo debug configuration support");
MODULE_LICENSE("GPL");
