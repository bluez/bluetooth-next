/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *  Bluetooth support for Realtek devices
 *
 *  Copyright (C) 2015 Endless Mobile, Inc.
 */

#define RTL_FRAG_LEN 252

#define rtl_dev_err(dev, fmt, ...) bt_dev_err(dev, "RTL: " fmt, ##__VA_ARGS__)
#define rtl_dev_warn(dev, fmt, ...) bt_dev_warn(dev, "RTL: " fmt, ##__VA_ARGS__)
#define rtl_dev_info(dev, fmt, ...) bt_dev_info(dev, "RTL: " fmt, ##__VA_ARGS__)
#define rtl_dev_dbg(dev, fmt, ...) bt_dev_dbg(dev, "RTL: " fmt, ##__VA_ARGS__)

#ifndef kzalloc_obj
#define kzalloc_obj(obj) kzalloc(sizeof(obj), GFP_KERNEL)
#endif
#ifndef kmalloc_obj
#define kmalloc_obj(obj) kmalloc(sizeof(obj), GFP_KERNEL)
#endif


#define RTL_VSC_OP_DOWNLOAD_CMD			0xfc20
#define RTL_VSC_OP_READ_VENDER			0xfc61
#define RTL_VSC_OP_WRITE_VENDOR			0xfc62
#define RTL_VSC_OP_READ_ROM_VER			0xfc6d
#define RTL_VSC_OP_READ_CHIP_ID			0xfc6f
#define RTL_VSC_OP_COREDUMP			0xfcff
#define RTL_VSC_OP_CHECK_DOWNLOAD_STATE		0xfdcf
#define RTL_VSC_OP_WDG_RESET_CMD		0xfc8e

#define FW_TYPE_V0		0
#define FW_TYPE_V1		1
#define FW_TYPE_V2		2
#define FW_TYPE_V3_1		3
#define FW_TYPE_V3_2		4
#define is_v3_fw(type)	((type) == FW_TYPE_V3_1 || (type) == FW_TYPE_V3_2)
#define CHIP_ID_V3_BASE		55

struct rtl_chip_type_evt {
	__u8 status;
	__u8 type;
} __packed;

struct rtl_download_cmd {
	__u8 index;
	__u8 data[RTL_FRAG_LEN];
} __packed;

struct rtl_download_response {
	__u8 status;
	__u8 index;
} __packed;

struct rtl_rom_version_evt {
	__u8 status;
	__u8 version;
} __packed;

struct rtl_epatch_header {
	__u8 signature[8];
	__le32 fw_version;
	__le16 num_patches;
} __packed;

struct rtl_vendor_config_entry {
	__le16 offset;
	__u8 len;
	__u8 data[];
} __packed;

struct rtl_vendor_config {
	__le32 signature;
	__le16 total_len;
	__u8 entry[];
} __packed;

struct rtl_epatch_header_v2 {
	__u8   signature[8];
	__u8   fw_version[8];
	__le32 num_sections;
} __packed;

struct rtl_section {
	__le32 opcode;
	__le32 len;
	u8     data[];
} __packed;

struct rtl_section_hdr {
	__le16 num;
	__le16 reserved;
} __packed;

struct rtl_common_subsec {
	__u8   eco;
	__u8   prio;
	__u8   cb[2];
	__le32 len;
	__u8   data[];
};

struct rtl_sec_hdr {
	__u8   eco;
	__u8   prio;
	__u8   key_id;
	__u8   reserved;
	__le32 len;
	__u8   data[];
} __packed;

struct rtl_subsection {
	struct list_head list;
	u32 opcode;
	u32 len;
	u8 prio;
	u8 *data;
};

struct rtl_iovec {
	u8  *data;
	u32 len;
};

struct rtl_vendor_cmd {
	__u8 param[5];
} __packed;

struct rtl_rp_read_chip_id {
	__u8 status;
	__u8 chip_id;
} __packed;

enum {
	REALTEK_ALT6_CONTINUOUS_TX_CHIP,
	REALTEK_DOWNLOADING,

	__REALTEK_NUM_FLAGS,
};

struct rtl_dump_info {
	const char *driver_name;
	char *controller;
	u32  fw_version;
};

struct btrealtek_data {
	DECLARE_BITMAP(flags, __REALTEK_NUM_FLAGS);

	struct rtl_dump_info rtl_dump;
};

#define btrealtek_set_flag(hdev, nr)					\
	do {								\
		struct btrealtek_data *realtek = hci_get_priv((hdev));	\
		set_bit((nr), realtek->flags);				\
	} while (0)

#define btrealtek_get_flag(hdev)					\
	(((struct btrealtek_data *)hci_get_priv(hdev))->flags)

#define btrealtek_wake_up_flag(hdev, nr)				\
	do {								\
		struct btrealtek_data *rtl = hci_get_priv((hdev));	\
		wake_up_bit(rtl->flags, (nr));				\
	} while (0)
#define btrealtek_test_flag(hdev, nr)	test_bit((nr), btrealtek_get_flag(hdev))
#define btrealtek_test_and_clear_flag(hdev, nr)				\
		test_and_clear_bit((nr), btrealtek_get_flag(hdev))
#define btrealtek_wait_on_flag_timeout(hdev, nr, m, to)			\
		wait_on_bit_timeout(btrealtek_get_flag(hdev), (nr), m, to)
#define btrealtek_clear_flag(hdev, nr)					\
		do {							\
			struct btrealtek_data *rtl = hci_get_priv((hdev));	\
			clear_bit((nr), rtl->flags);			\
		} while (0)

struct id_table {
	__u16 match_flags;
	__u16 lmp_subver;
	__u16 hci_rev;
	__u8 hci_ver;
	__u8 hci_bus;
	__u8 chip_type;
	bool config_needed;
	bool has_rom_version;
	bool has_msft_ext;
	char *fw_name;
	char *cfg_name;
	char *hw_info;
};

struct btrtl_device_info {
	const struct id_table *ic_info;
	u8 rom_version;
	u8 *fw_data;
	int fw_len;
	u8 *cfg_data;
	int cfg_len;
	bool drop_fw;
	int project_id;
	u32 opcode;
	u8 fw_type;
	u8 key_id;
	struct list_head patch_subsecs;
	struct list_head patch_images;
};

struct btrtl_enh_ops {
	int (*parse_firmware_v3)(struct hci_dev *hdev,
				 struct btrtl_device_info *btrtl_dev);
	int (*download_firmware_v3)(struct hci_dev *hdev,
				    struct btrtl_device_info *btrtl_dev);
	void (*free_patch_images)(struct btrtl_device_info *btrtl_dev);
	int (*recv_event)(struct hci_dev *hdev, struct sk_buff *skb);
};
/* Symbol exported by btrtl_enh.ko for symbol_get/symbol_put */
extern struct btrtl_enh_ops rtl_enh_ops;





/* Internal functions shared between btrtl_core.c and btrtl_enh.c */
void btrtl_free_patch_images(struct btrtl_device_info *btrtl_dev);
void *rtl_iov_pull_data(struct rtl_iovec *iov, u32 len);
struct sk_buff *btrtl_read_local_version(struct hci_dev *hdev);
int btrtl_read_chip_id(struct hci_dev *hdev, u8 *chip_id);
int rtl_download_firmware(struct hci_dev *hdev, u8 fw_type,
			  const unsigned char *data, int fw_len);
int rtlbt_parse_firmware_v3(struct hci_dev *hdev,
			    struct btrtl_device_info *btrtl_dev);
int rtl_download_firmware_v3(struct hci_dev *hdev,
			     struct btrtl_device_info *btrtl_dev);

#if IS_ENABLED(CONFIG_BT_RTL)

struct btrtl_device_info *btrtl_initialize(struct hci_dev *hdev,
					   const char *postfix);
void btrtl_free(struct btrtl_device_info *btrtl_dev);
int btrtl_download_firmware(struct hci_dev *hdev,
			    struct btrtl_device_info *btrtl_dev);
void btrtl_set_quirks(struct hci_dev *hdev,
		      struct btrtl_device_info *btrtl_dev);
int btrtl_setup_realtek(struct hci_dev *hdev);
int btrtl_shutdown_realtek(struct hci_dev *hdev);
int btrtl_get_uart_settings(struct hci_dev *hdev,
			    struct btrtl_device_info *btrtl_dev,
			    unsigned int *controller_baudrate,
			    u32 *device_baudrate, bool *flow_control);
void btrtl_set_driver_name(struct hci_dev *hdev, const char *driver_name);
int btrtl_recv_event(struct hci_dev *hdev, struct sk_buff *skb);

#else

static inline struct btrtl_device_info *btrtl_initialize(struct hci_dev *hdev,
							 const char *postfix)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline int btrtl_recv_event(struct hci_dev *hdev, struct sk_buff *skb)
{
	return -EOPNOTSUPP;
}

static inline void btrtl_free(struct btrtl_device_info *btrtl_dev)
{
}

static inline int btrtl_download_firmware(struct hci_dev *hdev,
					  struct btrtl_device_info *btrtl_dev)
{
	return -EOPNOTSUPP;
}

static inline void btrtl_set_quirks(struct hci_dev *hdev,
				    struct btrtl_device_info *btrtl_dev)
{
}

static inline int btrtl_setup_realtek(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

static inline int btrtl_shutdown_realtek(struct hci_dev *hdev)
{
	return -EOPNOTSUPP;
}

static inline int btrtl_get_uart_settings(struct hci_dev *hdev,
					  struct btrtl_device_info *btrtl_dev,
					  unsigned int *controller_baudrate,
					  u32 *device_baudrate,
					  bool *flow_control)
{
	return -ENOENT;
}

static inline void btrtl_set_driver_name(struct hci_dev *hdev, const char *driver_name)
{
}

#endif
