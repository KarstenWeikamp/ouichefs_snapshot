/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2025
 *   Celina Sophie Kalus, Karsten Weikamp, Marco Schlicht
 */
#ifndef _OUICHEFS_SNAPSHOT_H
#define _OUICHEFS_SNAPSHOT_H

#include <linux/fs.h>

#include "ouichefs.h"

static inline int is_inode_snapshot(struct super_block *sb, long ino)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = ino / 8, bit_index = ino % 8;

	return (((uint8_t *)sbi->isnap_bitmap)[byte_index] & (1 << bit_index));
}

static inline void set_inode_snapshot_bit(struct super_block *sb, long ino)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = ino / 8, bit_index = ino % 8;

	((uint8_t *)sbi->isnap_bitmap)[byte_index] |= 1 << bit_index;
}

static inline void unset_inode_snapshot_bit(struct super_block *sb, long ino)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = ino / 8, bit_index = ino % 8;

	((uint8_t *)sbi->isnap_bitmap)[byte_index] &= ~(1 << bit_index);
}

static inline int is_block_snapshot(struct super_block *sb, long bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = bno / 8, bit_index = bno % 8;

	return (((uint8_t *)sbi->bsnap_bitmap)[byte_index] & (1 << bit_index));
}

static inline void set_block_snapshot_bit(struct super_block *sb, long bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = bno / 8, bit_index = bno % 8;

	((uint8_t *)sbi->bsnap_bitmap)[byte_index] |= 1 << bit_index;
}

static inline void unset_block_snapshot_bit(struct super_block *sb, long bno)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	const long byte_index = bno / 8, bit_index = bno % 8;

	((uint8_t *)sbi->bsnap_bitmap)[byte_index] &= ~(1 << bit_index);
}

struct ouichefs_snapshot *ouichefs_snap_next(struct super_block *sb,
					     struct ouichefs_snapshot *snap);
struct ouichefs_snapshot *ouichefs_snap_lookup(struct super_block *sb,
					       int snap_id);

int ouichefs_snap_create(struct super_block *sb, const int id, const char *comment);
int ouichefs_snap_destroy(struct super_block *sb, int snap_id);
int ouichefs_snap_restore(struct super_block *sb, int snap_id);

#endif /* _OUICHEFS_SNAPSHOT_H */
