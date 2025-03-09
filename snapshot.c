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
 * scrub_index_block - Scrubs an index block by zeroing its contents.
 * @sb: Pointer to the super_block structure.
 * @bno: Block number to be scrubbed.
 *
 * Return: 0 on success, -EINVAL if the block number is invalid, or -EIO if
 *         the block could not be read.
 */

static inline int scrub_index_block(struct super_block *sb, uint32_t bno)
{
	int retval = 0;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);

	if (bno == 0 && !test_bit(bno, sbi->bfree_bitmap)) {
		pr_err("Invalid block number #%u for scrubbing. Block is either free or 0\n",
		       bno);
		return -EINVAL;
	}
	struct buffer_head *bh = sb_bread(sb, bno);

	if (bh) {
		memset(bh->b_data, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
	} else {
		pr_err("Failed to read block #%u for scrubbing.\n", bno);
		retval = -EIO;
	}
	brelse(bh);
	return retval;
}

static int inode_subfile_bitmap_operation(
	struct super_block *sb, struct inode *sub_inode, unsigned long *bitmap,
	void(bitmap_op)(unsigned long *, unsigned int, unsigned int))
{
	if (OUICHEFS_INODE(sub_inode)->index_block == 0) {
		pr_err("Invalid index block\n");
		return -EINVAL;
	}
	struct buffer_head *bh =
		sb_bread(sb, OUICHEFS_INODE(sub_inode)->index_block);

	if (!bh) {
		pr_err("Failed to read block %u\n",
		       OUICHEFS_INODE(sub_inode)->index_block);
		return -EIO;
	}
	struct ouichefs_file_index_block *fblock =
		(struct ouichefs_file_index_block *)bh->b_data;
	for (int i = 0; i < OUICHEFS_FILE_MAX_SUBBLOCKS; i++) {
		if (fblock->blocks[i] == 0)
			continue;
		bitmap_op(bitmap, fblock->blocks[i], 1);
	}
	brelse(bh);
	return 0;
}

static int scrub_inode(struct super_block *sb, uint64_t delete_ino)
{
	struct inode *inode = ouichefs_iget(sb, delete_ino);

	if (!inode) {
		pr_err("Failed to get inode #%llu for scrubbing.\n",
		       delete_ino);
		return -EIO;
	}

	inode_lock(inode);

	uint32_t bno = OUICHEFS_INODE(inode)->index_block;
	// Scrub its index block
	scrub_index_block(sb, bno);

	inode->i_blocks = 0;
	OUICHEFS_INODE(inode)->index_block = 0;
	inode->i_size = 0;
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode->i_mode = 0;
	inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
		0;
	inode->i_ctime.tv_nsec = inode->i_mtime.tv_nsec =
		inode->i_atime.tv_nsec = 0;
	inode_dec_link_count(inode);
	mark_inode_dirty(inode);
	inode_unlock(inode);
	iput(inode);

	put_inode(OUICHEFS_SB(sb), delete_ino);
	put_block(OUICHEFS_SB(sb), bno);

	return 0;
}

/**
 * walk_tree_build_bitmaps - Traverse directory tree and perform operations on bitmaps.
 * @sb: Pointer to the super block structure.
 * @dblock: Pointer to the directory block structure.
 * @inode_bmap: Bitmap of inodes.
 * @block_bmap: Bitmap of blocks.
 * @bitmap_op: function pointer to set what bitmap function should be performed on both bitmaps.
 *
 * This function traverses the directory tree starting from the given
 * directory block and builds bitmaps for inodes and blocks
 *
 * @return: 0 on success.
 */
static int walk_tree_build_bitmaps(
	struct super_block *sb, struct ouichefs_dir_block *dblock,
	unsigned long *inode_bmap, unsigned long *block_bmap,
	void(bitmap_op)(unsigned long *, unsigned int, unsigned int))
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	unsigned long *inode_walk_bmap =
		bitmap_zalloc(sbi->nr_inodes, GFP_KERNEL);

	LIST_HEAD(inode_list);

	struct inode_entry {
		uint32_t ino;
		struct list_head list;
	};

	int retval = 0;

	while (1) {
		for (int i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
			if (dblock->files[i].inode == 0)
				break;
			if (test_bit(dblock->files[i].inode, inode_walk_bmap))
				continue; //inode already in covered
			bitmap_op(inode_bmap, dblock->files[i].inode, 1);
			struct inode *sub_inode =
				ouichefs_iget(sb, dblock->files[i].inode);

			if (OUICHEFS_INODE(sub_inode)->index_block == 0) {
				pr_err("INDEX NODE %lu has index block 0",
				       sub_inode->i_ino);
				iput(sub_inode);
				continue;
			}
			if (S_ISDIR(sub_inode->i_mode)) {
				struct inode_entry *entry =
					kmalloc(sizeof(*entry), GFP_KERNEL);
				entry->ino = sub_inode->i_ino;
				list_add_tail(&entry->list, &inode_list);
			} else if (S_ISREG(sub_inode->i_mode)) {
				inode_subfile_bitmap_operation(
					sb, sub_inode, block_bmap, bitmap_op);
			}
			iput(sub_inode);
		}
		//get first inode from worklist
		//If worklist is empty, were done
		if (list_empty(&inode_list))
			break;

		struct inode_entry *entry =
			list_first_entry(&inode_list, struct inode_entry, list);

		struct inode *next_inode = ouichefs_iget(sb, entry->ino);

		list_del(&entry->list);
		kfree(entry);

		retval = ouichefs_get_dir_block_from_disk(
			sb, OUICHEFS_INODE(next_inode)->index_block, dblock);
		iput(next_inode);
	}

	bitmap_free(inode_walk_bmap);
	return 0;
}

/**
 * ouichefs_scrub_snapshot- Scrub all inodes that are only referenced by the snapshot
 * that is going to be deleted.
 *
 * @param sb Superblock of the partition
 * @param snap snapshot to be scrubbed
 *
 * @return Returns 0 on success, or a negative error code otherwise
 */
static int ouichefs_scrub_snapshot(struct super_block *sb, struct ouichefs_snapshot *snap)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_dir_block *dblock =
		kmalloc(sizeof(struct ouichefs_dir_block), GFP_KERNEL);

	// Get the snapshot directory block
	int retval = ouichefs_get_dir_block_from_disk(sb, snap->bno, dblock);

	if (retval) {
		kfree(dblock);
		return -EIO;
	}

	//Mark all inodes in this bitmap that shall be deleted
	unsigned long *to_delete_inode_bmap =
		bitmap_zalloc(sbi->nr_inodes, GFP_KERNEL);

	//Bitmap of all extra block of file inodes that shall be deleted
	unsigned long *to_delete_block_bmap =
		bitmap_zalloc(sbi->nr_blocks, GFP_KERNEL);

	/* First we walk over the to be deleted snapshot.
	 * Set all inodes and blocks of file inodes in the bitmaps are contained
	 * in this snapshot.
	 */
	walk_tree_build_bitmaps(sb, dblock, to_delete_inode_bmap,
				to_delete_block_bmap, bitmap_set);

	/* Invalidate the snapshot list entry */
	put_block(sbi, snap->bno);
	memset(snap, 0, sizeof(*snap));

	/* Now we walk over all other snapshots,
	 * remove all inodes and blocks that are in other snapshots from the
	 * bitmaps.
	 */
	while ((snap = ouichefs_snap_next(sb, snap)) != NULL) {
		int retval =
			ouichefs_get_dir_block_from_disk(sb, snap->bno, dblock);

		if (retval) {
			retval = -EIO;
			goto cleanup;
		}

		walk_tree_build_bitmaps(sb, dblock, to_delete_inode_bmap,
					to_delete_block_bmap, bitmap_clear);
	}
	// Now scrub all inodes left in the bitmap.
	while (1) {
		uint64_t delete_ino =
			find_first_bit(to_delete_inode_bmap, sbi->nr_inodes);
		if (delete_ino == sbi->nr_inodes)
			break;
		bitmap_clear(to_delete_inode_bmap, delete_ino, 1);
		scrub_inode(sb, delete_ino);
	}

	// Scrub all the blocks on the block bitmap
	while (1) {
		uint32_t delete_bno =
			find_first_bit(to_delete_block_bmap, sbi->nr_blocks);
		if (delete_bno == sbi->nr_blocks)
			break;
		bitmap_clear(to_delete_block_bmap, delete_bno, 1);

		//scrub block
		scrub_index_block(sb, delete_bno);
		put_block(sbi, delete_bno);
	}

cleanup:
	//Clean up all allocated resources
	bitmap_free(to_delete_inode_bmap);
	bitmap_free(to_delete_block_bmap);
	kfree(dblock);

	// Clear caches
	shrink_dcache_parent(sb->s_root);
	evict_inodes(sb);

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

	freeze_super(sb);

	/* remove this snapshots ID from any other snapshots parent_id */
	while ((temp = ouichefs_snap_next(sb, temp)) != NULL) {
		if (temp->parent_id == snap_id)
			temp->parent_id = snap->parent_id;
	}

	ouichefs_scrub_snapshot(sb, snap);

	thaw_super(sb);

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

