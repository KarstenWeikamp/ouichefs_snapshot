/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _OUICHEFS_SYSFS_H
#define _OUICHEFS_SYSFS_H

#include <linux/fs.h>

int init_sysfs(void);
void deinit_sysfs(void);

int create_ouichefs_partition_snapshot_controls(struct super_block *sb);
int delete_ouichefs_partition_snapshot_controls(const char *dev_name);

#endif /*_OUIVHEFS_SYSFS_H*/
