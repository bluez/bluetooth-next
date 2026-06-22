/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  Generic Bluetooth USB driver
 *
 *  Copyright (C) 2005-2008  Marcel Holtmann <marcel@holtmann.org>
 */

#ifndef __BTUSB_H
#define __BTUSB_H

#include <linux/usb.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/gpio/consumer.h>
#include <net/bluetooth/hci_core.h>

/* driver_info flags */
#define BTUSB_IGNORE			BIT(0)
#define BTUSB_DIGIANSWER		BIT(1)
#define BTUSB_CSR			BIT(2)
#define BTUSB_SNIFFER			BIT(3)
#define BTUSB_BCM92035			BIT(4)
#define BTUSB_BROKEN_ISOC		BIT(5)
#define BTUSB_WRONG_SCO_MTU		BIT(6)
#define BTUSB_ATH3012			BIT(7)
#define BTUSB_INTEL_COMBINED		BIT(8)
#define BTUSB_INTEL_BOOT		BIT(9)
#define BTUSB_BCM_PATCHRAM		BIT(10)
#define BTUSB_MARVELL			BIT(11)
#define BTUSB_SWAVE			BIT(12)
#define BTUSB_AMP			BIT(13)
#define BTUSB_QCA_ROME			BIT(14)
#define BTUSB_BCM_APPLE			BIT(15)
#define BTUSB_REALTEK			BIT(16)
#define BTUSB_BCM2045			BIT(17)
#define BTUSB_IFNUM_2			BIT(18)
#define BTUSB_CW6622			BIT(19)
#define BTUSB_MEDIATEK			BIT(20)
#define BTUSB_WIDEBAND_SPEECH		BIT(21)
#define BTUSB_INVALID_LE_STATES		BIT(22)
#define BTUSB_QCA_WCN6855		BIT(23)
#define BTUSB_INTEL_BROKEN_SHUTDOWN_LED	BIT(24)
#define BTUSB_INTEL_BROKEN_INITIAL_NCMD BIT(25)
#define BTUSB_INTEL_NO_WBS_SUPPORT	BIT(26)
#define BTUSB_ACTIONS_SEMI		BIT(27)
#define BTUSB_BARROT			BIT(28)

#define BTUSB_MAX_ISOC_FRAMES	10

/* btusb_data flags */
#define BTUSB_INTR_RUNNING	0
#define BTUSB_BULK_RUNNING	1
#define BTUSB_ISOC_RUNNING	2
#define BTUSB_SUSPENDING	3
#define BTUSB_DID_ISO_RESUME	4
#define BTUSB_BOOTLOADER	5
#define BTUSB_DOWNLOADING	6
#define BTUSB_FIRMWARE_LOADED	7
#define BTUSB_FIRMWARE_FAILED	8
#define BTUSB_BOOTING		9
#define BTUSB_DIAG_RUNNING	10
#define BTUSB_OOB_WAKE_ENABLED	11
#define BTUSB_HW_RESET_ACTIVE	12
#define BTUSB_TX_WAIT_VND_EVT	13
#define BTUSB_WAKEUP_AUTOSUSPEND	14
#define BTUSB_USE_ALT3_FOR_WBS	15
#define BTUSB_ALT6_CONTINUOUS_TX	16
#define BTUSB_HW_SSR_ACTIVE	17

struct btusb_data {
	struct hci_dev       *hdev;
	struct usb_device    *udev;
	struct usb_interface *intf;
	struct usb_interface *isoc;
	struct usb_interface *diag;
	unsigned int isoc_ifnum;

	unsigned long flags;

	bool poll_sync;
	int intr_interval;
	struct work_struct  work;
	struct work_struct  waker;
	struct delayed_work rx_work;

	struct sk_buff_head acl_q;

	struct usb_anchor deferred;
	struct usb_anchor tx_anchor;
	int tx_in_flight;
	spinlock_t txlock;

	struct usb_anchor intr_anchor;
	struct usb_anchor bulk_anchor;
	struct usb_anchor isoc_anchor;
	struct usb_anchor diag_anchor;
	struct usb_anchor ctrl_anchor;
	spinlock_t rxlock;

	struct sk_buff *evt_skb;
	struct sk_buff *acl_skb;
	struct sk_buff *sco_skb;

	struct usb_endpoint_descriptor *intr_ep;
	struct usb_endpoint_descriptor *bulk_tx_ep;
	struct usb_endpoint_descriptor *bulk_rx_ep;
	struct usb_endpoint_descriptor *isoc_tx_ep;
	struct usb_endpoint_descriptor *isoc_rx_ep;
	struct usb_endpoint_descriptor *diag_tx_ep;
	struct usb_endpoint_descriptor *diag_rx_ep;

	struct gpio_desc *reset_gpio;

	__u8 cmdreq_type;
	__u8 cmdreq;

	unsigned int sco_num;
	unsigned int air_mode;
	bool usb_alt6_packet_flow;
	int isoc_altsetting;
	int suspend_count;
	const struct usb_device_id *match_id;

	int (*recv_event)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv_acl)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv_bulk)(struct btusb_data *data, void *buffer, int count);

	int (*setup_on_usb)(struct hci_dev *hdev);

	int (*suspend)(struct hci_dev *hdev);
	int (*resume)(struct hci_dev *hdev);
	int (*disconnect)(struct hci_dev *hdev);

	int oob_wake_irq;   /* irq for out-of-band wake-on-bt */
};

#endif /* __BTUSB_H */
