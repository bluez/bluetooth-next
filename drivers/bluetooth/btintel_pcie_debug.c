// SPDX-License-Identifier: GPL-2.0-only
/* Intel Bluetooth PCIe YoYo debug configuration support. */

#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <net/bluetooth/bluetooth.h>

#include "btintel_pcie_debug.h"

static int btintel_pcie_yoyo_parse_dbg_info(struct btintel_pcie_yoyo_ctx *ctx,
					    const struct btintel_pcie_tlv_dbg_info *info,
					    u32 len)
{
	const u8 *data = (const u8 *)info;
	u32 domain, image_type, core_type, version;

	if (len != sizeof(*info) - sizeof(struct btintel_pcie_tlv_hdr))
		return -EBADMSG;

	image_type = get_unaligned_le32(data +
				       offsetof(struct btintel_pcie_tlv_dbg_info,
						image_type));
	core_type = get_unaligned_le32(data +
				      offsetof(struct btintel_pcie_tlv_dbg_info,
					       core_type));
	domain = get_unaligned_le32(data +
				   offsetof(struct btintel_pcie_tlv_dbg_info,
					    dbg_hdr.domain));
	version = get_unaligned_le32(data +
				     offsetof(struct btintel_pcie_tlv_dbg_info,
					      dbg_hdr.tlv_data_version));

	if (domain != BTINTEL_PCIE_DOMAIN_BT ||
	    version != BTINTEL_PCIE_DBG_INFO_TLV_VERSION) {
		BT_WARN("YoYo: unsupported domain/version (%u/%u), disabling",
			domain, version);
		return -EINVAL;
	}
	if (image_type != BTINTEL_PCIE_IMAGE_TLV_TYPE_EXTERNAL) {
		BT_WARN("YoYo: unsupported image_type 0x%x, disabling", image_type);
		return -EINVAL;
	}
	if (core_type != BTINTEL_PCIE_CORE_TYPE_BT) {
		BT_WARN("YoYo: non-Bluetooth core_type %u, disabling", core_type);
		return -EINVAL;
	}

	ctx->dbg_info_present = true;
	BT_DBG("YoYo: config='%.64s' image=%u core=%u",
	       info->config_name, image_type, core_type);
	return 0;
}

static int btintel_pcie_yoyo_parse_buf_alloc(struct btintel_pcie_yoyo_ctx *ctx,
					     const struct btintel_pcie_tlv_buf_alloc *tlv,
					     u32 len)
{
	u32 alloc_id, buf_location, domain, idx, max_num_frags, min_buf_size;
	u32 req_buf_size, version;

	if (len != sizeof(*tlv) - sizeof(struct btintel_pcie_tlv_hdr)) {
		BT_WARN("YoYo: unexpected buffer allocation TLV length %u", len);
		return -EBADMSG;
	}

	domain = get_unaligned_le32(&tlv->dbg_hdr.domain);
	version = get_unaligned_le32(&tlv->dbg_hdr.tlv_data_version);
	if (domain != BTINTEL_PCIE_DOMAIN_BT ||
	    version != BTINTEL_PCIE_BUF_ALLOC_TLV_VERSION) {
		BT_WARN("YoYo: unsupported buffer allocation domain/version (%u/%u)",
			domain, version);
		return -EINVAL;
	}

	alloc_id = get_unaligned_le32(&tlv->alloc_id);
	if (alloc_id < BTINTEL_PCIE_ALLOC_ID_DBGC1 ||
	    alloc_id > BTINTEL_PCIE_ALLOC_ID_DBGC3) {
		BT_WARN("YoYo: invalid buffer allocation ID %u", alloc_id);
		return -EINVAL;
	}

	idx = alloc_id - BTINTEL_PCIE_ALLOC_ID_DBGC1;
	buf_location = get_unaligned_le32(&tlv->buf_location);
	max_num_frags = get_unaligned_le32(&tlv->max_num_frags);
	min_buf_size = get_unaligned_le32(&tlv->min_buf_size);
	req_buf_size = get_unaligned_le32(&tlv->req_buf_size);
	if (buf_location != BTINTEL_PCIE_BUF_LOC_DRAM) {
		BT_WARN("YoYo: allocation %u requests unsupported location %u",
			alloc_id, buf_location);
		return -EINVAL;
	}

	if (!max_num_frags || max_num_frags > BTINTEL_PCIE_MAX_BUF_FRAGS ||
	    min_buf_size < BTINTEL_PCIE_MIN_BUF_SIZE ||
	    min_buf_size > BTINTEL_PCIE_MAX_BUF_SIZE ||
	    req_buf_size > BTINTEL_PCIE_MAX_BUF_SIZE ||
	    req_buf_size % max_num_frags ||
	    req_buf_size / max_num_frags < min_buf_size) {
		BT_WARN("YoYo: invalid allocation %u size=%u fragments=%u minimum=%u",
			alloc_id, req_buf_size, max_num_frags, min_buf_size);
		return -EINVAL;
	}

	ctx->buf_alloc[idx].alloc_id = alloc_id;
	ctx->buf_alloc[idx].buf_location = buf_location;
	ctx->buf_alloc[idx].req_buf_size = req_buf_size;
	ctx->buf_alloc[idx].max_num_frags = max_num_frags;
	ctx->buf_alloc[idx].min_buf_size = min_buf_size;
	ctx->buf_alloc[idx].present = true;
	ctx->num_buf_allocs = max(ctx->num_buf_allocs, idx + 1);
	BT_DBG("YoYo: allocation %u location=%u size=%u fragments=%u minimum=%u",
	       alloc_id, buf_location, req_buf_size, max_num_frags, min_buf_size);
	return 0;
}

static int btintel_pcie_yoyo_parse_tlv_buffer(struct btintel_pcie_yoyo_ctx *ctx,
					      const u8 *buf, size_t buf_len)
{
	const u8 *ptr = buf;
	size_t remaining = buf_len;
	u32 count = 0;
	int err;

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
			err = btintel_pcie_yoyo_parse_dbg_info(ctx,
							       (const void *)ptr,
								       len);
			if (err) {
				ctx->dbg_info_present = false;
				return err;
			}
			break;
		case BTINTEL_PCIE_TLV_TYPE_DBG_BUF_ALLOC:
			err = btintel_pcie_yoyo_parse_buf_alloc(ctx,
								(const void *)ptr,
								       len);
			if (err)
				return err;
			break;
		case BTINTEL_PCIE_TLV_TYPE_DBG_REGION:
		case BTINTEL_PCIE_TLV_TYPE_DBG_TRIGGER:
		case BTINTEL_PCIE_TLV_TYPE_DBG_HCMD:
		case BTINTEL_PCIE_TLV_TYPE_DBG_CONFIG_SET:
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
	if (!ctx->dbg_info_present) {
		BT_ERR("YoYo: mandatory DEBUG_INFO TLV missing");
		return -EBADMSG;
	}
	if (!ctx->num_buf_allocs) {
		BT_ERR("YoYo: mandatory buffer allocation TLV missing");
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
	ctx->dbg_info_present = false;
	ctx->num_buf_allocs = 0;
	memset(ctx->buf_alloc, 0, sizeof(ctx->buf_alloc));
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
	if (!ctx->dbg_info_present) {
		BT_ERR("YoYo: mandatory DEBUG_INFO TLV missing; using legacy defaults");
		return -EBADMSG;
	}
	ctx->enabled = true;
	BT_INFO("YoYo: configuration loaded and validated");
	return 0;
}
EXPORT_SYMBOL_GPL(btintel_pcie_yoyo_load_and_parse);

const struct btintel_pcie_buf_alloc *
btintel_pcie_yoyo_get_buf_alloc(struct btintel_pcie_yoyo_ctx *ctx, u32 alloc_id)
{
	u32 idx;

	if (!ctx || !ctx->enabled ||
	    alloc_id < BTINTEL_PCIE_ALLOC_ID_DBGC1 ||
	    alloc_id > BTINTEL_PCIE_ALLOC_ID_DBGC3)
		return NULL;

	idx = alloc_id - BTINTEL_PCIE_ALLOC_ID_DBGC1;
	if (!ctx->buf_alloc[idx].present)
		return NULL;
	return &ctx->buf_alloc[idx];
}
EXPORT_SYMBOL_GPL(btintel_pcie_yoyo_get_buf_alloc);

MODULE_DESCRIPTION("Intel Bluetooth PCIe YoYo debug configuration support");
MODULE_LICENSE("GPL");
