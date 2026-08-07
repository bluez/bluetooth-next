// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/skbuff.h>

#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_bt.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#define VERSION "0.1"
#define VIRTBT_RX_BUF_SIZE	(HCI_MAX_FRAME_SIZE + 1)

enum {
	VIRTBT_VQ_TX,
	VIRTBT_VQ_RX,
	VIRTBT_NUM_VQS,
};

struct virtio_bluetooth {
	struct virtio_device *vdev;
	struct virtqueue *vqs[VIRTBT_NUM_VQS];
	struct work_struct rx_refill;
	/* Serializes RX virtqueue operations and teardown. */
	spinlock_t rx_lock;
	struct hci_dev *hdev;
	unsigned int rx_buf_count;
	bool stopped;
};

static int virtbt_add_inbuf(struct virtio_bluetooth *vbt, gfp_t gfp)
{
	struct virtqueue *vq = vbt->vqs[VIRTBT_VQ_RX];
	struct scatterlist sg[1];
	struct sk_buff *skb;
	unsigned long flags;
	int err;

	skb = alloc_skb(VIRTBT_RX_BUF_SIZE, gfp);
	if (!skb)
		return -ENOMEM;

	sg_init_one(sg, skb->data, VIRTBT_RX_BUF_SIZE);

	spin_lock_irqsave(&vbt->rx_lock, flags);
	if (vbt->stopped) {
		err = -ESHUTDOWN;
	} else {
		/* The lock serializes RX callback and refill worker producers. */
		err = virtqueue_add_inbuf(vq, sg, 1, skb, GFP_ATOMIC);
		if (!err)
			vbt->rx_buf_count++;
	}
	spin_unlock_irqrestore(&vbt->rx_lock, flags);

	if (err < 0) {
		kfree_skb(skb);
		return err;
	}

	return 0;
}

static int virtbt_open(struct hci_dev *hdev)
{
	return 0;
}

static int virtbt_open_vdev(struct virtio_bluetooth *vbt)
{
	struct virtqueue *vq = vbt->vqs[VIRTBT_VQ_RX];
	int err;

	if (!virtqueue_get_vring_size(vq))
		return -EIO;

	do {
		err = virtbt_add_inbuf(vbt, GFP_KERNEL);
	} while (!err);

	if (!vbt->rx_buf_count)
		return err;

	virtqueue_kick(vq);
	return 0;
}

static int virtbt_close(struct hci_dev *hdev)
{
	return 0;
}

static int virtbt_close_vdev(struct virtio_bluetooth *vbt)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&vbt->rx_lock, flags);
	vbt->stopped = true;
	virtqueue_disable_cb(vbt->vqs[VIRTBT_VQ_RX]);
	spin_unlock_irqrestore(&vbt->rx_lock, flags);

	cancel_work_sync(&vbt->rx_refill);

	for (i = 0; i < ARRAY_SIZE(vbt->vqs); i++) {
		struct virtqueue *vq = vbt->vqs[i];
		struct sk_buff *skb;

		while ((skb = virtqueue_detach_unused_buf(vq)))
			kfree_skb(skb);
		cond_resched();
	}

	spin_lock_irqsave(&vbt->rx_lock, flags);
	vbt->rx_buf_count = 0;
	spin_unlock_irqrestore(&vbt->rx_lock, flags);

	return 0;
}

static int virtbt_flush(struct hci_dev *hdev)
{
	return 0;
}

static int virtbt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct virtio_bluetooth *vbt = hci_get_drvdata(hdev);
	struct scatterlist sg[1];
	int err;

	memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);

	sg_init_one(sg, skb->data, skb->len);
	err = virtqueue_add_outbuf(vbt->vqs[VIRTBT_VQ_TX], sg, 1, skb,
				   GFP_KERNEL);
	if (err) {
		kfree_skb(skb);
		return err;
	}

	virtqueue_kick(vbt->vqs[VIRTBT_VQ_TX]);
	return 0;
}

static int virtbt_setup_zephyr(struct hci_dev *hdev)
{
	struct sk_buff *skb;

	/* Read Build Information */
	skb = __hci_cmd_sync(hdev, 0xfc08, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	/* Bounded print: the backend controls skb->len. */
	if (skb->len > 1) {
		int len = skb->len - 1;

		bt_dev_info(hdev, "%.*s", len, (char *)(skb->data + 1));
		hci_set_fw_info(hdev, "%.*s", len, skb->data + 1);
	}

	kfree_skb(skb);
	return 0;
}

static int virtbt_set_bdaddr_zephyr(struct hci_dev *hdev,
				    const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;

	/* Write BD_ADDR */
	skb = __hci_cmd_sync(hdev, 0xfc06, 6, bdaddr, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	kfree_skb(skb);
	return 0;
}

static int virtbt_setup_intel(struct hci_dev *hdev)
{
	struct sk_buff *skb;

	/* Intel Read Version */
	skb = __hci_cmd_sync(hdev, 0xfc05, 0, NULL, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	kfree_skb(skb);
	return 0;
}

static int virtbt_set_bdaddr_intel(struct hci_dev *hdev, const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;

	/* Intel Write BD Address */
	skb = __hci_cmd_sync(hdev, 0xfc31, 6, bdaddr, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	kfree_skb(skb);
	return 0;
}

static int virtbt_setup_realtek(struct hci_dev *hdev)
{
	struct sk_buff *skb;

	/* Read ROM Version */
	skb = __hci_cmd_sync(hdev, 0xfc6d, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	bt_dev_info(hdev, "ROM version %u", *((__u8 *) (skb->data + 1)));

	kfree_skb(skb);
	return 0;
}

static int virtbt_shutdown_generic(struct hci_dev *hdev)
{
	struct sk_buff *skb;

	/* Reset */
	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	kfree_skb(skb);
	return 0;
}

static void virtbt_rx_handle(struct virtio_bluetooth *vbt, struct sk_buff *skb)
{
	size_t min_hdr;
	__u8 pkt_type;

	pkt_type = *((__u8 *) skb->data);
	skb_pull(skb, 1);

	switch (pkt_type) {
	case HCI_EVENT_PKT:
		min_hdr = sizeof(struct hci_event_hdr);
		break;
	case HCI_ACLDATA_PKT:
		min_hdr = sizeof(struct hci_acl_hdr);
		break;
	case HCI_SCODATA_PKT:
		min_hdr = sizeof(struct hci_sco_hdr);
		break;
	case HCI_ISODATA_PKT:
		min_hdr = sizeof(struct hci_iso_hdr);
		break;
	default:
		kfree_skb(skb);
		return;
	}

	if (skb->len < min_hdr) {
		bt_dev_err_ratelimited(vbt->hdev,
				       "rx pkt_type 0x%02x payload %u < hdr %zu\n",
				       pkt_type, skb->len, min_hdr);
		kfree_skb(skb);
		return;
	}

	hci_skb_pkt_type(skb) = pkt_type;
	hci_recv_frame(vbt->hdev, skb);
}

static void virtbt_rx_refill_work(struct work_struct *work)
{
	struct virtio_bluetooth *vbt = container_of(work,
						    struct virtio_bluetooth, rx_refill);
	bool kick = false;

	while (!virtbt_add_inbuf(vbt, GFP_KERNEL))
		kick = true;

	if (kick)
		virtqueue_kick(vbt->vqs[VIRTBT_VQ_RX]);
}

static void virtbt_tx_done(struct virtqueue *vq)
{
	struct sk_buff *skb;
	unsigned int len;

	while ((skb = virtqueue_get_buf(vq, &len)))
		kfree_skb(skb);
}

static void virtbt_rx_done(struct virtqueue *vq)
{
	struct virtio_bluetooth *vbt = vq->vdev->priv;
	bool cb_enabled = true;
	bool kick = false;

	for (;;) {
		struct sk_buff *skb;
		unsigned int len;
		unsigned long flags;

		spin_lock_irqsave(&vbt->rx_lock, flags);
		if (vbt->stopped) {
			spin_unlock_irqrestore(&vbt->rx_lock, flags);
			return;
		}

		if (cb_enabled) {
			virtqueue_disable_cb(vq);
			cb_enabled = false;
		}

		skb = virtqueue_get_buf(vq, &len);
		if (skb)
			vbt->rx_buf_count--;

		if (!skb) {
			if (kick) {
				spin_unlock_irqrestore(&vbt->rx_lock, flags);
				kick = false;
				virtqueue_kick(vq);
				continue;
			}

			if (virtqueue_enable_cb(vq)) {
				spin_unlock_irqrestore(&vbt->rx_lock, flags);
				return;
			}
			cb_enabled = true;
		}
		spin_unlock_irqrestore(&vbt->rx_lock, flags);

		if (!skb)
			continue;

		if (!len || len > VIRTBT_RX_BUF_SIZE) {
			bt_dev_err_ratelimited(vbt->hdev,
					       "rx reply len %u outside [1, %u]\n",
					       len, VIRTBT_RX_BUF_SIZE);
			kfree_skb(skb);
		} else {
			skb_put(skb, len);
			virtbt_rx_handle(vbt, skb);
		}

		if (virtbt_add_inbuf(vbt, GFP_ATOMIC) < 0)
			schedule_work(&vbt->rx_refill);
		else
			kick = true;
	}
}

static int virtbt_probe(struct virtio_device *vdev)
{
	struct virtqueue_info vqs_info[VIRTBT_NUM_VQS] = {
		[VIRTBT_VQ_TX] = { "tx", virtbt_tx_done },
		[VIRTBT_VQ_RX] = { "rx", virtbt_rx_done },
	};
	struct virtio_bluetooth *vbt;
	struct hci_dev *hdev;
	int err;
	__u8 type;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	type = virtio_cread8(vdev, offsetof(struct virtio_bt_config, type));

	switch (type) {
	case VIRTIO_BT_CONFIG_TYPE_PRIMARY:
		break;
	default:
		return -EINVAL;
	}

	vbt = kzalloc_obj(*vbt);
	if (!vbt)
		return -ENOMEM;

	vdev->priv = vbt;
	vbt->vdev = vdev;

	INIT_WORK(&vbt->rx_refill, virtbt_rx_refill_work);
	spin_lock_init(&vbt->rx_lock);

	err = virtio_find_vqs(vdev, VIRTBT_NUM_VQS, vbt->vqs, vqs_info, NULL);
	if (err)
		return err;

	hdev = hci_alloc_dev();
	if (!hdev) {
		err = -ENOMEM;
		goto failed;
	}

	vbt->hdev = hdev;

	hdev->bus = HCI_VIRTIO;
	hci_set_drvdata(hdev, vbt);

	hdev->open  = virtbt_open;
	hdev->close = virtbt_close;
	hdev->flush = virtbt_flush;
	hdev->send  = virtbt_send_frame;

	if (virtio_has_feature(vdev, VIRTIO_BT_F_VND_HCI)) {
		__u16 vendor;

		if (virtio_has_feature(vdev, VIRTIO_BT_F_CONFIG_V2))
			virtio_cread(vdev, struct virtio_bt_config_v2,
				     vendor, &vendor);
		else
			virtio_cread(vdev, struct virtio_bt_config,
				     vendor, &vendor);

		switch (vendor) {
		case VIRTIO_BT_CONFIG_VENDOR_ZEPHYR:
			hdev->manufacturer = 1521;
			hdev->setup = virtbt_setup_zephyr;
			hdev->shutdown = virtbt_shutdown_generic;
			hdev->set_bdaddr = virtbt_set_bdaddr_zephyr;
			break;

		case VIRTIO_BT_CONFIG_VENDOR_INTEL:
			hdev->manufacturer = 2;
			hdev->setup = virtbt_setup_intel;
			hdev->shutdown = virtbt_shutdown_generic;
			hdev->set_bdaddr = virtbt_set_bdaddr_intel;
			hci_set_quirk(hdev, HCI_QUIRK_STRICT_DUPLICATE_FILTER);
			hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);
			hci_set_quirk(hdev, HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED);
			break;

		case VIRTIO_BT_CONFIG_VENDOR_REALTEK:
			hdev->manufacturer = 93;
			hdev->setup = virtbt_setup_realtek;
			hdev->shutdown = virtbt_shutdown_generic;
			hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);
			hci_set_quirk(hdev, HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED);
			break;
		}
	}

	if (virtio_has_feature(vdev, VIRTIO_BT_F_MSFT_EXT)) {
		__u16 msft_opcode;

		if (virtio_has_feature(vdev, VIRTIO_BT_F_CONFIG_V2))
			virtio_cread(vdev, struct virtio_bt_config_v2,
				     msft_opcode, &msft_opcode);
		else
			virtio_cread(vdev, struct virtio_bt_config,
				     msft_opcode, &msft_opcode);

		hci_set_msft_opcode(hdev, msft_opcode);
	}

	if (virtio_has_feature(vdev, VIRTIO_BT_F_AOSP_EXT))
		hci_set_aosp_capable(hdev);

	if (hci_register_dev(hdev) < 0) {
		hci_free_dev(hdev);
		err = -EBUSY;
		goto failed;
	}

	virtio_device_ready(vdev);
	err = virtbt_open_vdev(vbt);
	if (err)
		goto open_failed;

	return 0;

open_failed:
	hci_free_dev(hdev);
failed:
	vdev->config->del_vqs(vdev);
	return err;
}

static void virtbt_remove(struct virtio_device *vdev)
{
	struct virtio_bluetooth *vbt = vdev->priv;
	struct hci_dev *hdev = vbt->hdev;

	hci_unregister_dev(hdev);
	virtio_reset_device(vdev);
	virtbt_close_vdev(vbt);

	hci_free_dev(hdev);
	vbt->hdev = NULL;

	vdev->config->del_vqs(vdev);
	kfree(vbt);
}

static struct virtio_device_id virtbt_table[] = {
	{ VIRTIO_ID_BT, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

MODULE_DEVICE_TABLE(virtio, virtbt_table);

static const unsigned int virtbt_features[] = {
	VIRTIO_BT_F_VND_HCI,
	VIRTIO_BT_F_MSFT_EXT,
	VIRTIO_BT_F_AOSP_EXT,
	VIRTIO_BT_F_CONFIG_V2,
};

static struct virtio_driver virtbt_driver = {
	.driver.name         = KBUILD_MODNAME,
	.feature_table       = virtbt_features,
	.feature_table_size  = ARRAY_SIZE(virtbt_features),
	.id_table            = virtbt_table,
	.probe               = virtbt_probe,
	.remove              = virtbt_remove,
};

module_virtio_driver(virtbt_driver);

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Generic Bluetooth VIRTIO driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
