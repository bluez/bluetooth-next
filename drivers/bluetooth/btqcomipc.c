// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 The Linux Foundation. All rights reserved.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btqca.h"

/** Message header format.
 *
 *        ----------------------------------------------------------------
 * BitPos |    15    | 14 | 13 | 12 | 11 | 10 |  9  |  8  |    7 - 0     |
 *         ---------------------------------------------------------------
 * Field  | long_msg |ACK |        RFU        |  pkt_type |    cmd       |
 *        ----------------------------------------------------------------
 *
 * - long_msg   : If set, indicates that the payload is larger than the
 *                IPC_MSG_PLD_SZ. The payload instead contains a pointer to the
 *                long message buffer in the shared BTSS memory space.
 *
 * - ACK       : Set if sending ACK if required by sending acknowledegement
 *                to sender i.e. send an ack IPC interrupt if set.
 *
 * - RFU        : Reserved for future use.
 *
 * - pkt_type   : IPC Packet Type
 *
 * - cmd        : Contains unique command ID
 */

#define IPC_MSG_HDR_SZ		4
#define IPC_MSG_PLD_SZ		40
#define IPC_TOTAL_MSG_SZ	(IPC_MSG_HDR_SZ + IPC_MSG_PLD_SZ)

/* Message Header */
#define IPC_HDR_LONG_MSG	BIT(15)
#define IPC_HDR_REQ_ACK		BIT(14)
#define IPC_HDR_PKT_TYPE_MASK	GENMASK(9, 8)
#define  IPC_HDR_PKT_TYPE_CUST	0
#define  IPC_HDR_PKT_TYPE_HCI	1
#define  IPC_HDR_PKT_TYPE_AUDIO	2
#define  IPC_HDR_PKT_TYPE_RFU	3
#define IPC_HDR_CMD_MASK	GENMASK(7, 0)

#define IPC_CMD_STOP		1
#define IPC_CMD_SWITCH_TO_UART	2
#define IPC_CMD_PREPARE_DUMP	3
#define IPC_CMD_COLLECT_DUMP	4
#define IPC_CMD_START		5

#define IPC_TX_QSIZE		32

#define	TO_APPS_ADDR(a)		(desc->mem_region + (int)(uintptr_t)a)
#define	TO_BT_ADDR(a)		(a - desc->mem_region)
#define IPC_LBUF_SZ(w, x, y, z)	(((TO_BT_ADDR((void *)w) + w->x) - w->y) / w->z)

#define	GET_NO_OF_BLOCKS(a, b) ((a + b - 1) / b)

#define GET_RX_INDEX_FROM_BUF(x, y)	((x - desc->rx_ctxt->lring_buf) / y)

#define GET_TX_INDEX_FROM_BUF(x, y)	((x - desc->tx_ctxt->lring_buf) / y)

#define IS_RX_MEM_NON_CONTIGIOUS(buf, len, sz)		\
	((buf + len) > (desc->rx_ctxt->lring_buf +	\
	(sz * desc->rx_ctxt->lmsg_buf_cnt)))

#define POWER_CONTROL_DELAY_MS	50

#define BTSS_PAS_ID	0xc

struct long_msg_info {
	__le16 smsg_free_cnt;
	__le16 lmsg_free_cnt;
	u8 ridx;
	u8 widx;
} __packed;

struct ipc_aux_ptr {
	__le32 len;
	__le32 buf;
} __packed;

struct ring_buffer {
	__le16 msg_hdr;
	__le16 len;
	union {
		u8 smsg_data[IPC_MSG_PLD_SZ];
		__le32 lmsg_data;
	} payload;
} __packed;

struct ring_buffer_info {
	__le32 rbuf;
	u8 ring_buf_cnt;
	u8 ridx;
	u8 widx;
	u8 tidx;
	__le32 next;
} __packed;

struct context_info {
	__le16 total_size;
	u8 lmsg_buf_cnt;
	u8 smsg_buf_cnt;
	struct ring_buffer_info sring_buf_info;
	__le32 sring_buf;
	__le32 lring_buf;
	__le32 reserved;
} __packed;

struct qcom_btss {
	struct device *dev;
	struct rproc *rproc;
	struct hci_dev *hdev;

	struct regmap *regmap;
	u32 offset;
	u32 bit;
	int irq;

	void *mem_region;
	phys_addr_t mem_phys;
	phys_addr_t mem_reloc;
	size_t mem_size;

	struct sk_buff_head tx_q;
	struct workqueue_struct *wq;
	struct work_struct work;
	wait_queue_head_t wait_q;
	spinlock_t lock;

	struct context_info *tx_ctxt;
	struct context_info *rx_ctxt;
	struct long_msg_info lmsg_ctxt;

	bool running;
};

static void btqcomipc_update_stats(struct hci_dev *hdev, struct sk_buff *skb);

static void *btss_alloc_lmsg(struct qcom_btss *desc, u32 len,
			     struct ipc_aux_ptr *aux_ptr, bool *is_lbuf_full)
{
	struct device *dev = desc->dev;
	u8 idx, blks, blks_consumed;
	void *ret_ptr;
	u32 lsz;

	if (desc->tx_ctxt->lring_buf == 0) {
		dev_err(dev, "no long message buffer initialized\n");
		return ERR_PTR(-ENODEV);
	}

	lsz = IPC_LBUF_SZ(desc->tx_ctxt, total_size, lring_buf, lmsg_buf_cnt);
	blks = GET_NO_OF_BLOCKS(len, lsz);

	if (!desc->lmsg_ctxt.lmsg_free_cnt ||
			(blks > desc->lmsg_ctxt.lmsg_free_cnt))
		return ERR_PTR(-EAGAIN);

	idx = desc->lmsg_ctxt.widx;

	if ((desc->lmsg_ctxt.widx + blks) > desc->tx_ctxt->lmsg_buf_cnt) {
		blks_consumed = desc->tx_ctxt->lmsg_buf_cnt - idx;
		aux_ptr->len = len - (blks_consumed * lsz);
		aux_ptr->buf = desc->tx_ctxt->lring_buf;
	}

	desc->lmsg_ctxt.widx = (desc->lmsg_ctxt.widx + blks) %
		desc->tx_ctxt->lmsg_buf_cnt;

	desc->lmsg_ctxt.lmsg_free_cnt -= blks;

	if (desc->lmsg_ctxt.lmsg_free_cnt <=
			((desc->tx_ctxt->lmsg_buf_cnt * 20) / 100))
		*is_lbuf_full = true;

	ret_ptr = TO_APPS_ADDR(desc->tx_ctxt->lring_buf) + (idx * lsz);

	return ret_ptr;
}

static struct ring_buffer_info *btss_get_tx_rbuf(struct qcom_btss *desc,
						 bool *is_sbuf_full)
{
	u8 idx;
	struct ring_buffer_info *rinfo;

	for (rinfo = &(desc->tx_ctxt->sring_buf_info);	rinfo != NULL;
		rinfo = (struct ring_buffer_info *)(uintptr_t)(rinfo->next)) {
		idx = (rinfo->widx + 1) % (desc->tx_ctxt->smsg_buf_cnt);

		if (idx != rinfo->tidx) {
			desc->lmsg_ctxt.smsg_free_cnt--;

			if (desc->lmsg_ctxt.smsg_free_cnt <=
				((desc->tx_ctxt->smsg_buf_cnt * 20) / 100))
				*is_sbuf_full = true;

			return rinfo;
		}
	}

	return ERR_PTR(-EAGAIN);
}

static int btss_send(struct qcom_btss *desc, u16 msg_hdr,
		     struct sk_buff *skb)
{
	struct hci_dev *hdev = desc->hdev;
	struct ring_buffer_info *rinfo;
	struct ipc_aux_ptr aux_ptr;
	struct ring_buffer *rbuf;
	bool is_lbuf_full = false;
	bool is_sbuf_full = false;
	u16 hdr = msg_hdr;
	void *ptr_buf;
	u32 len;

	/* Account for HCI packet type as it's not included in the skb payload */
	len = skb->len + 1;
	memset(&aux_ptr, 0, sizeof(struct ipc_aux_ptr));

	if (len > IPC_MSG_PLD_SZ) {
		hdr |= IPC_HDR_LONG_MSG;

		ptr_buf = btss_alloc_lmsg(desc, len,
					  &aux_ptr, &is_lbuf_full);
		if (IS_ERR(ptr_buf)) {
			bt_dev_err(hdev, "long msg buf full");
			hdev->stat.err_tx++;
			return PTR_ERR(ptr_buf);
		}
	}

	rinfo = btss_get_tx_rbuf(desc, &is_sbuf_full);
	if (IS_ERR(rinfo)) {
		bt_dev_err(hdev, "short msg buf full");
		hdev->stat.err_tx++;
		return PTR_ERR(rinfo);
	}

	rbuf = &((struct ring_buffer *)(TO_APPS_ADDR(rinfo->rbuf)))[rinfo->widx];

	if (len > IPC_MSG_PLD_SZ)
		rbuf->payload.lmsg_data = cpu_to_le32(TO_BT_ADDR(ptr_buf));
	else
		ptr_buf = rbuf->payload.smsg_data;

	/* if it's a short message, the aux len and buf are NULL */
	memcpy_toio(ptr_buf, &hci_skb_pkt_type(skb), 1);
	memcpy_toio((u8 *)ptr_buf + 1, skb->data, skb->len - aux_ptr.len);
	if (aux_ptr.buf) {
		memcpy_toio(TO_APPS_ADDR(aux_ptr.buf),
			    (skb->data + (skb->len - aux_ptr.len)), aux_ptr.len);
	}

	if (is_sbuf_full || is_lbuf_full)
		hdr |= IPC_HDR_REQ_ACK;

	rbuf->msg_hdr = cpu_to_le16(hdr);
	rbuf->len = cpu_to_le16(len);

	rinfo->widx = (rinfo->widx + 1) % desc->tx_ctxt->smsg_buf_cnt;

	regmap_set_bits(desc->regmap, desc->offset, BIT(desc->bit));

	return 0;
}

static void btss_process_tx_queue(struct qcom_btss *desc)
{
	struct sk_buff *skb;
	u16 hdr;
	int ret;

	while ((skb = skb_dequeue(&desc->tx_q))) {
		hdr = FIELD_PREP(IPC_HDR_PKT_TYPE_MASK, IPC_HDR_PKT_TYPE_HCI);

		ret = btss_send(desc, hdr, skb);
		if (ret) {
			bt_dev_err(desc->hdev, "Failed to send message");
			skb_queue_head(&desc->tx_q, skb);
			break;
		}

		btqcomipc_update_stats(desc->hdev, skb);
		kfree_skb(skb);
	}
}

static void btss_free_lmsg(struct qcom_btss *desc, u32 lmsg, u16 len)
{
	u8 idx;
	u8 blks;
	u32 lsz = IPC_LBUF_SZ(desc->tx_ctxt, total_size, lring_buf,
				   lmsg_buf_cnt);

	idx = GET_TX_INDEX_FROM_BUF(lmsg, lsz);

	if (idx != desc->lmsg_ctxt.ridx)
		return;

	blks = GET_NO_OF_BLOCKS(len, lsz);

	desc->lmsg_ctxt.ridx  = (desc->lmsg_ctxt.ridx  + blks) %
		desc->tx_ctxt->lmsg_buf_cnt;

	desc->lmsg_ctxt.lmsg_free_cnt += blks;
}

static int btss_send_ctrl(struct qcom_btss *desc, u16 msg_hdr)
{
	struct ring_buffer_info *rinfo;
	struct ring_buffer *rbuf;

	rinfo = btss_get_tx_rbuf(desc, NULL);
	if (IS_ERR(rinfo))
		return PTR_ERR(rinfo);

	rbuf = &((struct ring_buffer *)TO_APPS_ADDR(rinfo->rbuf))[rinfo->widx];
	rbuf->msg_hdr = cpu_to_le16(msg_hdr);
	rbuf->len = 0;

	rinfo->widx = (rinfo->widx + 1) % desc->tx_ctxt->smsg_buf_cnt;

	regmap_set_bits(desc->regmap, desc->offset, BIT(desc->bit));

	return 0;
}

static void btss_recv_cust_frame(struct qcom_btss *desc, u8 cmd)
{
	u16 msg_hdr = 0;
	int ret;

	msg_hdr |= cmd;

	switch (cmd) {
	case IPC_CMD_STOP:
		bt_dev_info(desc->hdev, "BTSS stopped, gracefully stopping APSS IPC");
		break;
	case IPC_CMD_START:
		desc->tx_ctxt = (struct context_info *)((void *)desc->rx_ctxt +
				le16_to_cpu(desc->rx_ctxt->total_size));
		desc->lmsg_ctxt.widx = 0;
		desc->lmsg_ctxt.ridx = 0;
		desc->lmsg_ctxt.smsg_free_cnt = desc->tx_ctxt->smsg_buf_cnt;
		desc->lmsg_ctxt.lmsg_free_cnt = desc->tx_ctxt->lmsg_buf_cnt;
		WRITE_ONCE(desc->running, true);
		wake_up(&desc->wait_q);

		bt_dev_info(desc->hdev, "BTSS started, initializing APSS BT IPC");
		return;
	default:
		bt_dev_err(desc->hdev, "Unsupported CMD ID: %u", cmd);
		return;
	}

	if (unlikely(!READ_ONCE(desc->running))) {
		bt_dev_err(desc->hdev, "BTSS not initialized, no message sent");
		return;
	}

	WRITE_ONCE(desc->running, false);

	ret = btss_send_ctrl(desc, msg_hdr);
	if (ret)
		bt_dev_err(desc->hdev, "Failed to send control message");
}

static inline int btss_recv_hci_frame(struct qcom_btss *desc, const u8 *data, size_t len)
{
	unsigned char pkt_type;
	struct sk_buff *skb;
	size_t pkt_len;

	if (len < 1)
		return -EPROTO;

	pkt_type = data[0];

	switch (pkt_type) {
	case HCI_EVENT_PKT:
	{
		if (len < HCI_EVENT_HDR_SIZE)
			return -EILSEQ;
		struct hci_event_hdr *hdr = (struct hci_event_hdr *)(data + 1);

		pkt_len = HCI_EVENT_HDR_SIZE + hdr->plen;
		break;
	}
	case HCI_COMMAND_PKT: {
		if (len < HCI_COMMAND_HDR_SIZE)
			return -EILSEQ;
		struct hci_command_hdr *hdr = (struct hci_command_hdr *)(data + 1);

		pkt_len = HCI_COMMAND_HDR_SIZE + le16_to_cpu(hdr->plen);
		break;
	}
	case HCI_ACLDATA_PKT:
	{
		if (len < HCI_ACL_HDR_SIZE)
			return -EILSEQ;
		struct hci_acl_hdr *hdr = (struct hci_acl_hdr *)(data + 1);

		pkt_len = HCI_ACL_HDR_SIZE + le16_to_cpu(hdr->dlen);
		break;
	}
	case HCI_SCODATA_PKT:
	{
		if (len < HCI_SCO_HDR_SIZE)
			return -EILSEQ;
		struct hci_sco_hdr *hdr = (struct hci_sco_hdr *)(data + 1);

		pkt_len = HCI_SCO_HDR_SIZE + hdr->dlen;
		break;
	}
	default:
		return -EPROTO;
	}

	if (pkt_len > len)
		return -EINVAL;

	skb = bt_skb_alloc(pkt_len, GFP_ATOMIC);
	if (!skb) {
		desc->hdev->stat.err_rx++;
		return -ENOMEM;
	}

	skb->dev = (void *)desc->hdev;
	hci_skb_pkt_type(skb) = pkt_type;
	skb_put_data(skb, data + 1, pkt_len);

	hci_recv_frame(desc->hdev, skb);
	desc->hdev->stat.byte_rx += pkt_len;

	return 0;
}

static inline int btss_process_rx(struct qcom_btss *desc,
				  struct ring_buffer_info *rinfo,
				  bool *ack, u8 *rx_count)
{
	u8 ridx, lbuf_idx, blks_consumed, pkt_type, cmd;
	struct ipc_aux_ptr aux_ptr;
	struct ring_buffer *rbuf;
	uint8_t *rxbuf = NULL;
	unsigned char *buf;
	u32 lsz;
	int ret;

	ridx = rinfo->ridx;

	while (ridx != rinfo->widx) {
		memset(&aux_ptr, 0, sizeof(struct ipc_aux_ptr));

		rbuf = &((struct ring_buffer *)(TO_APPS_ADDR(rinfo->rbuf)))[ridx];

		if (rbuf->msg_hdr & IPC_HDR_LONG_MSG) {
			rxbuf = TO_APPS_ADDR(rbuf->payload.lmsg_data);
			lsz = IPC_LBUF_SZ(desc->rx_ctxt, total_size, lring_buf,
				   lmsg_buf_cnt);

			if (IS_RX_MEM_NON_CONTIGIOUS(rbuf->payload.lmsg_data,
						     rbuf->len, lsz)) {
				lbuf_idx = GET_RX_INDEX_FROM_BUF(
						rbuf->payload.lmsg_data, lsz);

				blks_consumed = desc->rx_ctxt->lmsg_buf_cnt -
					lbuf_idx;
				aux_ptr.len = rbuf->len - (blks_consumed * lsz);
				aux_ptr.buf = desc->rx_ctxt->lring_buf;
			}
		} else {
			rxbuf = rbuf->payload.smsg_data;
		}

		*ack = (rbuf->msg_hdr & IPC_HDR_REQ_ACK);

		pkt_type = FIELD_GET(IPC_HDR_PKT_TYPE_MASK, rbuf->msg_hdr);

		switch (pkt_type) {
		case IPC_HDR_PKT_TYPE_HCI:
			buf = kmalloc(rbuf->len, GFP_ATOMIC);
			if (!buf) {
				rinfo->ridx = ridx;
				return -ENOMEM;
			}

			memcpy_fromio(buf, rxbuf, rbuf->len - aux_ptr.len);

			if (aux_ptr.buf)
				memcpy_fromio(buf + (rbuf->len - aux_ptr.len),
					      TO_APPS_ADDR(aux_ptr.buf), aux_ptr.len);

			ret = btss_recv_hci_frame(desc, buf, rbuf->len);
			if (ret)
				bt_dev_err(desc->hdev, "Failed to process HCI frame: %d", ret);
			kfree(buf);
			break;
		case IPC_HDR_PKT_TYPE_CUST:
			cmd = FIELD_GET(IPC_HDR_CMD_MASK, rbuf->msg_hdr);
			btss_recv_cust_frame(desc, cmd);
			break;
		default:
			break;
		}

		ridx = (ridx + 1) % rinfo->ring_buf_cnt;

		if (rx_count)
			(*rx_count)++;

		rinfo->ridx = ridx;
	}

	return 0;
}

static void btss_process_ack(struct qcom_btss *desc)
{
	struct ring_buffer_info *rinfo;
	struct ring_buffer *rbuf;
	u8 tidx;

	for (rinfo = &desc->tx_ctxt->sring_buf_info; rinfo != NULL;
		rinfo = (struct ring_buffer_info *)(uintptr_t)(rinfo->next)) {
		tidx = rinfo->tidx;
		rbuf = (struct ring_buffer *)TO_APPS_ADDR(rinfo->rbuf);

		while (tidx != rinfo->ridx) {
			if (rbuf[tidx].msg_hdr & IPC_HDR_LONG_MSG) {
				btss_free_lmsg(desc,
					       rbuf[tidx].payload.lmsg_data,
					       rbuf[tidx].len);
			}

			tidx = (tidx + 1) % desc->tx_ctxt->smsg_buf_cnt;
			desc->lmsg_ctxt.smsg_free_cnt++;
		}

		rinfo->tidx = tidx;

		btss_process_tx_queue(desc);
	}
}

static void btss_worker(struct work_struct *work)
{
	struct qcom_btss *desc = container_of(work, struct qcom_btss, work);
	struct ring_buffer_info *rinfo;
	bool ack = false;
	u32 offset;
	int ret;

	if (desc->rproc->state != RPROC_RUNNING)
		return;

	spin_lock(&desc->lock);

	if (unlikely(!READ_ONCE(desc->running))) {
		// FW sets offset of RX context info at start of memory region upon boot
		offset = readl(desc->mem_region);
		dev_dbg(desc->dev, "offset after M0 boot: 0x%08x\n", offset);
		desc->rx_ctxt = (struct context_info *)(desc->mem_region + offset);
	} else {
		btss_process_ack(desc);
	}

	for (rinfo = &(desc->rx_ctxt->sring_buf_info);
	     rinfo != NULL;
	     rinfo = (struct ring_buffer_info *)(uintptr_t)(rinfo->next)) {
		ret = btss_process_rx(desc, rinfo, &ack,
				      &desc->rx_ctxt->smsg_buf_cnt);
		if (ret) {
			bt_dev_err(desc->hdev, "Failed to process peer msgs: %d", ret);
			goto spin_unlock;
		}
	}

	if (ack)
		regmap_set_bits(desc->regmap, desc->offset, BIT(desc->bit));

spin_unlock:
	spin_unlock(&desc->lock);
}

static irqreturn_t btss_irq_handler(int irq, void *data)
{
	struct qcom_btss *desc = data;

	queue_work(desc->wq, &desc->work);

	return IRQ_HANDLED;
}

static int btss_init(struct qcom_btss *desc)
{
	struct device *dev = desc->dev;
	int ret;

	init_waitqueue_head(&desc->wait_q);
	spin_lock_init(&desc->lock);
	skb_queue_head_init(&desc->tx_q);

	desc->wq = create_singlethread_workqueue("btss_wq");
	if (!desc->wq) {
		dev_err(dev, "Failed to initialize workqueue\n");
		return -EAGAIN;
	}

	INIT_WORK(&desc->work, btss_worker);

	ret = devm_request_threaded_irq(dev, desc->irq, NULL, btss_irq_handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"btss_irq", desc);

	if (ret)
		dev_err(dev, "error registering irq[%d] ret = %d\n",
			desc->irq, ret);

	return ret;
}

static void btqcomipc_update_stats(struct hci_dev *hdev, struct sk_buff *skb)
{
	u8 pkt_type = hci_skb_pkt_type(skb);

	hdev->stat.byte_tx += skb->len;
	switch (pkt_type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	default:
		break;
	}
}

static int btqcomipc_open(struct hci_dev *hdev)
{
	struct qcom_btss *desc = hci_get_drvdata(hdev);

	int ret;

	ret = btss_init(desc);
	if (ret) {
		bt_dev_err(hdev, "Failed initializing BTSS: %d", ret);
		return ret;
	}

	/* wait 2 seconds for filesystem to become available */
	msleep(2000);

	/* Boot M0 firmware */
	ret = rproc_boot(desc->rproc);
	if (ret) {
		bt_dev_err(hdev, "Failed to boot M0 processor: %d", ret);
		return ret;
	}

	msleep(POWER_CONTROL_DELAY_MS);
	ret = wait_event_timeout(desc->wait_q, READ_ONCE(desc->running),
				 msecs_to_jiffies(1000));

	if (!ret) {
		bt_dev_err(hdev, "Timeout waiting for BTSS start");
		return -ETIMEDOUT;
	}

	return 0;
}

static int btqcomipc_close(struct hci_dev *hdev)
{
	struct qcom_btss *desc = hci_get_drvdata(hdev);

	rproc_shutdown(desc->rproc);
	msleep(POWER_CONTROL_DELAY_MS);

	return 0;
}

static int btqcomipc_shutdown(struct hci_dev *hdev)
{
	struct qcom_btss *desc = hci_get_drvdata(hdev);

	WRITE_ONCE(desc->running, false);
	if (desc->wq != NULL) {
		disable_irq(desc->irq);
		flush_workqueue(desc->wq);
		devm_free_irq(desc->dev, desc->irq, desc);
		skb_queue_purge(&desc->tx_q);
		destroy_workqueue(desc->wq);
		desc->wq = NULL;
	}

	return 0;
}

static int btqcomipc_setup(struct hci_dev *hdev)
{
	struct qca_btsoc_version ver;
	int ret;

	/*
	 * Enable controller to do both LE scan and BR/EDR inquiry
	 * simultaneously.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);

	/*
	 * Enable NON_PERSISTENT_SETUP QUIRK to ensure to execute
	 * setup for every hci up.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
	ret = qca_read_soc_version(hdev, &ver, QCA_IPQ5018);
	if (ret)
		return -EINVAL;

	ret = qca_uart_setup(hdev, 0, QCA_IPQ5018, ver, NULL);
	if (ret) {
		bt_dev_err(hdev, "Failed to setup UART: %d\n", ret);
		return ret;
	}

	bt_dev_info(hdev, "QCA Build Info: %s", hdev->fw_info);

	/* Obtain and set BD address from NVMEM cell */
	hci_set_quirk(hdev, HCI_QUIRK_USE_BDADDR_NVMEM);
	hci_set_quirk(hdev, HCI_QUIRK_BDADDR_NVMEM_BE);

	return 0;
}

static int btqcomipc_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	u16 hdr = FIELD_PREP(IPC_HDR_PKT_TYPE_MASK, IPC_HDR_PKT_TYPE_HCI);
	struct qcom_btss *desc = hci_get_drvdata(hdev);
	int ret;

	if (unlikely(!READ_ONCE(desc->running))) {
		bt_dev_err(hdev, "BTSS not initialized, failed to send message");
		ret = -ENODEV;
		goto free_skb;
	}

	ret = btss_send(desc, hdr, skb);
	if (ret) {
		if (ret == -EAGAIN) {
			if (skb_queue_len(&desc->tx_q) >= IPC_TX_QSIZE) {
				bt_dev_err(hdev, "TX queue full, dropping message");
				hdev->stat.err_tx++;
				ret = -ENOBUFS;
			} else {
				skb_queue_tail(&desc->tx_q, skb);
				return 0;
			}
		} else {
			bt_dev_err(hdev, "Failed to send message: %d", ret);
			hdev->stat.err_tx++;
		}
	}

	btqcomipc_update_stats(desc->hdev, skb);

free_skb:
	kfree_skb(skb);

	return ret;
}

static int btqcomipc_flush(struct hci_dev *hdev)
{
	struct qcom_btss *desc = hci_get_drvdata(hdev);

	skb_queue_purge(&desc->tx_q);
	return 0;
}

static int btqcomipc_alloc_memory_region(struct qcom_btss *desc)
{
	struct device *dev = desc->dev;
	struct resource res;
	int ret;

	/* lookup reserved-memory region of the remoteproc node */
	ret = of_reserved_mem_region_to_resource(desc->rproc->dev.parent->of_node, 0, &res);
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

static void btqcomipc_rproc_put(void *data)
{
	struct rproc *rproc = data;

	rproc_put(rproc);
}

static int btqcomipc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qcom_btss *desc;
	phandle rproc_phandle;
	struct hci_dev *hdev;
	unsigned int args[2];
	int ret;

	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		return -ENOMEM;

	desc->dev = dev;

	if (of_property_read_u32(dev->of_node, "qcom,rproc", &rproc_phandle))
		return dev_err_probe(dev, -ENOENT, "Failed to get remoteproc handle\n");

	desc->rproc = rproc_get_by_phandle(rproc_phandle);
	if (!desc->rproc)
		return dev_err_probe(dev, -EPROBE_DEFER, "Failed to get remoteproc\n");

	devm_add_action_or_reset(dev, btqcomipc_rproc_put, desc->rproc);

	ret = btqcomipc_alloc_memory_region(desc);
	if (ret)
		return ret;

	desc->regmap = syscon_regmap_lookup_by_phandle_args(dev->of_node, "qcom,ipc", 2, args);
	if (IS_ERR(desc->regmap))
		return PTR_ERR(desc->regmap);

	desc->offset = args[0];
	desc->bit = args[1];

	desc->irq = platform_get_irq(pdev, 0);
	if (desc->irq < 0)
		return dev_err_probe(dev, desc->irq, "Failed to acquire IRQ\n");

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	hci_set_drvdata(hdev, desc);
	desc->hdev = hdev;
	SET_HCIDEV_DEV(hdev, &pdev->dev);
	hdev->bus = HCI_IPC;

	hdev->open = btqcomipc_open;
	hdev->close = btqcomipc_close;
	hdev->shutdown = btqcomipc_shutdown;
	hdev->setup = btqcomipc_setup;
	hdev->send = btqcomipc_send;
	hdev->flush = btqcomipc_flush;
	hdev->set_bdaddr = qca_set_bdaddr;

	ret = hci_register_dev(hdev);
	if (ret < 0) {
		hci_free_dev(hdev);
		return dev_err_probe(dev, -EBUSY, "Failed to register hdev\n");
	}

	platform_set_drvdata(pdev, desc);

	return 0;
}

static void btqcomipc_remove(struct platform_device *pdev)
{
	struct qcom_btss *desc = platform_get_drvdata(pdev);

	if (!desc || !desc->hdev)
		return;

	hci_unregister_dev(desc->hdev);
	hci_free_dev(desc->hdev);
}

static const struct of_device_id btqcomipc_of_match[] = {
	{ .compatible = "qcom,ipq5018-bt" },
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, btqcomipc_of_match);

static struct platform_driver btqcomipc_driver = {
	.probe = btqcomipc_probe,
	.remove = btqcomipc_remove,
	.driver = {
		.name = "btqcomipc",
		.of_match_table = btqcomipc_of_match,
	},
};

module_platform_driver(btqcomipc_driver);

MODULE_DESCRIPTION("Qualcomm Bluetooth IPC Driver");
MODULE_LICENSE("GPL");
