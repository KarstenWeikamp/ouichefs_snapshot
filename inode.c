// SPDX-License-Identifier: GPL-2.0
/*
 * ouiche_fs - a simple educational filesystem for Linux
 *
 * Copyright (C) 2018 Redha Gouicem <redha.gouicem@lip6.fr>
 */
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/printk.h>

#include "ouichefs.h"
#include "bitmap.h"
#include "snapshot.h"

#define OUICHEFS_INODE_STORE 1
#define OUICHEFS_INODE_BLOCK(ino) \
	((ino / OUICHEFS_INODES_PER_BLOCK) + OUICHEFS_INODE_STORE)
#define OUICHEFS_INODE_SHIFT(ino) \
	(ino % OUICHEFS_INODES_PER_BLOCK)

static const struct inode_operations ouichefs_inode_ops;

void ouichefs_inode_init_owner(struct mnt_idmap *idmap, struct inode *inode,
		      const struct inode *dir, umode_t mode)
{
	struct ouichefs_inode_info *ci = OUICHEFS_INODE(inode);

	inode_init_owner(idmap, inode, dir, mode);

	if (dir == NULL || inode->i_sb != dir->i_sb) {
		ci->parent = NULL;
		return;
	}

	ci->parent = OUICHEFS_INODE(dir);
}

/*
 * Get inode ino from disk.
 */
struct inode *ouichefs_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode = NULL;
	struct ouichefs_inode *cinode = NULL;
	struct ouichefs_inode_info *ci = NULL;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct buffer_head *bh = NULL;
	uint32_t inode_block = OUICHEFS_INODE_BLOCK(ino);
	uint32_t inode_shift = OUICHEFS_INODE_SHIFT(ino);
	int ret;

	/* Fail if ino is out of range */
	if (ino >= sbi->nr_inodes)
		return ERR_PTR(-EINVAL);

	/* Get a locked inode from Linux */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	/* If inode is in cache, return it */
	if (!(inode->i_state & I_NEW))
		return inode;

	ci = OUICHEFS_INODE(inode);
	/* Read inode from disk and initialize */
	bh = sb_bread(sb, inode_block);
	if (!bh) {
		ret = -EIO;
		goto failed;
	}
	cinode = (struct ouichefs_inode *)bh->b_data;
	cinode += inode_shift;

	inode->i_ino = ino;
	inode->i_sb = sb;
	inode->i_op = &ouichefs_inode_ops;

	inode->i_mode = le32_to_cpu(cinode->i_mode);
	i_uid_write(inode, le32_to_cpu(cinode->i_uid));
	i_gid_write(inode, le32_to_cpu(cinode->i_gid));
	inode->i_size = le32_to_cpu(cinode->i_size);
	inode->i_ctime.tv_sec = (time64_t)le32_to_cpu(cinode->i_ctime);
	inode->i_ctime.tv_nsec = (long)le64_to_cpu(cinode->i_nctime);
	inode->i_atime.tv_sec = (time64_t)le32_to_cpu(cinode->i_atime);
	inode->i_atime.tv_nsec = (long)le64_to_cpu(cinode->i_natime);
	inode->i_mtime.tv_sec = (time64_t)le32_to_cpu(cinode->i_mtime);
	inode->i_mtime.tv_nsec = (long)le64_to_cpu(cinode->i_nmtime);
	inode->i_blocks = le32_to_cpu(cinode->i_blocks);
	set_nlink(inode, le32_to_cpu(cinode->i_nlink));

	ci->index_block = le32_to_cpu(cinode->index_block);

	if (S_ISDIR(inode->i_mode)) {
		inode->i_fop = &ouichefs_dir_ops;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
	}

	brelse(bh);

	/* Unlock the inode to make it usable */
	unlock_new_inode(inode);

	return inode;

failed:
	brelse(bh);
	iget_failed(inode);
	return ERR_PTR(ret);
}

static int ouichefs_index_replace(struct ouichefs_dir_block *index, long old_ino,
				  long new_ino)
{
	for (int i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		if (le32_to_cpu(index->files[i].inode) == old_ino) {
			index->files[i].inode = cpu_to_le32(new_ino);
			return i;
		}
	}

	pr_err("Could not find inode #%li to replace it with #%li!\n",
	       old_ino, new_ino);
	return -1;
}

int ouichefs_inode_ensure_writeable(struct super_block *sb, struct inode *inode)
{
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct ouichefs_inode_info *inode_inf;
	struct buffer_head *bh = NULL;
	struct ouichefs_dir_block *dblock;
	long old_ino, old_index, new_index, child_old_ino = 0, child_new_ino = 0;
	int ret;

	/*
	 * Any path from a file or directory to the root will be one of
	 * three things:
	 *
	 * - only snapshot inodes, which are read-only,
	 * - only non-snapshot inodes, or
	 * - a sequence of snapshot inodes followed by non-snapshot
	 *   inodes
	 *
	 * If the file/directory we want to change is a snapshot inode,
	 * we need to walk up from the inode until we reach either a
	 * non-snapshot inode (which we can write without a problem) or
	 * we reach the root, and for each snapshot inode, create a copy
	 * which is a non-snapshot inode.
	 */

	pr_debug("Starting ensure_writeable at inode #%li\n", inode->i_ino);

	dblock = kzalloc(OUICHEFS_BLOCK_SIZE, GFP_KERNEL);

	while (1) {
		if (inode == NULL) {
			/* this should NOT happen. */
			pr_err("Inode is null!\n");
			return -ENODEV;
		}

		if (inode->i_sb != sb) {
			/* we went past the root; we can stop. not an error. */
			pr_warn("Went past root!\n");
			break;
		}

		inode_inf = OUICHEFS_INODE(inode);
		old_index = inode_inf->index_block;

		/* create a copy of the index block */
		if (inode->i_ino == 1 || is_inode_snapshot(sb, inode->i_ino) == 0) {
			/* reaching a non-snapshot inode or the file system root
			 * means we no longer need to keep iterating, but if we
			 * have modified some inode previously, we still need to
			 * update the reference in this inode's index block.
			 */
			new_index = old_index;
		} else {
			new_index = get_free_block(sbi);
		}

		if (new_index <= 0) {
			pr_err("Could not create an index for cow\n");
			ret = -ENOSPC;
			break;
		}

		bh = sb_bread(sb, inode_inf->index_block);
		if (!bh) {
			pr_err("Could not read old index block.\n");
			ret = -EIO;
			break;
		}

		memcpy(dblock, bh->b_data, OUICHEFS_BLOCK_SIZE);
		brelse(bh);

		/* set the inode number of the child, which we
		 * previously changed, properly
		 */
		if (child_old_ino != 0)
			ret = ouichefs_index_replace(dblock, child_old_ino, child_new_ino);
		else
			ret = 0;

		if (ret < 0) {
			pr_err("Something went wrong while replacing index.\n");
			break;
		}

		ret = write_inode_now(inode, 1);
		if (ret != 0) {
			pr_err("Unable to write inode!\n");
			break;
		}

		/* transfer content to new index block */
		bh = sb_bread(sb, new_index);
		if (!bh) {
			pr_err("Could not write new index block.\n");
			ret = -EIO;
			break;
		}

		memcpy(bh->b_data, dblock, OUICHEFS_BLOCK_SIZE);

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);

		brelse(bh);

		if (inode->i_ino == 1 || is_inode_snapshot(sb, inode->i_ino) == 0) {
			ret = 0;
			break;
		}

		/* if we are here, we are still in ouichefs and the current
		 * inode is a snapshot inode which is not the root
		 */

		inode_inf->index_block = new_index;
		old_ino = inode->i_ino;
		inode->i_ino = get_free_inode(sbi);

		mark_inode_dirty(inode);

		/* make inode writeable. needs to happen after the
		 * previous check.
		 */
		unset_inode_snapshot_bit(sb, inode->i_ino);

		/* use writeable disk inode in ram inode */
		child_old_ino = old_ino;
		child_new_ino = inode->i_ino;

		/* iterate upwards in the file tree */
		inode_inf = inode_inf->parent;
		inode = &(inode_inf->vfs_inode);
	}

	kfree(dblock);

	return 0;
}

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *ouichefs_lookup(struct inode *dir, struct dentry *dentry,
				      unsigned int flags)
{
	struct super_block *sb = dir->i_sb;
	struct ouichefs_inode_info *ci_dir = OUICHEFS_INODE(dir);
	struct inode *inode = NULL;
	struct buffer_head *bh = NULL;
	struct ouichefs_dir_block *dblock = NULL;
	struct ouichefs_file *f = NULL;
	int i;

	/* Check filename length */
	if (dentry->d_name.len > OUICHEFS_FILENAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	/* Read the directory index block on disk */
	bh = sb_bread(sb, ci_dir->index_block);
	if (!bh)
		return ERR_PTR(-EIO);
	dblock = (struct ouichefs_dir_block *)bh->b_data;

	/* Search for the file in directory */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		f = &dblock->files[i];
		if (!f->inode)
			break;
		if (!strncmp(f->filename, dentry->d_name.name,
			     OUICHEFS_FILENAME_LEN)) {
			inode = ouichefs_iget(sb, f->inode);
			if (!(inode->i_state & I_NEW))
				ouichefs_inode_init_owner(&nop_mnt_idmap, inode, dir,
							  inode->i_mode);
			break;
		}
	}
	brelse(bh);

	/* Update directory access time */
	dir->i_atime = current_time(dir);
	mark_inode_dirty(dir);

	/* Fill the dentry with the inode */
	d_add(dentry, inode);

	return NULL;
}

/*
 * Create a new inode in dir.
 */
static struct inode *ouichefs_new_inode(struct inode *dir, mode_t mode)
{
	struct inode *inode;
	struct ouichefs_inode_info *ci;
	struct super_block *sb;
	struct ouichefs_sb_info *sbi;
	uint32_t ino, bno;
	int ret;

	/* Check mode before doing anything to avoid undoing everything */
	if (!S_ISDIR(mode) && !S_ISREG(mode)) {
		pr_err("File type not supported (only directory and regular files supported)\n");
		return ERR_PTR(-EINVAL);
	}

	/* Check if inodes are available */
	sb = dir->i_sb;
	sbi = OUICHEFS_SB(sb);
	if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
		return ERR_PTR(-ENOSPC);

	/* Get a new free inode */
	ino = get_free_inode(sbi);
	if (!ino)
		return ERR_PTR(-ENOSPC);
	inode = ouichefs_iget(sb, ino);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto put_ino;
	}
	ci = OUICHEFS_INODE(inode);

	/* Get a free block for this new inode's index */
	bno = get_free_block(sbi);
	if (!bno) {
		ret = -ENOSPC;
		goto put_inode;
	}
	ci->index_block = bno;

	/* Initialize inode */
	ouichefs_inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_blocks = 1;
	if (S_ISDIR(mode)) {
		inode->i_size = OUICHEFS_BLOCK_SIZE;
		inode->i_fop = &ouichefs_dir_ops;
		set_nlink(inode, 2); /* . and .. */
	} else if (S_ISREG(mode)) {
		inode->i_size = 0;
		inode->i_fop = &ouichefs_file_ops;
		inode->i_mapping->a_ops = &ouichefs_aops;
		set_nlink(inode, 1);
	}

	inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);

	return inode;

put_inode:
	iput(inode);
put_ino:
	put_inode(sbi, ino);

	return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
static int ouichefs_create(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, umode_t mode, bool excl)
{
	struct super_block *sb;
	struct inode *inode;
	struct ouichefs_inode_info *ci_dir;
	struct ouichefs_dir_block *dblock;
	char *fblock;
	struct buffer_head *bh, *bh2;
	int ret = 0, i;

	/* Check filename length */
	if (strlen(dentry->d_name.name) > OUICHEFS_FILENAME_LEN)
		return -ENAMETOOLONG;

	/* Read parent directory index */
	ci_dir = OUICHEFS_INODE(dir);
	sb = dir->i_sb;

	/* ensure directory inode is not a snapshot inode */
	ouichefs_inode_ensure_writeable(sb, dir);

	bh = sb_bread(sb, ci_dir->index_block);
	if (!bh)
		return -EIO;
	dblock = (struct ouichefs_dir_block *)bh->b_data;

	/* Check if parent directory is full */
	if (dblock->files[OUICHEFS_MAX_SUBFILES - 1].inode != 0) {
		ret = -EMLINK;
		goto end;
	}

	/* Get a new free inode */
	inode = ouichefs_new_inode(dir, mode);
	if (IS_ERR(inode)) {
		ret = PTR_ERR(inode);
		goto end;
	}

	/*
	 * Scrub index_block for new file/directory to avoid previous data
	 * messing with new file/directory.
	 */
	bh2 = sb_bread(sb, OUICHEFS_INODE(inode)->index_block);
	if (!bh2) {
		ret = -EIO;
		goto iput;
	}
	fblock = (char *)bh2->b_data;
	memset(fblock, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh2);
	brelse(bh2);

	/* Find first free slot in parent index and register new inode */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++)
		if (dblock->files[i].inode == 0)
			break;
	dblock->files[i].inode = inode->i_ino;
	strscpy(dblock->files[i].filename, dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update stats and mark dir and new inode dirty */
	mark_inode_dirty(inode);
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(mode))
		inode_inc_link_count(dir);
	mark_inode_dirty(dir);

	/* setup dentry */
	d_instantiate(dentry, inode);

	return 0;

iput:
	put_block(OUICHEFS_SB(sb), OUICHEFS_INODE(inode)->index_block);
	put_inode(OUICHEFS_SB(sb), inode->i_ino);
	iput(inode);
end:
	brelse(bh);
	return ret;
}

/*
 * Remove a link for a file. If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int ouichefs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct ouichefs_sb_info *sbi = OUICHEFS_SB(sb);
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL, *bh2 = NULL;
	struct ouichefs_dir_block *dir_block = NULL;
	struct ouichefs_file_index_block *file_block = NULL;
	uint32_t ino, bno;
	int i, f_id = -1, nr_subs = 0;

	ino = inode->i_ino;
	bno = OUICHEFS_INODE(inode)->index_block;

	/* ensure directory inode is not a snapshot inode */
	ouichefs_inode_ensure_writeable(sb, dir);

	/* Read parent directory index */
	bh = sb_bread(sb, OUICHEFS_INODE(dir)->index_block);
	if (!bh)
		return -EIO;
	dir_block = (struct ouichefs_dir_block *)bh->b_data;

	/* Search for inode in parent index and get number of subfiles */
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		if (dir_block->files[i].inode == ino)
			f_id = i;
		else if (dir_block->files[i].inode == 0)
			break;
	}
	nr_subs = i;

	/* Remove file from parent directory */
	if (f_id != OUICHEFS_MAX_SUBFILES - 1)
		memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
			(nr_subs - f_id - 1) * sizeof(struct ouichefs_file));
	memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct ouichefs_file));
	mark_buffer_dirty(bh);
	brelse(bh);

	/* Update inode stats */
	dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
	if (S_ISDIR(inode->i_mode))
		inode_dec_link_count(dir);
	mark_inode_dirty(dir);

	/* do not delete inodes that are part of a snapshot */
	if (is_inode_snapshot(sb, inode->i_ino)) {
		/* preserve snapshot state if it is dirty */
		write_inode_now(inode, 1);

		/*
		 * delete the vfs inode. make sure to change the inode number to
		 * avoid overwriting.
		 */
		clear_nlink(inode);
		inode->i_ino = 0;
		mark_inode_dirty(inode);

		return 0;
	}

	/*
	 * Cleanup pointed blocks if unlinking a file. If we fail to read the
	 * index block, cleanup inode anyway and lose this file's blocks
	 * forever. If we fail to scrub a data block, don't fail (too late
	 * anyway), just put the block and continue.
	 */
	bh = sb_bread(sb, bno);
	if (!bh)
		goto clean_inode;
	file_block = (struct ouichefs_file_index_block *)bh->b_data;
	if (S_ISDIR(inode->i_mode))
		goto scrub;
	for (i = 0; i < inode->i_blocks - 1; i++) {
		char *block;

		if (!file_block->blocks[i])
			continue;

		put_block(sbi, file_block->blocks[i]);
		bh2 = sb_bread(sb, file_block->blocks[i]);
		if (!bh2)
			continue;
		block = (char *)bh2->b_data;
		memset(block, 0, OUICHEFS_BLOCK_SIZE);
		mark_buffer_dirty(bh2);
		brelse(bh2);
	}

scrub:
	/* Scrub index block */
	memset(file_block, 0, OUICHEFS_BLOCK_SIZE);
	mark_buffer_dirty(bh);
	brelse(bh);

clean_inode:
	/* Cleanup inode and mark dirty */
	inode->i_blocks = 0;
	OUICHEFS_INODE(inode)->index_block = 0;
	inode->i_size = 0;
	i_uid_write(inode, 0);
	i_gid_write(inode, 0);
	inode->i_mode = 0;
	inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
		0;
	inode->i_ctime.tv_nsec = inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec =
		0;
	inode_dec_link_count(inode);
	mark_inode_dirty(inode);
	/* Overwrite the old inode with 0s */
	write_inode_now(inode, 1);

	/* Free inode and index block from bitmap */
	put_block(sbi, bno);
	put_inode(sbi, ino);

	return 0;
}

static int ouichefs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			   struct dentry *old_dentry, struct inode *new_dir,
			   struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct ouichefs_inode_info *ci_old = OUICHEFS_INODE(old_dir);
	struct ouichefs_inode_info *ci_new = OUICHEFS_INODE(new_dir);
	struct inode *src = d_inode(old_dentry);
	struct buffer_head *bh_old = NULL, *bh_new = NULL;
	struct ouichefs_dir_block *dir_block = NULL;
	int i, f_id = -1, new_pos = -1, ret, nr_subs, f_pos = -1;

	/* fail with these unsupported flags */
	if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
		return -EINVAL;

	/* Check if filename is not too long */
	if (strlen(new_dentry->d_name.name) > OUICHEFS_FILENAME_LEN)
		return -ENAMETOOLONG;

	ouichefs_inode_ensure_writeable(sb, old_dir);
	ouichefs_inode_ensure_writeable(sb, new_dir);

	/* Fail if new_dentry exists or if new_dir is full */
	bh_new = sb_bread(sb, ci_new->index_block);
	if (!bh_new)
		return -EIO;
	dir_block = (struct ouichefs_dir_block *)bh_new->b_data;
	for (i = 0; i < OUICHEFS_MAX_SUBFILES; i++) {
		/* if old_dir == new_dir, save the renamed file position */
		if (new_dir == old_dir) {
			if (strncmp(dir_block->files[i].filename,
				    old_dentry->d_name.name,
				    OUICHEFS_FILENAME_LEN) == 0)
				f_pos = i;
		}
		if (strncmp(dir_block->files[i].filename,
			    new_dentry->d_name.name,
			    OUICHEFS_FILENAME_LEN) == 0) {
			ret = -EEXIST;
			goto relse_new;
		}
		if (new_pos < 0 && dir_block->files[i].inode == 0)
			new_pos = i;
	}
	/* if old_dir == new_dir, just rename entry */
	if (old_dir == new_dir) {
		strscpy(dir_block->files[f_pos].filename,
			new_dentry->d_name.name, OUICHEFS_FILENAME_LEN);
		mark_buffer_dirty(bh_new);
		ret = 0;
		goto relse_new;
	}

	/* If new directory is empty, fail */
	if (new_pos < 0) {
		ret = -EMLINK;
		goto relse_new;
	}

	/* insert in new parent directory */
	dir_block->files[new_pos].inode = src->i_ino;
	strscpy(dir_block->files[new_pos].filename, new_dentry->d_name.name,
		OUICHEFS_FILENAME_LEN);
	mark_buffer_dirty(bh_new);
	brelse(bh_new);

	/* Update new parent inode metadata */
	new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
		current_time(new_dir);
	if (S_ISDIR(src->i_mode))
		inode_inc_link_count(new_dir);
	mark_inode_dirty(new_dir);

	/* remove target from old parent directory */
	bh_old = sb_bread(sb, ci_old->index_block);
	if (!bh_old)
		return -EIO;
	dir_block = (struct ouichefs_dir_block *)bh_old->b_data;
	/* Search for inode in old directory and number of subfiles */
	for (i = 0; OUICHEFS_MAX_SUBFILES; i++) {
		if (dir_block->files[i].inode == src->i_ino)
			f_id = i;
		else if (dir_block->files[i].inode == 0)
			break;
	}
	nr_subs = i;

	/* Remove file from old parent directory */
	if (f_id != OUICHEFS_MAX_SUBFILES - 1)
		memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
			(nr_subs - f_id - 1) * sizeof(struct ouichefs_file));
	memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct ouichefs_file));
	mark_buffer_dirty(bh_old);
	brelse(bh_old);

	/* Update old parent inode metadata */
	old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
		current_time(old_dir);
	if (S_ISDIR(src->i_mode))
		inode_dec_link_count(old_dir);
	mark_inode_dirty(old_dir);

	return 0;

relse_new:
	brelse(bh_new);
	return ret;
}

static int ouichefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			  struct dentry *dentry, umode_t mode)
{
	/* ensure directory inode is not a snapshot inode */
	ouichefs_inode_ensure_writeable(dir->i_sb, dir);

	return ouichefs_create(NULL, dir, dentry, mode | S_IFDIR, 0);
}

static int ouichefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh;
	struct ouichefs_dir_block *dblock;

	/* If the directory is not empty, fail */
	if (inode->i_nlink > 2)
		return -ENOTEMPTY;

	/* ensure directory inode is not a snapshot inode */
	ouichefs_inode_ensure_writeable(sb, dir);

	bh = sb_bread(sb, OUICHEFS_INODE(inode)->index_block);
	if (!bh)
		return -EIO;
	dblock = (struct ouichefs_dir_block *)bh->b_data;
	if (dblock->files[0].inode != 0) {
		brelse(bh);
		return -ENOTEMPTY;
	}
	brelse(bh);

	/* Remove directory with unlink */
	return ouichefs_unlink(dir, dentry);
}

static const struct inode_operations ouichefs_inode_ops = {
	.lookup = ouichefs_lookup,
	.create = ouichefs_create,
	.unlink = ouichefs_unlink,
	.mkdir = ouichefs_mkdir,
	.rmdir = ouichefs_rmdir,
	.rename = ouichefs_rename,
};
