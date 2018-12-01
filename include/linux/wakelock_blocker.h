/*
 * Kernel Wakelock Block driver
 *
 * Copyright (c) 2018 Sonsation <jhson9304@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef __LINUX_WAKELOCK_BLOCKER_H
#define __LINUX_WAKELOCK_BLOCKER_H

#include <linux/list.h>

#define MAJOR_VERSION "1.0"
#define MAX_NAME_LEN  255
#define DEFAULT_ENABLED true
#define DEFAULT_LIST "bbd_wake_lock ssp_wake_lock ssp_comm_wake_lock wlan_rx_wake wlan_wake wlan_ctrl_wake NETLINK wlan_txfl_wake bluetooth_timer BT_bt_wake BT_host_wake mmc0_detect"

struct wake_item {
	char wake_name[MAX_NAME_LEN];
	size_t wake_count;
	int len;
	struct list_head list;
};

bool check_wakelock(struct wakeup_source *ws);

#endif
