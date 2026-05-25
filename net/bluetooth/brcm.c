// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 The Asahi Linux Contributors
 */

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "brcm.h"

struct brcm_prio_cmd {
	__le16 handle;
	u8 enable;
} __packed;

int brcm_set_high_priority(struct hci_dev *hdev, struct hci_conn *conn,
			   bool enable)
{
	struct sk_buff *skb;
	struct brcm_prio_cmd cmd;

	if (!hdev->brcm_capable)
		return 0;

	if (conn->brcm_high_prio == enable)
		return 0;

	cmd.handle = cpu_to_le16(conn->handle);
	cmd.enable = !!enable;

	skb = hci_cmd_sync(hdev, 0xfc57, sizeof(cmd), &cmd, HCI_CMD_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	conn->brcm_high_prio = enable;
	kfree_skb(skb);
	return 0;
}
