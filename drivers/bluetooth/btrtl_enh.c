// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Bluetooth support for Realtek devices
 *
 *  Copyright (C) 2026 Realtek Semiconductor Corporation.
 */

#include <linux/firmware.h>
#include <linux/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btrtl.h"

#define RTL_VENDOR_WRITEB_TYPE		0x00
#define RTL_VENDOR_WRITE_TYPE		0x21
#define RTL_VENDOR_REG32_TYPE		0x20
#define RTL_CHIP_7090A			62
#define RTL_WRZM_CNT			48
#define RTL_WRZM_ADDR_A			0x00403BAC
#define RTL_WRZM_ADDR_B			0x00400018
#define RTL_WRZM_ADDR_C			0x00400014
#define RTL_PATCH_V3_1		0x01
#define RTL_PATCH_V3_2		0x02
#define IMAGE_ID_F000		0xf000
#define IMAGE_ID_F001		0xf001
#define IMAGE_ID_F002		0xf002

#define DL_FIX_CI_ID		0
#define DL_FIX_CI_ADDR		1
#define DL_FIX_PATCH_ADDR	2
#define DL_FIX_SEC_HDR_ADDR	3
#define DL_FIX_ADDR_MAX		4

struct rtl_vendor_write_cmd {
	u8 type;
	__le32 addr;
	__le32 val;
} __packed;

struct rtl_vendor_writeb_cmd {
	u8 type;
	__le32 addr;
	u8 val;
} __packed;

struct rtl_vendor_read_cmd {
	u8 type;
	__le32 addr;
} __packed;

struct rtl_vendor_read_rsp {
	u8 status;
	__le32 val;
} __packed;

struct rtl_rp_dl_v3 {
	__u8 status;
	__u8 index;
	__u8 err;
} __packed;

struct rtl_epatch_header_v3 {
	__u8 signature[8];
	__u8 timestamp[8];
	__le32 ver_rsvd;
	__le32 num_sections;
} __packed;

struct rtl_section_v3 {
	__le32 opcode;
	__le64 len;
	u8 data[];
} __packed;

struct rtl_addr_fix {
	u32 addr;
	u32 value;
};

struct rtl_section_patch_image {
	u16 image_id;
	u8 index;
	u8 config_rule;
	u8 need_config;

	struct rtl_addr_fix fix[DL_FIX_ADDR_MAX];

	u32 image_len;
	u8 *image_data;
	u32 image_ver;

	u8  *cfg_buf;
	u16 cfg_len;

	struct list_head list;
};

struct rtl_patch_image_hdr {
	__le16 chip_id;
	u8 ic_cut;
	u8 key_id;
	u8 enable_ota;
	__le16 image_id;
	u8 config_rule;
	u8 need_config;
	u8 rsv[950];

	__le64 addr_fix[DL_FIX_ADDR_MAX * 2];
	u8 index;

	__le64 patch_image_len;
	__u8 data[];
} __packed;

static int btrtl_vendor_write_mem(struct hci_dev *hdev, u32 addr, u32 val)
{
	struct rtl_vendor_write_cmd cp;
	struct sk_buff *skb;
	int err = 0;

	cp.type = RTL_VENDOR_WRITE_TYPE;
	cp.addr = cpu_to_le32(addr);
	cp.val = cpu_to_le32(val);
	skb = __hci_cmd_sync(hdev, RTL_VSC_OP_WRITE_VENDOR, sizeof(cp), &cp, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "RTL: Write mem32 failed (%d)", err);
		return err;
	}

	kfree_skb(skb);
	return 0;
}

static int btrtl_vendor_read_reg32(struct hci_dev *hdev, u32 addr, u32 *val)
{
	struct rtl_vendor_read_cmd cp;
	struct rtl_vendor_read_rsp *rp;
	struct sk_buff *skb;

	cp.type = RTL_VENDOR_REG32_TYPE;
	cp.addr = cpu_to_le32(addr);
	skb = __hci_cmd_sync(hdev, RTL_VSC_OP_READ_VENDER,
			     sizeof(cp), &cp, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	rp = skb_pull_data(skb, sizeof(*rp));
	if (rp && !rp->status)
		*val = le32_to_cpu(rp->val);
	kfree_skb(skb);

	if (!rp || rp->status)
		return -EIO;

	return 0;
}

static int btrtl_vendor_write_reg32(struct hci_dev *hdev, u32 addr, u32 val)
{
	struct rtl_vendor_write_cmd cp;
	struct sk_buff *skb;

	cp.type = RTL_VENDOR_REG32_TYPE;
	cp.addr = cpu_to_le32(addr);
	cp.val  = cpu_to_le32(val);
	skb = __hci_cmd_sync(hdev, RTL_VSC_OP_WRITE_VENDOR,
			     sizeof(cp), &cp, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	kfree_skb(skb);
	return 0;
}

static int btrtl_vendor_write_reg8(struct hci_dev *hdev, u32 addr, u8 val)
{
	struct rtl_vendor_writeb_cmd cp;
	struct sk_buff *skb;

	cp.type = RTL_VENDOR_WRITEB_TYPE;
	cp.addr = cpu_to_le32(addr);
	cp.val  = val;
	skb = __hci_cmd_sync(hdev, RTL_VSC_OP_WRITE_VENDOR,
			     sizeof(cp), &cp, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);
	kfree_skb(skb);
	return 0;
}

static int btrtl_wrzm(struct hci_dev *hdev,
		      struct btrtl_device_info *btrtl_dev)
{
	u32 val;
	int ret;
	int i;

	for (i = 0; i < RTL_WRZM_CNT; i++) {
		ret = btrtl_vendor_read_reg32(hdev, RTL_WRZM_ADDR_A + i * 4, &val);
		if (ret) {
			rtl_dev_err(hdev, "WRZM: read[%d] failed (%d)", i, ret);
			return ret;
		}
		ret = btrtl_vendor_write_reg32(hdev, RTL_WRZM_ADDR_B + i * 4, val);
		if (ret) {
			rtl_dev_err(hdev, "WRZM: write[%d] failed (%d)", i, ret);
			return ret;
		}
	}

	ret = btrtl_vendor_read_reg32(hdev, RTL_WRZM_ADDR_C, &val);
	if (ret) {
		rtl_dev_err(hdev, "WRZM: read (part2) failed (%d)", ret);
		return ret;
	}

	val |= 0x10;

	ret = btrtl_vendor_write_reg8(hdev, RTL_WRZM_ADDR_C, val);
	if (ret) {
		rtl_dev_err(hdev, "WRZM: write (part2) failed (%d)", ret);
		return ret;
	}

	return 0;
}

static void btrtl_insert_ordered_patch_image(struct rtl_section_patch_image *image,
					     struct btrtl_device_info *btrtl_dev)
{
	struct list_head *pos;
	struct list_head *next;
	struct rtl_section_patch_image *node;

	list_for_each_safe(pos, next, &btrtl_dev->patch_images) {
		node = list_entry(pos, struct rtl_section_patch_image, list);

		if (node->image_id > image->image_id) {
			__list_add(&image->list, pos->prev, pos);
			return;
		}

		if (node->image_id == image->image_id &&
		    node->index > image->index) {
			__list_add(&image->list, pos->prev, pos);
			return;
		}
	}
	__list_add(&image->list, pos->prev, pos);
}

static int rtlbt_parse_config(struct hci_dev *hdev,
			      struct rtl_section_patch_image *patch_image,
			      struct btrtl_device_info *btrtl_dev)
{
	const struct id_table *ic_info = NULL;
	const struct firmware *fw;
	char tmp_name[32];
	char filename[64];
	u8 *cfg_buf;
	char *str;
	char *p;
	size_t len;
	int ret;

	if (btrtl_dev && btrtl_dev->ic_info)
		ic_info = btrtl_dev->ic_info;

	if (!ic_info)
		return -EINVAL;

	str = ic_info->cfg_name;
	if (!str)
		return -EINVAL;

	if (btrtl_dev->fw_type == FW_TYPE_V3_1) {
		if (!patch_image->image_id && !patch_image->index) {
			snprintf(filename, sizeof(filename), "%s.bin", str);
			goto load_fw;
		}
		goto done;
	}

	len = strlen(str);
	if (len > sizeof(tmp_name) - 1)
		len = sizeof(tmp_name) - 1;
	memcpy(tmp_name, str, len);
	tmp_name[len] = '\0';

	str = tmp_name;
	p = strsep(&str, ".");

	ret = snprintf(filename, sizeof(filename), "%s", p);
	if (patch_image->config_rule && patch_image->need_config) {
		switch (patch_image->image_id) {
		case IMAGE_ID_F000:
		case IMAGE_ID_F001:
		case IMAGE_ID_F002:
			ret += snprintf(filename + ret, sizeof(filename) - ret,
					"_%04x", patch_image->image_id);
			break;
		default:
			goto done;
		}
	} else {
		goto done;
	}

	snprintf(filename + ret, sizeof(filename) - ret, ".%s", str ? str : "bin");

load_fw:
	rtl_dev_info(hdev, "config file: %s", filename);
	ret = request_firmware(&fw, filename, &hdev->dev);
	if (ret < 0) {
		if (btrtl_dev->fw_type == FW_TYPE_V3_2) {
			len = 4;
			cfg_buf = kvmalloc(len, GFP_KERNEL);
			if (!cfg_buf)
				return -ENOMEM;

			memset(cfg_buf, 0xff, len);
			patch_image->cfg_buf = cfg_buf;
			patch_image->cfg_len = len;
			return 0;
		}
		goto err_req_fw;
	}
	rtl_dev_info(hdev, "config file: %s found", filename);
	cfg_buf = kvmalloc(fw->size, GFP_KERNEL);
	if (!cfg_buf) {
		ret = -ENOMEM;
		goto err;
	}
	memcpy(cfg_buf, fw->data, fw->size);
	len = fw->size;
	release_firmware(fw);

	patch_image->cfg_buf = cfg_buf;
	patch_image->cfg_len = len;
done:
	return 0;
err:
	release_firmware(fw);
err_req_fw:
	rtl_dev_info(hdev, "config file: [%s] not found", filename);
	return ret;
}

static int rtlbt_parse_section_v3(struct hci_dev *hdev,
				  struct btrtl_device_info *btrtl_dev,
				  u32 opcode, u8 *data, u32 len)
{
	struct rtl_section_patch_image *patch_image;
	struct rtl_patch_image_hdr *hdr;
	u16 image_id;
	u16 chip_id;
	size_t patch_image_len;
	u8 *ptr;
	int ret = 0;
	size_t i;
	struct rtl_iovec iov = {
		.data = data,
		.len  = len,
	};

	hdr = rtl_iov_pull_data(&iov, sizeof(*hdr));
	if (!hdr)
		return -EINVAL;

	if (btrtl_dev->opcode && btrtl_dev->opcode != opcode) {
		rtl_dev_err(hdev, "invalid opcode 0x%02x", opcode);
		return -EINVAL;
	}

	if (!btrtl_dev->opcode) {
		btrtl_dev->opcode = opcode;
		switch (btrtl_dev->opcode) {
		case RTL_PATCH_V3_1:
			btrtl_dev->fw_type = FW_TYPE_V3_1;
			break;
		case RTL_PATCH_V3_2:
			btrtl_dev->fw_type = FW_TYPE_V3_2;
			break;
		default:
			return -EINVAL;
		}
	}

	patch_image_len = (u32)le64_to_cpu(hdr->patch_image_len);
	chip_id = le16_to_cpu(hdr->chip_id);
	image_id = le16_to_cpu(hdr->image_id);
	rtl_dev_info(hdev, "image (%04x:%02x), chip id %u, cut 0x%02x, len %08zx"
		     , image_id, hdr->index, chip_id, hdr->ic_cut,
		     patch_image_len);

	if (btrtl_dev->key_id != hdr->key_id) {
		rtl_dev_info(hdev, "skip image, key_id mismatch (%u, %u)",
			    hdr->key_id, btrtl_dev->key_id);
		return 0;
	}

	if (hdr->ic_cut != btrtl_dev->rom_version + 1) {
		rtl_dev_info(hdev, "skip image, ic_cut mismatch (%u, %u)",
			    hdr->ic_cut, btrtl_dev->rom_version + 1);
		return 0;
	}

	if (btrtl_dev->fw_type == FW_TYPE_V3_1 && !btrtl_dev->project_id)
		btrtl_dev->project_id = chip_id;

	if (btrtl_dev->fw_type == FW_TYPE_V3_2 &&
	    chip_id != btrtl_dev->project_id) {
		rtl_dev_info(hdev, "skip image, chip_id mismatch (%u, %d)", chip_id,
			    btrtl_dev->project_id);
		return 0;
	}

	ptr = rtl_iov_pull_data(&iov, patch_image_len);
	if (!ptr)
		return -ENODATA;

	patch_image = kzalloc_obj(*patch_image);
	if (!patch_image)
		return -ENOMEM;
	patch_image->index = hdr->index;
	patch_image->image_id = image_id;
	patch_image->config_rule = hdr->config_rule;
	patch_image->need_config = hdr->need_config;

	for (i = 0; i < DL_FIX_ADDR_MAX; i++) {
		patch_image->fix[i].addr =
			(u32)le64_to_cpu(hdr->addr_fix[i * 2]);
		patch_image->fix[i].value =
			(u32)le64_to_cpu(hdr->addr_fix[i * 2 + 1]);
	}

	patch_image->image_len = patch_image_len;

	if (patch_image_len < 4) {
		rtl_dev_err(hdev, "image payload too short (%zu)",
			    patch_image_len);
		ret = -EINVAL;
		goto err;
	}

	patch_image->image_data = kvmalloc(patch_image_len, GFP_KERNEL);
	if (!patch_image->image_data) {
		ret = -ENOMEM;
		goto err;
	}
	memcpy(patch_image->image_data, ptr, patch_image_len);
	patch_image->image_ver =
		get_unaligned_le32(ptr + patch_image->image_len - 4);
	rtl_dev_info(hdev, "image version: %08x", patch_image->image_ver);

	ret = rtlbt_parse_config(hdev, patch_image, btrtl_dev);
	if (ret) {
		rtl_dev_err(hdev, "config parse failed (%d)", ret);
		goto err;
	}

	ret = patch_image->image_len;

	btrtl_insert_ordered_patch_image(patch_image, btrtl_dev);

	return ret;
err:
	kvfree(patch_image->image_data);
	kvfree(patch_image->cfg_buf);
	kfree(patch_image);
	return ret;
}

int rtlbt_parse_firmware_v3(struct hci_dev *hdev,
			    struct btrtl_device_info *btrtl_dev)
{
	struct rtl_epatch_header_v3 *hdr;
	int rc;
	u32 num_sections;
	struct rtl_section_v3 *section;
	u32 section_len;
	u32 opcode;
	int len = 0;
	int i;
	u8 *ptr;
	struct rtl_iovec iov = {
		.data = btrtl_dev->fw_data,
		.len  = btrtl_dev->fw_len,
	};

	rtl_dev_info(hdev, "key id %u", btrtl_dev->key_id);

	hdr = rtl_iov_pull_data(&iov, sizeof(*hdr));
	if (!hdr)
		return -EINVAL;
	num_sections = le32_to_cpu(hdr->num_sections);

	rtl_dev_dbg(hdev, "timpstamp %08x-%08x", *((u32 *)hdr->timestamp),
		    *((u32 *)(hdr->timestamp + 4)));

	for (i = 0; i < num_sections; i++) {
		section = rtl_iov_pull_data(&iov, sizeof(*section));
		if (!section)
			break;

		section_len = (u32)le64_to_cpu(section->len);
		opcode = le32_to_cpu(section->opcode);

		rtl_dev_dbg(hdev, "opcode 0x%04x", section->opcode);

		ptr = rtl_iov_pull_data(&iov, section_len);
		if (!ptr)
			break;

		rc = 0;
		switch (opcode) {
		case RTL_PATCH_V3_1:
		case RTL_PATCH_V3_2:
			rc = rtlbt_parse_section_v3(hdev, btrtl_dev, opcode,
						    ptr, section_len);
			break;
		default:
			rtl_dev_warn(hdev, "Unknown opcode %08x", opcode);
			break;
		}
		if (rc < 0) {
			rtl_dev_err(hdev, "Parse section (%u) err (%d)",
				    opcode, rc);
			continue;
		}
		len += rc;
	}

	rtl_dev_info(hdev, "image payload total len: 0x%08x", len);
	if (!len) {
		rtl_dev_err(hdev, "no matching firmware section found");
		return -ENODATA;
	}

	return len;
}

static int rtl_check_download_state(struct hci_dev *hdev,
				    struct btrtl_device_info *btrtl_dev)
{
	struct sk_buff *skb;
	int ret = 0;
	u8 *state;

	btrealtek_set_flag(hdev, REALTEK_DOWNLOADING);

	skb = __hci_cmd_sync(hdev, RTL_VSC_OP_CHECK_DOWNLOAD_STATE, 0, NULL, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		btrealtek_clear_flag(hdev, REALTEK_DOWNLOADING);
		rtl_dev_err(hdev, "write tb error %lu", PTR_ERR(skb));
		return -EIO;
	}

	/* Other driver might be downloading the combined firmware. */
	state = skb_pull_data(skb, sizeof(*state));
	if (state && *state == 0x03) {
		ret = btrealtek_wait_on_flag_timeout(hdev, REALTEK_DOWNLOADING,
						     TASK_INTERRUPTIBLE,
						     msecs_to_jiffies(5000));
		if (ret == -EINTR) {
			bt_dev_err(hdev, "Firmware loading interrupted");
			goto out;
		}

		if (ret) {
			bt_dev_err(hdev, "Firmware loading timeout");
			ret = -ETIMEDOUT;
		} else {
			ret = -EALREADY;
		}

	} else {
		btrealtek_clear_flag(hdev, REALTEK_DOWNLOADING);
	}

out:
	kfree_skb(skb);
	return ret;
}

static int rtl_finalize_download(struct hci_dev *hdev,
				 struct btrtl_device_info *btrtl_dev)
{
	struct hci_rp_read_local_version *rp_ver;
	u8 params[2] = { 0x03, 0xb2 };
	struct sk_buff *skb;
	int ret = 0;
	u16 opcode;
	u32 len;
	u8 *p;

	opcode = RTL_VSC_OP_WDG_RESET_CMD;
	len = 2;
	if (btrtl_dev->opcode == RTL_PATCH_V3_1) {
		opcode = RTL_VSC_OP_DOWNLOAD_CMD;
		params[0] = 0x80;
		len = 1;
	}
	skb = __hci_cmd_sync(hdev, opcode, len, params, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		rtl_dev_err(hdev, "Watchdog reset err (%ld)", PTR_ERR(skb));
		return -EIO;
	}
	p = skb_pull_data(skb, 1);
	if (!p) {
		ret = -ENODATA;
		goto out;
	}
	rtl_dev_info(hdev, "Watchdog reset status %02x", *p);
	kfree_skb(skb);

	skb = btrtl_read_local_version(hdev);
	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		rtl_dev_err(hdev, "read local version failed (%d)", ret);
		return ret;
	}

	rp_ver = skb_pull_data(skb, sizeof(*rp_ver));
	if (rp_ver)
		rtl_dev_info(hdev, "fw version 0x%04x%04x",
			     __le16_to_cpu(rp_ver->hci_rev),
			     __le16_to_cpu(rp_ver->lmp_subver));
out:
	kfree_skb(skb);
	return ret;
}

static int rtl_security_check(struct hci_dev *hdev,
			      struct btrtl_device_info *btrtl_dev)
{
	struct rtl_section_patch_image *tmp = NULL;
	struct rtl_section_patch_image *image = NULL;
	u32 val;
	int ret;

	list_for_each_entry_reverse(tmp, &btrtl_dev->patch_images, list) {
		/* Check security hdr */
		if (!tmp->fix[DL_FIX_SEC_HDR_ADDR].value ||
		    !tmp->fix[DL_FIX_SEC_HDR_ADDR].addr ||
		    tmp->fix[DL_FIX_SEC_HDR_ADDR].addr == 0xffffffff)
			continue;
		rtl_dev_info(hdev, "addr 0x%08x, value 0x%08x",
			     tmp->fix[DL_FIX_SEC_HDR_ADDR].addr,
			     tmp->fix[DL_FIX_SEC_HDR_ADDR].value);
		image = tmp;
		break;
	}

	if (!image)
		return 0;

	rtl_dev_info(hdev, "sec image (%04x:%02x)", image->image_id,
		     image->index);
	val = image->fix[DL_FIX_PATCH_ADDR].value + image->image_len -
					image->fix[DL_FIX_SEC_HDR_ADDR].value;
	ret = btrtl_vendor_write_mem(hdev, image->fix[DL_FIX_PATCH_ADDR].addr,
				     val);
	if (ret) {
		rtl_dev_err(hdev, "write sec reg failed (%d)", ret);
		return ret;
	}
	return 0;
}

int rtl_download_firmware_v3(struct hci_dev *hdev,
			     struct btrtl_device_info *btrtl_dev)
{
	struct rtl_section_patch_image *image, *tmp;
	struct rtl_rp_dl_v3 *rp;
	struct sk_buff *skb;
	u8 *fw_data;
	int fw_len;
	int ret = 0;
	u8 i;

	if (btrtl_dev->project_id == RTL_CHIP_7090A) {
		ret = btrtl_wrzm(hdev, btrtl_dev);
		if (ret) {
			rtl_dev_err(hdev, "v3 WRZM failed (%d)", ret);
			return ret;
		}
	}

	if (btrtl_dev->fw_type == FW_TYPE_V3_2) {
		ret = rtl_check_download_state(hdev, btrtl_dev);
		if (ret) {
			if (ret == -EALREADY)
				return 0;
			return ret;
		}
	}

	list_for_each_entry_safe(image, tmp, &btrtl_dev->patch_images, list) {
		rtl_dev_dbg(hdev, "image (%04x:%02x)", image->image_id,
			    image->index);

		for (i = DL_FIX_CI_ID; i < DL_FIX_ADDR_MAX; i++) {
			if (!image->fix[i].addr ||
			    image->fix[i].addr == 0xffffffff) {
				rtl_dev_dbg(hdev, "no need to write addr %08x",
					    image->fix[i].addr);
				continue;
			}
			rtl_dev_dbg(hdev, "write addr and val, 0x%08x, 0x%08x",
				    image->fix[i].addr, image->fix[i].value);
			if (btrtl_vendor_write_mem(hdev, image->fix[i].addr,
						   image->fix[i].value)) {
				rtl_dev_err(hdev, "write reg failed");
				ret = -EIO;
				goto done;
			}
		}

		fw_len = image->image_len + image->cfg_len;
		fw_data = kvmalloc(fw_len, GFP_KERNEL);
		if (!fw_data) {
			rtl_dev_err(hdev, "Couldn't alloc buf for image data");
			ret = -ENOMEM;
			goto done;
		}
		memcpy(fw_data, image->image_data, image->image_len);
		if (image->cfg_len > 0)
			memcpy(fw_data + image->image_len, image->cfg_buf,
			       image->cfg_len);

		rtl_dev_dbg(hdev, "patch image (%04x:%02x). len: %d",
			    image->image_id, image->index, fw_len);
		rtl_dev_dbg(hdev, "fw_data %p, image buf %p, len %u", fw_data,
			    image->image_data, image->image_len);

		ret = rtl_download_firmware(hdev, btrtl_dev->fw_type, fw_data,
					    fw_len);
		kvfree(fw_data);
		if (ret < 0) {
			rtl_dev_err(hdev, "download firmware failed (%d)", ret);
			goto done;
		}

		if (image->list.next != &btrtl_dev->patch_images &&
		    image->image_id == tmp->image_id)
			continue;

		if (btrtl_dev->fw_type == FW_TYPE_V3_1)
			continue;

		i = 0x80;
		skb = __hci_cmd_sync(hdev, RTL_VSC_OP_DOWNLOAD_CMD, 1, &i, HCI_CMD_TIMEOUT);
		if (IS_ERR(skb)) {
			ret = -EIO;
			rtl_dev_err(hdev, "Failed to issue last cmd fc20, %ld",
				    PTR_ERR(skb));
			goto done;
		}
		ret = 2;
		rp = skb_pull_data(skb, sizeof(*rp));
		if (rp)
			ret = rp->err;
		kfree_skb(skb);
		if (ret == 2) {
			/* Verification failure */
			ret = -EFAULT;
			goto done;
		}
	}

	if (btrtl_dev->fw_type == FW_TYPE_V3_1) {
		ret = rtl_security_check(hdev, btrtl_dev);
		if (ret) {
			rtl_dev_err(hdev, "Security check failed (%d)", ret);
			goto done;
		}
	}

	ret = rtl_finalize_download(hdev, btrtl_dev);

done:
	return ret;
}

void btrtl_free_patch_images(struct btrtl_device_info *btrtl_dev)
{
	struct rtl_section_patch_image *image, *next;

	list_for_each_entry_safe(image, next, &btrtl_dev->patch_images, list) {
		list_del(&image->list);
		kvfree(image->image_data);
		kvfree(image->cfg_buf);
		kfree(image);
	}
}


struct btrtl_enh_ops rtl_enh_ops = {
	.parse_firmware_v3    = rtlbt_parse_firmware_v3,
	.download_firmware_v3 = rtl_download_firmware_v3,
	.free_patch_images    = btrtl_free_patch_images,
};
EXPORT_SYMBOL_GPL(rtl_enh_ops);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Realtek Bluetooth firmware v3+ support");
