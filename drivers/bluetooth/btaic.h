/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __BTAIC_H
#define __BTAIC_H

#include <linux/types.h>

struct device;
struct fwnode_handle;

struct aic_bt_boot;

struct aic_bt_boot *aic_bt_boot_register(struct device *dev);
void aic_bt_boot_ready(struct aic_bt_boot *boot, int status);
void aic_bt_boot_unregister(struct aic_bt_boot *boot);

struct aic_bt_boot *aic_bt_boot_get(const struct fwnode_handle *fwnode);
void aic_bt_boot_put(struct aic_bt_boot *boot);
int aic_bt_boot_wait(struct aic_bt_boot *boot, unsigned long timeout);
struct device *aic_bt_boot_device(struct aic_bt_boot *boot);

int aic_bt_sdio_register(void);
void aic_bt_sdio_unregister(void);
int aic_bt_uart_register(void);
void aic_bt_uart_unregister(void);

#endif
