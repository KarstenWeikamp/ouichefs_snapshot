// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#include <linux/bitmap.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/printk.h>
#include <linux/time.h>

#include "bitmap.h"
#include "snapshot.h"
#include "ouichefs.h"

/**
 * ouichefs_snap_next - Iterate through saved snapshots of partition
 *
 * To start the iteration, pass NULL to argument snap. Iteration ends
 * when NULL is returned.
 *
 * @param sb superblock of the partition
 * @param snap return value of the previous iteration
 *
 * @return Returns pointer to the next snapshot in the list, or NULL if
 * the end of the list was reached.
 */
struct ouichefs_snapshot *ouichefs_snap_next(struct super_block *sb,
					     struct ouichefs_snapshot *snap)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (sbi->isnap_bitmap == NULL) {
		pr_debug("Partition %s does not support snapshots.\n",
			 sb->s_id);
		return NULL;
	}

	/* function user can start iteration by passing NULL */
	if (snap == NULL)
		snap = sbi->snapshot_list;
	else
		snap++;

	/* loop through array and return first snapshot with non-zero inode number */
	for (; snap < &(sbi->snapshot_list[OUICHEFS_MAX_SNAPSHOTS]); snap++) {
		if (le64_to_cpu(snap->bno) > 0)
			return snap;
	}

	/* return NULL if no further snapshots were found */
	return NULL;
}

/**
 * ouichefs_snap_lookup - Look up a snapshot list entry by its ID
 *
 * To start the iteration, pass NULL to argument snap. Iteration ends
 * when NULL is returned.
 *
 * @param sb superblock of the partition
 * @param snap return value of the previous iteration
 *
 * @return Returns pointer to the next snapshot in the list, or NULL if
 * the end of the list was reached.
 */
struct ouichefs_snapshot *ouichefs_snap_lookup(struct super_block *sb,
					       int snap_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot *snap;

	if (sbi->isnap_bitmap == NULL) {
		pr_debug("Partition %s does not support snapshots.\n",
			 sb->s_id);
		return NULL;
	}

	/*
	 * For the current implementation, where the array index
	 * directly corresponds to the snapshot ID, this implementation
	 * should suffice. It needs to be adapted if the IDs ever become
	 * unhooked from the indices.
	 */

	if (snap_id < 0) {
		pr_debug("Snapshot ID #%i out of range (less than 0).\n",
			 snap_id);
		return NULL;
	}

	if (snap_id >= OUICHEFS_MAX_SNAPSHOTS) {
		pr_debug("Snapshot ID #%i out of range (greater than OUICHEFS_MAX_SNAPSHOTS).\n",
			 snap_id);
		return NULL;
	}

	snap = sbi->snapshot_list + snap_id;

	if (le64_to_cpu(snap->bno) <= 0) {
		pr_debug("Snapshot ID #%i not in use (index block no: %llu).\n",
			 snap_id, le64_to_cpu(snap->bno));
		return NULL;
	}

	return snap;
}

/**
 * ouichefs_get_block_from_disk - Reads a directory block from disk.
 * @sb: Pointer to the super block structure.
 * @index_block: Index of the block to read from disk.
 * @dir_block: Pointer to the directory block structure to be filled with data.
 *
 * This function reads a directory index block from the disk and copies its
 * content into the provided directory block structure.
 *
 * @return: 0 on success, -EIO on error.
 */
static int
ouichefs_get_dir_block_from_disk(struct super_block *sb, uint32_t index_block,
				 struct ouichefs_dir_block *dir_block)
{
	/* Read the directory index block on disk */
	struct buffer_head *bh = sb_bread(sb, index_block);

	if (!bh)
		return -EIO;
	memcpy(dir_block, bh->b_data, sizeof(struct ouichefs_dir_block));
	brelse(bh);
	return 0;
}

/**
 * ouichefs_sync_dir_block_to_disk - Syncs a directory block to disk.
 * @sb: Pointer to the super block structure.
 * @index_block: Index of the block to be synced.
 * @dir_block: Pointer to the directory block to be written to disk.
 *
 * This function reads a block from disk, copies the directory block data
 * into it, marks the buffer as dirty, and then syncs the dirty buffer to disk.
 *
 * @return: 0 on success, -EIO if the block could not be read.
 */
static int ouichefs_sync_dir_block_to_disk(struct super_block *sb,
					   uint32_t index_block,
					   struct ouichefs_dir_block *dir_block)
{
	struct buffer_head *bh = sb_bread(sb, index_block);

	if (!bh)
		return -EIO;

	memcpy(bh->b_data, dir_block, sizeof(struct ouichefs_dir_block));

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	brelse(bh);
	return 0;
}

/**
 * Marks all inodes and blocks as snapshot, thus making them read-only.
 *
 * @param sb Pointer to the superblock of the partition
 */
static void ouichefs_mark_all_snap(struct super_block *sb)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	bitmap_complement(sbi->isnap_bitmap, sbi->ifree_bitmap, sbi->nr_inodes);
	bitmap_complement(sbi->bsnap_bitmap, sbi->bfree_bitmap, sbi->nr_blocks);
}

/**
 * ouichefs_snap_create - Create a snapshot
 *
 * @param sb Superblock of partition on which to create the snapshot
 * @param comment Null-terminated comment to attach to the snapshot
 *
 * @return Returns the positive unique ID of the created snapshot, or a
 * negative error code
 */
int ouichefs_snap_create(struct super_block *sb, int snap_id, const char *comment)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot *snap = NULL;
	struct ouichefs_dir_block *root_block = NULL;
	uint32_t snap_index_block_id = 0;
	int retval = 0;

	/* if the partition does not support snapshots, exit early */
	if (sbi->isnap_bitmap == NULL) {
		pr_debug("Partition %s does not support snapshots.\n",
			 sb->s_id);
		return -EOPNOTSUPP;
	}

	if (snap_id > 0 && snap_id < OUICHEFS_MAX_SNAPSHOTS) {
		snap = &sbi->snapshot_list[snap_id];
		long bno = le64_to_cpu(snap->bno);

		if (bno != 0) {
			/*
			 * the requested snapshot number is in use; we need to find
			 * a new, free one. set id to 0 to start looking for a new
			 * ID in the following loop.
			 */
			snap_id = 0;
			snap = NULL;
		}
	} else {
		snap_id = 0;
	}

	if (snap_id == 0) {
		/* start with 1, reserve 0 for "current version" snapshot */
		for (snap_id = 1; snap_id < OUICHEFS_MAX_SNAPSHOTS; snap_id++) {
			long bno = le64_to_cpu(sbi->snapshot_list[snap_id].bno);

			if (bno == 0) {
				snap = &(sbi->snapshot_list[snap_id]);
				break;
			}
		}
	}

	if (snap == NULL) {
		pr_debug("No free snapshot slots\n");
		return -EMFILE;
	}

	pr_debug("free snapshot slot at #%i\n", snap_id);

	snap_index_block_id = get_free_block(sbi);

	if (snap_index_block_id == 0) {
		pr_err("No free block left on partition, cannot backup!\n");
		return -ENOSPC;
	}

	snap->id = cpu_to_le32(snap_id);
	snap->timestamp = cpu_to_le64(ktime_get_real_seconds());
	snap->bno = cpu_to_le32(snap_index_block_id);
	snap->parent_id = cpu_to_le32(sbi->latest_snapshot);
	memcpy(snap->comment, comment, OUICHEFS_SNAPSHOT_COMMENT_LEN);

	freeze_super(sb);

	struct inode *root_inode = ouichefs_iget(sb, 1); // Get root inode

	if (!root_inode) {
		retval = -EINVAL;
		goto put_block;
	}

	/* Allocate memory for the root block, this needs to be allocated,
	 * otherwise we exceed kernel function stack size.
	 */
	root_block = kzalloc(sizeof(struct ouichefs_dir_block), GFP_KERNEL);

	if (!root_block) {
		retval = -ENOMEM;
		goto put_inode;
	}

	// Copy the index block (directory structure) from root_inode to new_inode
	struct ouichefs_inode_info *root_oi = OUICHEFS_INODE(root_inode);

	// Copy data from the root block to the new block
	retval = ouichefs_get_dir_block_from_disk(sb, root_oi->index_block,
						  root_block);

	if (retval) {
		pr_err("Unable to get root dir block from disk.\n");
		retval = -EIO;
		goto free_root_block;
	}

	ouichefs_mark_all_snap(sb);

	retval = ouichefs_sync_dir_block_to_disk(sb, snap_index_block_id,
						 root_block);

	if (retval) {
		pr_err("Unable to put root dir block on disk.\n");
		retval = -EIO;
		goto free_root_block;
	}

	sbi->latest_snapshot = snap_id;

	kfree(root_block);

	iput(root_inode);
	thaw_super(sb);

	return snap_id;
free_root_block:
	kfree(root_block);
put_inode:
	iput(root_inode);
put_block:
	put_block(sbi, snap_index_block_id);
	thaw_super(sb);
	return retval;
}

/**
 * ouichefs_snap_destroy - Destroy a snapshot
 *
 * @param sb Superblock of partition on which to delete a snapshot
 * @param snap_id unique ID of the snapshot to be destroyed
 *
 * @return Returns 0 on success, or a negative error code otherwise
 */
int ouichefs_snap_destroy(struct super_block *sb, int snap_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot *snap = NULL, *temp = NULL;

	/* if the partition does not support snapshots, exit early */
	if (sbi->isnap_bitmap == NULL) {
		pr_debug("Partition %s does not support snapshots.\n",
			 sb->s_id);
		return -EOPNOTSUPP;
	}

	snap = ouichefs_snap_lookup(sb, snap_id);

	if (snap == NULL) {
		pr_debug("Partition %s destroy: Could not find snap ID #%i\n",
			 sb->s_id, snap_id);
		return -EINVAL;
	}

	put_block(sbi, snap->bno);

	/* remove this snapshots ID from any other snapshots parent_id */
	while ((temp = ouichefs_snap_next(sb, temp)) != NULL) {
		if (temp->parent_id == snap_id)
			temp->parent_id = snap->parent_id;
	}

	/* Invalidate the snapshot list entry */
	memset(snap, 0, sizeof(*snap));

	pr_info("Partition %s: Destroyed snapshot #%i\n",
		sb->s_id, snap_id);

	return 0;
}

/**
 * ouichefs_snap_restore - Restore a snapshot
 *
 * @param sb Superblock of partition on which to restore a snapshot
 * @param snap_id unique ID of the snapshot to be restored
 *
 * @return Returns 0 on success, or a negative error code otherwise
 */
int ouichefs_snap_restore(struct super_block *sb, int snap_id)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_snapshot *snap = NULL;
	int retval = 0;

	/* if the partition does not support snapshots, exit early */
	if (sbi->isnap_bitmap == NULL) {
		pr_debug("Partition %s does not support snapshots.\n",
			 sb->s_id);
		return -EOPNOTSUPP;
	}

	snap = ouichefs_snap_lookup(sb, snap_id);

	if (snap == NULL) {
		pr_debug("Partition %s restore: Could not find snap ID #%i\n",
			 sb->s_id, snap_id);
		return -EINVAL;
	}

	// Write all old dirty data to disk, because we're now gonna detach it
	freeze_super(sb);

	struct inode *root_inode = ouichefs_iget(sb, 1); // Get root inode

	if (!root_inode) {
		pr_err("unable to get root inode!\n");
		retval = -EINVAL;
		goto thaw_superblock;
	}

	// Copy the index block
	struct ouichefs_inode_info *root_inode_info =
		OUICHEFS_INODE(root_inode);

	struct ouichefs_dir_block *snap_dirblock =
		kmalloc(sizeof(struct ouichefs_dir_block), GFP_KERNEL);

	retval = ouichefs_get_dir_block_from_disk(sb, snap->bno, snap_dirblock);
	if (retval) {
		pr_err("Failed to get dir block of snapshot inode from disk.\n");
		retval = -EIO;
		goto error;
	}

	retval = ouichefs_sync_dir_block_to_disk(
		sb, root_inode_info->index_block, snap_dirblock);
	if (retval) {
		pr_err("Failed to write dir block of snapshot to root inode on disk.\n");
		retval = -EIO;
		goto error;
	}

	sbi->latest_snapshot = snap_id;

	kfree(snap_dirblock);

	mark_inode_dirty(root_inode);

	iput(root_inode);

	sync_filesystem(sb);

	thaw_super(sb);

	// Reset dentry cache so filesystem state is consistent.
	shrink_dcache_parent(sb->s_root);
	evict_inodes(sb);

	return 0;
error:
	iput(root_inode);
	kfree(snap_dirblock);
thaw_superblock:
	thaw_super(sb);
	return retval;
}

