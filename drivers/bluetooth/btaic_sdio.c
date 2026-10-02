// SPDX-License-Identifier: GPL-2.0
/*
 ******************************************************************************
 *
 * Copyright (C) 2020 AIC semiconductor.
 *
 * @brief AIC8800D80 Bluetooth SDIO firmware loader
 *
 ******************************************************************************
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/minmax.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include "btaic.h"

#define AIC_SDIO_VENDOR_ID			0xc8a1
#define AIC_SDIO_DEVICE_ID_8800D80		0x0082

#define AIC_SDIO_BLOCK_SIZE			512
#define AIC_SDIO_BUFFER_SIZE			1536
#define AIC_SDIO_TX_BUFFER_SIZE			1536
#define AIC_SDIO_RX_MAX_SIZE			(127 * AIC_SDIO_BLOCK_SIZE)

#define AIC_SDIO_INTR_ENABLE			0x00
#define AIC_SDIO_INTR_PENDING			0x01
#define AIC_SDIO_FLOW_CTRL_Q1			0x03
#define AIC_SDIO_MISC_INT_STATUS		0x04
#define AIC_SDIO_BYTEMODE_LEN			0x05
#define AIC_SDIO_BYTEMODE_ENABLE		0x07
#define AIC_SDIO_RD_FIFO			0x0f
#define AIC_SDIO_WR_FIFO			0x10

#define AIC_SDIO_OTHER_INTERRUPT		BIT(7)
#define AIC_SDIO_BYTE_MODE_BLOCKS		120
#define AIC_SDIO_FLOW_RETRIES			50
#define AIC_SDIO_FRAME_TAIL_LEN			4

#define AIC_CMD_TIMEOUT_MS			6000
#define AIC_CMD_TYPE				0x11
#define AIC_TASK_DBG				1
#define AIC_DRIVER_TASK				100
#define AIC_FIRST_MSG(task)			((u16)(task) << 10)

#define AIC_DBG_MEM_READ_REQ			(AIC_FIRST_MSG(AIC_TASK_DBG) + 0)
#define AIC_DBG_MEM_READ_CFM			(AIC_FIRST_MSG(AIC_TASK_DBG) + 1)
#define AIC_DBG_MEM_WRITE_REQ			(AIC_FIRST_MSG(AIC_TASK_DBG) + 2)
#define AIC_DBG_MEM_WRITE_CFM			(AIC_FIRST_MSG(AIC_TASK_DBG) + 3)
#define AIC_DBG_MEM_BLOCK_WRITE_REQ		(AIC_FIRST_MSG(AIC_TASK_DBG) + 11)
#define AIC_DBG_MEM_BLOCK_WRITE_CFM		(AIC_FIRST_MSG(AIC_TASK_DBG) + 12)

#define AIC_BT_CHIP_ID_ADDR			0x40500000
#define AIC_BT_CHIP_REV_U02			3
#define AIC_BT_CHIP_REV_U03			7

#define AIC_BT_FW_ADID				"aic/aic8800d80/fw_adid_8800d80_u02.bin"
#define AIC_BT_FW_PATCH				"aic/aic8800d80/fw_patch_8800d80_u02.bin"
#define AIC_BT_FW_TABLE				"aic/aic8800d80/fw_patch_table_8800d80_u02.bin"

#define AIC_BT_PATCH_TAG			"AICBT_PT_TAG"
#define AIC_BT_PATCH_TAG_SIZE			16
#define AIC_BT_PATCH_RECORD_HEADER_SIZE		24
#define AIC_BT_PATCH_PAIR_SIZE			8
#define AIC_BT_PATCH_BLOCK_SIZE			1024

#define AIC_BT_MODE_ONLY_COANT			5
#define AIC_BT_PORT_UART			2
#define AIC_BT_UART_BAUD			1500000
#define AIC_BT_UART_FLOW_CTRL			1
#define AIC_BT_LOW_POWER_ENABLE			1
#define AIC_BT_TX_POWER_LEVEL			0x00006f2f

enum aic_bt_patch_type {
	AIC_BT_PATCH_INFO,
	AIC_BT_PATCH_TRAP,
	AIC_BT_PATCH_B4,
	AIC_BT_PATCH_MODE,
	AIC_BT_PATCH_POWER_ON,
	AIC_BT_PATCH_AF,
	AIC_BT_PATCH_VERSION,
};

struct aic_bt_e2a_header {
	__le16 id;
	__le16 dest_id;
	__le16 src_id;
	__le16 param_len;
	__le32 pattern;
	u8 param[];
} __packed;

struct aic_bt_mem_read_cfm {
	__le32 address;
	__le32 value;
} __packed;

struct aic_bt_sdio {
	struct sdio_func *func;
	struct aic_bt_boot *boot;

	/* Serializes commands because the firmware accepts one at a time. */
	struct mutex command_mutex;
	/* Protects response state shared with the SDIO IRQ handler. */
	spinlock_t response_lock;
	struct completion command_done;
	u16 expected_response;
	void *response;
	size_t response_size;
	int command_result;
	bool waiting_response;
	/* The firmware protocol has no transaction sequence number. */
	bool command_timed_out;

	u8 *tx_buf;
	u8 *rx_buf;
	bool function_enabled;
	bool irq_claimed;
};

static u8 aic_bt_crc8(const u8 *buffer, size_t len)
{
	u8 crc = 0;
	size_t byte;
	int bit;

	for (byte = 0; byte < len; byte++) {
		for (bit = 0x80; bit; bit >>= 1) {
			if (crc & 0x80)
				crc = (crc << 1) ^ 0x07;
			else
				crc <<= 1;

			if (buffer[byte] & bit)
				crc ^= 0x07;
		}
	}

	return crc;
}

static void aic_bt_complete_response(struct aic_bt_sdio *btdev,
				     const struct aic_bt_e2a_header *message,
				     size_t message_len)
{
	unsigned long flags;
	size_t param_len;
	u16 id;

	if (message_len < sizeof(*message))
		return;

	id = get_unaligned_le16(&message->id);
	param_len = get_unaligned_le16(&message->param_len);
	if (param_len > message_len - sizeof(*message))
		return;

	spin_lock_irqsave(&btdev->response_lock, flags);
	if (!btdev->waiting_response || id != btdev->expected_response)
		goto unlock;

	if (btdev->response && param_len < btdev->response_size) {
		btdev->command_result = -EMSGSIZE;
	} else {
		if (btdev->response)
			memcpy(btdev->response, message->param,
			       btdev->response_size);
		btdev->command_result = 0;
	}

	btdev->waiting_response = false;
	/*
	 * Serialize completion with command setup.  In particular, do not
	 * allow a timed out command's completion to race a following command's
	 * reinit_completion().
	 */
	complete(&btdev->command_done);

unlock:
	spin_unlock_irqrestore(&btdev->response_lock, flags);
}

static void aic_bt_parse_rx(struct aic_bt_sdio *btdev, const u8 *data,
			    size_t data_len)
{
	size_t offset = 0;

	while (data_len - offset >= 4) {
		const struct aic_bt_e2a_header *message;
		size_t frame_len;
		size_t frame_size;

		frame_len = get_unaligned_le16(data + offset);
		if (!frame_len)
			break;

		if (check_add_overflow(frame_len, (size_t)4, &frame_size) ||
		    frame_size > data_len - offset)
			break;

		if ((data[offset + 2] & 0x7f) == AIC_CMD_TYPE) {
			message = (const void *)(data + offset + 4);
			aic_bt_complete_response(btdev, message, frame_len);
		}

		frame_size = roundup(frame_len, 4) + 4;
		if (frame_size > data_len - offset)
			break;
		offset += frame_size;
	}
}

static void aic_bt_sdio_irq(struct sdio_func *func)
{
	struct aic_bt_sdio *btdev = sdio_get_drvdata(func);
	u8 *data;
	u8 blocks;
	u8 status;
	size_t data_len;
	int err;

	status = sdio_readb(func, AIC_SDIO_MISC_INT_STATUS, &err);
	if (err) {
		dev_err_ratelimited(&func->dev,
				    "failed to read interrupt status: %d\n", err);
		return;
	}

	if (status & AIC_SDIO_OTHER_INTERRUPT) {
		u8 pending;

		pending = sdio_readb(func, AIC_SDIO_INTR_PENDING, &err);
		if (!err) {
			pending &= ~BIT(0);
			sdio_writeb(func, pending, AIC_SDIO_INTR_PENDING, &err);
		}
		if (err)
			dev_err_ratelimited(&func->dev,
					    "failed to clear soft interrupt: %d\n",
					    err);
	}

	blocks = status & 0x7f;
	if (!blocks)
		return;

	if (blocks == AIC_SDIO_BYTE_MODE_BLOCKS) {
		u8 words;

		words = sdio_readb(func, AIC_SDIO_BYTEMODE_LEN, &err);
		if (err)
			return;
		data_len = (size_t)words * 4;
	} else {
		data_len = (size_t)blocks * AIC_SDIO_BLOCK_SIZE;
	}

	if (!data_len || data_len > AIC_SDIO_RX_MAX_SIZE) {
		dev_err_ratelimited(&func->dev,
				    "invalid SDIO response length %zu\n", data_len);
		return;
	}

	data = btdev->rx_buf;
	err = sdio_readsb(func, data, AIC_SDIO_RD_FIFO, data_len);
	if (err)
		dev_err_ratelimited(&func->dev,
				    "failed to read response: %d\n", err);
	else
		aic_bt_parse_rx(btdev, data, data_len);
}

static int aic_bt_wait_for_credits(struct aic_bt_sdio *btdev, size_t tx_len)
{
	unsigned int retry;
	int err;

	for (retry = 0; retry < AIC_SDIO_FLOW_RETRIES; retry++) {
		u8 credits;

		credits = sdio_readb(btdev->func, AIC_SDIO_FLOW_CTRL_Q1, &err);
		if (err)
			return err;

		if (credits && tx_len <= (size_t)credits * AIC_SDIO_BUFFER_SIZE)
			return 0;

		if (retry < 30)
			usleep_range(30, 50);
		else if (retry < 40)
			usleep_range(1000, 1500);
		else
			usleep_range(10000, 12000);
	}

	return -ETIMEDOUT;
}

static int aic_bt_command(struct aic_bt_sdio *btdev, u16 request_id,
			  const void *param, size_t param_len, u16 response_id,
			  void *response, size_t response_size)
{
	unsigned long flags;
	size_t frame_len;
	size_t message_len;
	size_t tx_len;
	long timeout;
	int err;

	if (param_len > U16_MAX)
		return -EMSGSIZE;

	message_len = 8 + param_len;
	frame_len = 8 + message_len;
	if (frame_len > AIC_SDIO_TX_BUFFER_SIZE)
		return -EMSGSIZE;

	if (IS_ALIGNED(frame_len, AIC_SDIO_BLOCK_SIZE))
		tx_len = frame_len;
	else
		tx_len = roundup(frame_len + AIC_SDIO_FRAME_TAIL_LEN,
				 AIC_SDIO_BLOCK_SIZE);

	if (tx_len > AIC_SDIO_TX_BUFFER_SIZE)
		return -EMSGSIZE;

	mutex_lock(&btdev->command_mutex);
	spin_lock_irqsave(&btdev->response_lock, flags);
	if (btdev->command_timed_out) {
		spin_unlock_irqrestore(&btdev->response_lock, flags);
		err = -ETIMEDOUT;
		goto unlock_command;
	}
	spin_unlock_irqrestore(&btdev->response_lock, flags);

	memset(btdev->tx_buf, 0, tx_len);
	put_unaligned_le16(message_len + 4, btdev->tx_buf);
	btdev->tx_buf[2] = AIC_CMD_TYPE;
	btdev->tx_buf[3] = aic_bt_crc8(btdev->tx_buf, 3);

	put_unaligned_le16(request_id, btdev->tx_buf + 8);
	put_unaligned_le16(AIC_TASK_DBG, btdev->tx_buf + 10);
	put_unaligned_le16(AIC_DRIVER_TASK, btdev->tx_buf + 12);
	put_unaligned_le16(param_len, btdev->tx_buf + 14);
	if (param_len)
		memcpy(btdev->tx_buf + 16, param, param_len);

	spin_lock_irqsave(&btdev->response_lock, flags);
	reinit_completion(&btdev->command_done);
	btdev->expected_response = response_id;
	btdev->response = response;
	btdev->response_size = response_size;
	btdev->command_result = -EINPROGRESS;
	btdev->waiting_response = true;
	spin_unlock_irqrestore(&btdev->response_lock, flags);

	sdio_claim_host(btdev->func);
	err = aic_bt_wait_for_credits(btdev, tx_len);
	if (!err)
		err = sdio_writesb(btdev->func, AIC_SDIO_WR_FIFO,
				   btdev->tx_buf, tx_len);
	sdio_release_host(btdev->func);
	if (err)
		goto clear_response;

	timeout = wait_for_completion_timeout(&btdev->command_done,
					      msecs_to_jiffies(AIC_CMD_TIMEOUT_MS));
	if (!timeout) {
		dev_err(&btdev->func->dev,
			"command 0x%04x timed out waiting for 0x%04x\n",
			request_id, response_id);
		err = -ETIMEDOUT;
		spin_lock_irqsave(&btdev->response_lock, flags);
		btdev->command_timed_out = true;
		spin_unlock_irqrestore(&btdev->response_lock, flags);
		goto clear_response;
	}

	spin_lock_irqsave(&btdev->response_lock, flags);
	err = btdev->command_result;
	btdev->response = NULL;
	btdev->response_size = 0;
	spin_unlock_irqrestore(&btdev->response_lock, flags);
	mutex_unlock(&btdev->command_mutex);

	return err;

clear_response:
	spin_lock_irqsave(&btdev->response_lock, flags);
	btdev->waiting_response = false;
	btdev->response = NULL;
	btdev->response_size = 0;
	spin_unlock_irqrestore(&btdev->response_lock, flags);
unlock_command:
	mutex_unlock(&btdev->command_mutex);

	return err;
}

static int aic_bt_mem_read(struct aic_bt_sdio *btdev, u32 address, u32 *value)
{
	struct aic_bt_mem_read_cfm cfm;
	__le32 request = cpu_to_le32(address);
	int err;

	err = aic_bt_command(btdev, AIC_DBG_MEM_READ_REQ,
			     &request, sizeof(request), AIC_DBG_MEM_READ_CFM,
			     &cfm, sizeof(cfm));
	if (!err)
		*value = le32_to_cpu(cfm.value);

	return err;
}

static int aic_bt_mem_write(struct aic_bt_sdio *btdev, u32 address, u32 value)
{
	__le32 request[2] = {
		cpu_to_le32(address),
		cpu_to_le32(value),
	};

	return aic_bt_command(btdev, AIC_DBG_MEM_WRITE_REQ,
			      request, sizeof(request), AIC_DBG_MEM_WRITE_CFM,
			      NULL, 0);
}

static int aic_bt_mem_block_write(struct aic_bt_sdio *btdev, u32 address,
				  const u8 *data, size_t data_len)
{
	__le32 status;
	u8 *request;
	int err;

	if (data_len > AIC_BT_PATCH_BLOCK_SIZE)
		return -EINVAL;

	request = kzalloc(8 + AIC_BT_PATCH_BLOCK_SIZE, GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	put_unaligned_le32(address, request);
	put_unaligned_le32(data_len, request + 4);
	memcpy(request + 8, data, data_len);

	err = aic_bt_command(btdev, AIC_DBG_MEM_BLOCK_WRITE_REQ,
			     request, 8 + AIC_BT_PATCH_BLOCK_SIZE,
			     AIC_DBG_MEM_BLOCK_WRITE_CFM,
			     &status, sizeof(status));
	kfree(request);
	if (err)
		return err;

	if (le32_to_cpu(status)) {
		dev_err(&btdev->func->dev,
			"firmware rejected block write at 0x%08x\n", address);
		return -EIO;
	}

	return 0;
}

static int aic_bt_upload_firmware(struct aic_bt_sdio *btdev, const char *name,
				  u32 address)
{
	const struct firmware *firmware;
	size_t offset = 0;
	int err;

	err = request_firmware(&firmware, name, &btdev->func->dev);
	if (err)
		return dev_err_probe(&btdev->func->dev, err,
				     "failed to load %s\n", name);

	if (firmware->size > U32_MAX - address) {
		err = -EFBIG;
		goto release;
	}

	while (offset < firmware->size) {
		size_t len = min_t(size_t, AIC_BT_PATCH_BLOCK_SIZE,
				   firmware->size - offset);

		err = aic_bt_mem_block_write(btdev, address + offset,
					     firmware->data + offset, len);
		if (err)
			goto release;
		offset += len;
	}

	dev_dbg(&btdev->func->dev, "loaded %s (%zu bytes) at 0x%08x\n",
		name, firmware->size, address);

release:
	release_firmware(firmware);
	return err;
}

static u32 aic_bt_mode_value(unsigned int pair, u32 firmware_value)
{
	switch (pair) {
	case 0:
		return 1;
	case 1:
		return U32_MAX;
	case 2:
		return 0;
	case 3:
		return AIC_BT_MODE_ONLY_COANT;
	case 4:
		return AIC_BT_PORT_UART;
	case 5:
		return AIC_BT_UART_BAUD;
	case 6:
		return AIC_BT_UART_FLOW_CTRL;
	case 7:
		return AIC_BT_LOW_POWER_ENABLE;
	case 8:
		return AIC_BT_TX_POWER_LEVEL;
	default:
		return firmware_value;
	}
}

static int aic_bt_process_patch_table(struct aic_bt_sdio *btdev,
				      const struct firmware *firmware,
				      bool apply, u32 *adid_addr,
				      u32 *patch_addr)
{
	const u8 *data = firmware->data;
	size_t offset = AIC_BT_PATCH_TAG_SIZE;
	bool have_record = false;
	bool have_info = false;

	if (firmware->size < AIC_BT_PATCH_TAG_SIZE ||
	    memcmp(data, AIC_BT_PATCH_TAG, sizeof(AIC_BT_PATCH_TAG)))
		return -EBADMSG;

	while (offset < firmware->size) {
		const u8 *record;
		const u8 *pairs_data;
		size_t pairs_size;
		unsigned int pair;
		u32 pair_count;
		u32 type;
		int err;

		if (firmware->size - offset <
		    AIC_BT_PATCH_RECORD_HEADER_SIZE)
			return -EBADMSG;

		record = data + offset;
		type = get_unaligned_le32(record + 16);
		pair_count = get_unaligned_le32(record + 20);
		offset += AIC_BT_PATCH_RECORD_HEADER_SIZE;

		if (type >= 1000) {
			pairs_size = 0;
			pair_count = 0;
		} else if (check_mul_overflow((size_t)pair_count,
					      (size_t)AIC_BT_PATCH_PAIR_SIZE,
					      &pairs_size)) {
			return -EOVERFLOW;
		}

		if (pairs_size > firmware->size - offset)
			return -EBADMSG;

		pairs_data = data + offset;
		have_record = true;

		if (type == AIC_BT_PATCH_INFO) {
			if (pair_count < 2)
				return -EBADMSG;

			if (!have_info) {
				if (adid_addr)
					*adid_addr = get_unaligned_le32(pairs_data + 4);
				if (patch_addr)
					*patch_addr = get_unaligned_le32(pairs_data + 12);
				have_info = true;
			}
		}

		if (!apply)
			goto next_record;

		if (type == AIC_BT_PATCH_VERSION) {
			dev_info(&btdev->func->dev, "BT patch version: %.*s\n",
				 (int)min_t(size_t, pairs_size, 80),
				 pairs_data);
			goto next_record;
		}

		if (type == AIC_BT_PATCH_MODE && pair_count < 9)
			return -EBADMSG;

		for (pair = 0; pair < pair_count; pair++) {
			u32 address;
			u32 value;

			address = get_unaligned_le32(pairs_data +
						    pair * AIC_BT_PATCH_PAIR_SIZE);
			value = get_unaligned_le32(pairs_data +
						  pair * AIC_BT_PATCH_PAIR_SIZE + 4);
			if (type == AIC_BT_PATCH_MODE)
				value = aic_bt_mode_value(pair, value);

			err = aic_bt_mem_write(btdev, address, value);
			if (err)
				return err;
		}

		if (type == AIC_BT_PATCH_POWER_ON)
			usleep_range(50, 100);

next_record:
		offset += pairs_size;
	}

	if (!have_record || ((adid_addr || patch_addr) && !have_info))
		return -EBADMSG;

	return 0;
}

static int aic_bt_download_firmware(struct aic_bt_sdio *btdev)
{
	const struct firmware *table;
	u32 chip_id;
	u32 adid_addr;
	u32 patch_addr;
	u8 revision;
	int err;

	err = aic_bt_mem_read(btdev, AIC_BT_CHIP_ID_ADDR, &chip_id);
	if (err)
		return err;

	revision = (chip_id >> 16) & 0x3f;
	if (revision != AIC_BT_CHIP_REV_U02 &&
	    revision != AIC_BT_CHIP_REV_U03) {
		dev_err(&btdev->func->dev,
			"unsupported AIC8800D80 revision %u\n", revision);
		return -ENODEV;
	}

	err = request_firmware(&table, AIC_BT_FW_TABLE, &btdev->func->dev);
	if (err)
		return dev_err_probe(&btdev->func->dev, err,
				     "failed to load %s\n", AIC_BT_FW_TABLE);

	err = aic_bt_process_patch_table(btdev, table, false,
					 &adid_addr, &patch_addr);
	if (err) {
		dev_err(&btdev->func->dev, "invalid BT patch table: %d\n", err);
		goto release_table;
	}

	err = aic_bt_upload_firmware(btdev, AIC_BT_FW_ADID, adid_addr);
	if (err)
		goto release_table;

	err = aic_bt_upload_firmware(btdev, AIC_BT_FW_PATCH, patch_addr);
	if (err)
		goto release_table;

	err = aic_bt_process_patch_table(btdev, table, true, NULL, NULL);
	if (!err)
		dev_info(&btdev->func->dev,
			 "AIC8800D80 revision %u Bluetooth firmware ready\n",
			 revision);

release_table:
	release_firmware(table);
	return err;
}

static int aic_bt_sdio_hw_init(struct aic_bt_sdio *btdev)
{
	struct sdio_func *func = btdev->func;
	struct mmc_host *host = func->card->host;
	u8 io_control;
	int err;

	sdio_claim_host(func);
	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;

	err = sdio_set_block_size(func, AIC_SDIO_BLOCK_SIZE);
	if (err)
		goto release_host;

	err = sdio_enable_func(func);
	if (err)
		goto release_host;
	btdev->function_enabled = true;

	sdio_f0_writeb(func, 0x7f, 0xf2, &err);
	if (err)
		goto disable_func;

	io_control = host->ios.timing == MMC_TIMING_UHS_DDR50 ? 0x20 : 0x00;
	io_control |= BIT(6);
	sdio_f0_writeb(func, io_control, 0xf0, &err);
	if (err)
		goto disable_func;

	sdio_f0_writeb(func, 0x00, 0xf8, &err);
	if (err)
		goto disable_func;

	sdio_f0_writeb(func, 0x00, 0xf1, &err);
	if (err)
		goto disable_func;

	sdio_writeb(func, 1, AIC_SDIO_BYTEMODE_ENABLE, &err);
	if (err)
		goto disable_func;

	err = sdio_claim_irq(func, aic_bt_sdio_irq);
	if (err)
		goto disable_func;
	btdev->irq_claimed = true;

	sdio_f0_writeb(func, 0x07, 0x04, &err);
	if (err)
		goto release_irq;

	sdio_writeb(func, 0x07, AIC_SDIO_INTR_ENABLE, &err);
	if (err)
		goto release_irq;

	sdio_release_host(func);
	return 0;

release_irq:
	sdio_release_irq(func);
	btdev->irq_claimed = false;
disable_func:
	sdio_disable_func(func);
	btdev->function_enabled = false;
release_host:
	sdio_release_host(func);
	return err;
}

static void aic_bt_sdio_hw_deinit(struct aic_bt_sdio *btdev)
{
	sdio_claim_host(btdev->func);

	if (btdev->irq_claimed) {
		int err;

		sdio_writeb(btdev->func, 0, AIC_SDIO_INTR_ENABLE, &err);
		sdio_release_irq(btdev->func);
		btdev->irq_claimed = false;
	}

	if (btdev->function_enabled) {
		sdio_disable_func(btdev->func);
		btdev->function_enabled = false;
	}

	sdio_release_host(btdev->func);
}

static int aic_bt_sdio_probe(struct sdio_func *func,
			     const struct sdio_device_id *id)
{
	struct aic_bt_sdio *btdev;
	int err;

	if (func->num != 1)
		return -ENODEV;

	btdev = devm_kzalloc(&func->dev, sizeof(*btdev), GFP_KERNEL);
	if (!btdev)
		return -ENOMEM;

	btdev->tx_buf = devm_kmalloc(&func->dev, AIC_SDIO_TX_BUFFER_SIZE,
				     GFP_KERNEL);
	if (!btdev->tx_buf)
		return -ENOMEM;

	btdev->rx_buf = devm_kmalloc(&func->dev, AIC_SDIO_RX_MAX_SIZE,
				     GFP_KERNEL);
	if (!btdev->rx_buf)
		return -ENOMEM;

	btdev->func = func;
	mutex_init(&btdev->command_mutex);
	spin_lock_init(&btdev->response_lock);
	init_completion(&btdev->command_done);
	sdio_set_drvdata(func, btdev);

	err = aic_bt_sdio_hw_init(btdev);
	if (err)
		goto clear_drvdata;

	btdev->boot = aic_bt_boot_register(&func->dev);
	if (IS_ERR(btdev->boot)) {
		err = PTR_ERR(btdev->boot);
		btdev->boot = NULL;
		goto deinit_hw;
	}

	err = aic_bt_download_firmware(btdev);
	aic_bt_boot_ready(btdev->boot, err);
	if (err)
		goto unregister_boot;

	return 0;

unregister_boot:
	aic_bt_boot_unregister(btdev->boot);
	btdev->boot = NULL;
deinit_hw:
	aic_bt_sdio_hw_deinit(btdev);
clear_drvdata:
	sdio_set_drvdata(func, NULL);
	return err;
}

static void aic_bt_sdio_remove(struct sdio_func *func)
{
	struct aic_bt_sdio *btdev = sdio_get_drvdata(func);

	aic_bt_boot_unregister(btdev->boot);
	btdev->boot = NULL;
	aic_bt_sdio_hw_deinit(btdev);
	sdio_set_drvdata(func, NULL);
}

static int aic_bt_sdio_suspend(struct device *dev)
{
	struct sdio_func *func = dev_to_sdio_func(dev);
	mmc_pm_flag_t caps;

	caps = sdio_get_host_pm_caps(func);
	if (!(caps & MMC_PM_KEEP_POWER))
		return -EOPNOTSUPP;

	return sdio_set_host_pm_flags(func, MMC_PM_KEEP_POWER);
}

static int aic_bt_sdio_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops aic_bt_sdio_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(aic_bt_sdio_suspend, aic_bt_sdio_resume)
};

static const struct sdio_device_id aic_bt_sdio_ids[] = {
	{ SDIO_DEVICE(AIC_SDIO_VENDOR_ID, AIC_SDIO_DEVICE_ID_8800D80) },
	{ }
};
MODULE_DEVICE_TABLE(sdio, aic_bt_sdio_ids);

static const struct of_device_id aic_bt_sdio_of_match[] = {
	{ .compatible = "aic,aic8800d80-bt-sdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, aic_bt_sdio_of_match);

static struct sdio_driver aic_bt_sdio_driver = {
	.name = "aic_bt_sdio",
	.id_table = aic_bt_sdio_ids,
	.probe = aic_bt_sdio_probe,
	.remove = aic_bt_sdio_remove,
	.drv = {
		.of_match_table = aic_bt_sdio_of_match,
		.pm = &aic_bt_sdio_pm_ops,
	},
};

int aic_bt_sdio_register(void)
{
	return sdio_register_driver(&aic_bt_sdio_driver);
}

void aic_bt_sdio_unregister(void)
{
	sdio_unregister_driver(&aic_bt_sdio_driver);
}

MODULE_FIRMWARE(AIC_BT_FW_ADID);
MODULE_FIRMWARE(AIC_BT_FW_PATCH);
MODULE_FIRMWARE(AIC_BT_FW_TABLE);
