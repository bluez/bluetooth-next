// SPDX-License-Identifier: GPL-2.0
/*
 ******************************************************************************
 *
 * Copyright (C) 2020 AIC semiconductor.
 *
 * @brief AIC8800D80 Bluetooth UART transport
 *
 ******************************************************************************
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serdev.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "btaic.h"
#include "hci_uart.h"

#define AIC_BT_UART_DEFAULT_SPEED	1500000
#define AIC_BT_BOOT_TIMEOUT_MS		10000

#define AIC_BT_TX_ACTIVE		0
#define AIC_BT_TX_WAKEUP		1

struct aic_bt_uart {
	struct serdev_device *serdev;
	struct hci_dev *hdev;
	struct hci_uart hu;
	struct aic_bt_boot *boot;

	struct sk_buff *rx_skb;
	struct sk_buff_head txq;
	struct work_struct tx_work;
	unsigned long tx_state;
};

static const struct h4_recv_pkt aic_bt_recv_pkts[] = {
	{ H4_RECV_ACL,   .recv = hci_recv_frame },
	{ H4_RECV_SCO,   .recv = hci_recv_frame },
	{ H4_RECV_EVENT, .recv = hci_recv_frame },
	{ H4_RECV_ISO,   .recv = hci_recv_frame },
};

static void aic_bt_tx_work(struct work_struct *work)
{
	struct aic_bt_uart *uart = container_of(work, struct aic_bt_uart,
						 tx_work);

	for (;;) {
		struct sk_buff *skb;

		clear_bit(AIC_BT_TX_WAKEUP, &uart->tx_state);

		while ((skb = skb_dequeue(&uart->txq))) {
			int len;

			len = serdev_device_write_buf(uart->serdev, skb->data,
						      skb->len);
			if (len <= 0) {
				skb_queue_head(&uart->txq, skb);
				if (len < 0)
					bt_dev_err(uart->hdev,
						   "UART transmit failed (%d)", len);
				break;
			}

			uart->hdev->stat.byte_tx += len;
			skb_pull(skb, len);
			if (skb->len) {
				skb_queue_head(&uart->txq, skb);
				break;
			}

			switch (hci_skb_pkt_type(skb)) {
			case HCI_COMMAND_PKT:
				uart->hdev->stat.cmd_tx++;
				break;
			case HCI_ACLDATA_PKT:
				uart->hdev->stat.acl_tx++;
				break;
			case HCI_SCODATA_PKT:
				uart->hdev->stat.sco_tx++;
				break;
			}

			kfree_skb(skb);
		}

		if (!test_bit(AIC_BT_TX_WAKEUP, &uart->tx_state))
			break;
	}

	clear_bit(AIC_BT_TX_ACTIVE, &uart->tx_state);
}

static void aic_bt_tx_wakeup(struct aic_bt_uart *uart)
{
	if (test_and_set_bit(AIC_BT_TX_ACTIVE, &uart->tx_state))
		set_bit(AIC_BT_TX_WAKEUP, &uart->tx_state);

	schedule_work(&uart->tx_work);
}

static size_t aic_bt_receive_buf(struct serdev_device *serdev,
				 const u8 *data, size_t count)
{
	struct aic_bt_uart *uart = serdev_device_get_drvdata(serdev);

	uart->rx_skb = h4_recv_buf(&uart->hu, uart->rx_skb, data, count,
				   aic_bt_recv_pkts,
				   ARRAY_SIZE(aic_bt_recv_pkts));
	if (IS_ERR(uart->rx_skb)) {
		bt_dev_err(uart->hdev, "Frame reassembly failed (%ld)",
			   PTR_ERR(uart->rx_skb));
		uart->rx_skb = NULL;
	}

	uart->hdev->stat.byte_rx += count;
	return count;
}

static void aic_bt_write_wakeup(struct serdev_device *serdev)
{
	struct aic_bt_uart *uart = serdev_device_get_drvdata(serdev);

	aic_bt_tx_wakeup(uart);
}

static const struct serdev_device_ops aic_bt_serdev_ops = {
	.receive_buf = aic_bt_receive_buf,
	.write_wakeup = aic_bt_write_wakeup,
};

static int aic_bt_open(struct hci_dev *hdev)
{
	struct aic_bt_uart *uart = hci_get_drvdata(hdev);
	unsigned int actual_speed;
	int err;

	err = aic_bt_boot_wait(uart->boot,
			       msecs_to_jiffies(AIC_BT_BOOT_TIMEOUT_MS));
	if (err) {
		bt_dev_err(hdev, "Bluetooth firmware is not ready (%d)", err);
		return err;
	}

	err = serdev_device_open(uart->serdev);
	if (err)
		goto free_rx;

	actual_speed = serdev_device_set_baudrate(uart->serdev, AIC_BT_UART_DEFAULT_SPEED);
	if (!actual_speed) {
		err = -EIO;
		goto close_serdev;
	}

	serdev_device_set_flow_control(uart->serdev, true);
	return 0;

close_serdev:
	serdev_device_close(uart->serdev);
free_rx:
	kfree_skb(uart->rx_skb);
	uart->rx_skb = NULL;
	return err;
}

static int aic_bt_close(struct hci_dev *hdev)
{
	struct aic_bt_uart *uart = hci_get_drvdata(hdev);

	serdev_device_close(uart->serdev);
	/* serdev_device_close() stops receive callbacks before freeing rx_skb. */
	kfree_skb(uart->rx_skb);
	uart->rx_skb = NULL;
	return 0;
}

static int aic_bt_flush(struct hci_dev *hdev)
{
	struct aic_bt_uart *uart = hci_get_drvdata(hdev);

	serdev_device_write_flush(uart->serdev);
	cancel_work_sync(&uart->tx_work);
	skb_queue_purge(&uart->txq);
	clear_bit(AIC_BT_TX_ACTIVE, &uart->tx_state);
	clear_bit(AIC_BT_TX_WAKEUP, &uart->tx_state);

	return 0;
}

static int aic_bt_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct aic_bt_uart *uart = hci_get_drvdata(hdev);
	u8 pkt_type = hci_skb_pkt_type(skb);
	int err;

	err = skb_cow_head(skb, 1);
	if (err) {
		/* The HCI core frees skb when ->send() returns an error. */
		return err;
	}

	memcpy(skb_push(skb, 1), &pkt_type, sizeof(pkt_type));
	skb_queue_tail(&uart->txq, skb);
	aic_bt_tx_wakeup(uart);

	return 0;
}

static void aic_bt_boot_put_action(void *data)
{
	aic_bt_boot_put(data);
}

static int aic_bt_find_boot_provider(struct device *dev,
				     struct aic_bt_uart *uart)
{
	struct fwnode_handle *fwnode = NULL;
	int err;

	if (device_property_present(dev, "aic,firmware-sdio")) {
		fwnode = fwnode_find_reference(dev_fwnode(dev),
					       "aic,firmware-sdio", 0);
		if (IS_ERR(fwnode))
			return PTR_ERR(fwnode);
	}

	uart->boot = aic_bt_boot_get(fwnode);
	fwnode_handle_put(fwnode);
	if (IS_ERR(uart->boot))
		return dev_err_probe(dev, PTR_ERR(uart->boot),
				     "failed to find Bluetooth SDIO firmware loader\n");

	err = devm_add_action_or_reset(dev, aic_bt_boot_put_action,
				       uart->boot);
	if (err)
		return err;

	if (!device_link_add(dev, aic_bt_boot_device(uart->boot),
			     DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;

	return 0;
}

static int aic_bt_uart_probe(struct serdev_device *serdev)
{
	struct device *dev = &serdev->dev;
	struct aic_bt_uart *uart;
	struct hci_dev *hdev;
	int err;

	uart = devm_kzalloc(dev, sizeof(*uart), GFP_KERNEL);
	if (!uart)
		return -ENOMEM;

	uart->serdev = serdev;
	serdev_device_set_drvdata(serdev, uart);
	serdev_device_set_client_ops(serdev, &aic_bt_serdev_ops);

	err = aic_bt_find_boot_provider(dev, uart);
	if (err)
		return err;

	INIT_WORK(&uart->tx_work, aic_bt_tx_work);
	skb_queue_head_init(&uart->txq);

	hdev = hci_alloc_dev();
	if (!hdev)
		return -ENOMEM;

	uart->hdev = hdev;
	uart->hu.hdev = hdev;
	hdev->bus = HCI_UART;
	hci_set_drvdata(hdev, uart);
	SET_HCIDEV_DEV(hdev, dev);

	hdev->open = aic_bt_open;
	hdev->close = aic_bt_close;
	hdev->flush = aic_bt_flush;
	hdev->send = aic_bt_send_frame;

	err = hci_register_dev(hdev);
	if (err) {
		hci_free_dev(hdev);
		return err;
	}

	return 0;
}

static void aic_bt_uart_remove(struct serdev_device *serdev)
{
	struct aic_bt_uart *uart = serdev_device_get_drvdata(serdev);

	hci_unregister_dev(uart->hdev);
	cancel_work_sync(&uart->tx_work);
	skb_queue_purge(&uart->txq);
	kfree_skb(uart->rx_skb);
	uart->rx_skb = NULL;
	hci_free_dev(uart->hdev);
}

static const struct of_device_id aic_bt_uart_of_match[] = {
	{ .compatible = "aic,aic8800d80-bt" },
	{ }
};
MODULE_DEVICE_TABLE(of, aic_bt_uart_of_match);

static struct serdev_device_driver aic_bt_uart_driver = {
	.probe = aic_bt_uart_probe,
	.remove = aic_bt_uart_remove,
	.driver = {
		.name = "bt_aic_uart",
		.of_match_table = aic_bt_uart_of_match,
	},
};

int aic_bt_uart_register(void)
{
	return serdev_device_driver_register(&aic_bt_uart_driver);
}

void aic_bt_uart_unregister(void)
{
	serdev_device_driver_unregister(&aic_bt_uart_driver);
}
