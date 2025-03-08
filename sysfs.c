// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "%s:%s: " fmt, KBUILD_MODNAME, __func__
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/errno.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/fs.h>

#include "ouichefs.h"
#include "sysfs.h"
#include "snapshot.h"

/* Use kset as base sysfs directory as it allows easy searching of kobjects by string
 * which is the only helpful information provided by the superblock_kill function.
 */
static struct kset *ouichefs_kset;

struct ouichefs_sysfs_snapshot_control {
	struct kobject kobj;
	struct super_block *sb;
};

#define kobj_to_snapshot_ctrl(k) \
	container_of(k, struct ouichefs_sysfs_snapshot_control, kobj)

/* These are the functions which will call the actual functions which will operate
 * on the ouichefs snapshots.
 */
static ssize_t create_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	struct ouichefs_sysfs_snapshot_control *ossc = kobj_to_snapshot_ctrl(kobj);
	char comment_buf[OUICHEFS_SNAPSHOT_COMMENT_LEN + 1];
	size_t comment_len;
	int snap_id = 0;
	int ret;

	ret = sscanf(buf, "%i: %8s\n", &snap_id, comment_buf);

	if (ret < 1)
		ret = sscanf(buf, "%i\n", &snap_id);
	if (ret < 1) {
		ret = sscanf(buf, "%8s", comment_buf);
		snap_id = 0;
	}
	if (ret < 1) {
		memset(comment_buf, 0, sizeof(comment_buf));
		snap_id = 0;
	}

	if (count > OUICHEFS_SNAPSHOT_COMMENT_LEN) {
		pr_warn("Snapshot comment longer than supported length (%i)!\n",
		    OUICHEFS_SNAPSHOT_COMMENT_LEN);
		comment_len = OUICHEFS_SNAPSHOT_COMMENT_LEN;
	}

	pr_info("Partition %s: Creating snapshot with comment '%s'\n",
		ossc->sb->s_id, comment_buf);

	ret = ouichefs_snap_create(ossc->sb, snap_id, comment_buf);

	/* TODO: error handling? */

	return count;
}

static ssize_t destroy_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	int snap_id = 0;
	struct ouichefs_sysfs_snapshot_control *ossc = kobj_to_snapshot_ctrl(kobj);

	ret = kstrtos32(buf, 10, &snap_id);

	if (ret != 0) {
		pr_warn("Partition %s destroy: Could not parse snapshot ID from '%s'\n",
			 ossc->sb->s_id, buf);
		return count;
	}

	pr_info("Partition %s: Destroying snapshot %i\n",
		ossc->sb->s_id, snap_id);

	ret = ouichefs_snap_destroy(ossc->sb, snap_id);

	/* TODO: error handling? */

	return count;
}

static ssize_t list_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct ouichefs_snapshot *snap = NULL;
	struct ouichefs_sysfs_snapshot_control *ossc = kobj_to_snapshot_ctrl(kobj);
	size_t bytes_rem = PAGE_SIZE;

	pr_info("Partition %s: Listing all snapshots\n", ossc->sb->s_id);

	while ((snap = ouichefs_snap_next(ossc->sb, snap)) != NULL) {
		size_t ret;
		int snap_id = le32_to_cpu(snap->id);
		time64_t ts = le64_to_cpu(snap->timestamp);
		struct tm tm;

		time64_to_tm(ts, 0, &tm);

		ret = snprintf(buf, bytes_rem,
			       "%u: %02i.%02i.%02li %02i:%02i:%02i - '%.8s' -> %u\n",
			       snap_id,
			       tm.tm_mday, tm.tm_mon + 1, tm.tm_year - 100,
			       tm.tm_hour, tm.tm_min, tm.tm_sec,
			       snap->comment, snap->parent_id);

		if (ret >= bytes_rem)
			break;

		bytes_rem -= ret;
		buf += ret;
	}

	return PAGE_SIZE - bytes_rem;
}

static ssize_t restore_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	int ret;
	int snap_id = 0;
	struct ouichefs_sysfs_snapshot_control *ossc = kobj_to_snapshot_ctrl(kobj);

	ret = kstrtos32(buf, 10, &snap_id);

	if (ret != 0) {
		pr_warn("Partition %s restore: Could not parse snapshot ID from '%s'\n",
			 ossc->sb->s_id, buf);
		return count;
	}

	pr_info("Partition %s: Restoring snapshot %i\n",
		ossc->sb->s_id, snap_id);

	ret = ouichefs_snap_restore(ossc->sb, snap_id);

	/* TODO: error handling? */

	return count;
}

/* Sysfs files that handle control of snapshots on each partition */
static struct kobj_attribute snapshot_ctrl_create = __ATTR_WO(create);
static struct kobj_attribute snapshot_ctrl_destroy = __ATTR_WO(destroy);
static struct kobj_attribute snapshot_ctrl_list = __ATTR_RO(list);
static struct kobj_attribute snapshot_ctrl_restore = __ATTR_WO(restore);

static const struct attribute *const snapshot_ctrl_attrs[] = {
	&snapshot_ctrl_create.attr,
	&snapshot_ctrl_destroy.attr,
	&snapshot_ctrl_list.attr,
	&snapshot_ctrl_restore.attr,
	NULL,
};

void ouichefs_sysfs_snapshot_control_release(struct kobject *kobj)
{
	struct ouichefs_sysfs_snapshot_control *mine =
		(struct ouichefs_sysfs_snapshot_control *)container_of(
			kobj, struct ouichefs_sysfs_snapshot_control, kobj);

	/* Perform any additional cleanup for snapshots on this object, then... */
	kfree(mine);
}

/* ktype to support freeing underlying ouichefs_sysfs_snapshot_control structs automagically
 */
static const struct kobj_type ouichefs_sysfs_ktype = {
	.release = ouichefs_sysfs_snapshot_control_release,
	.sysfs_ops = &kobj_sysfs_ops
};

/** Create Sysfs snapshot controls on mount of ouichefs partition.*/
int create_ouichefs_partition_snapshot_controls(struct super_block *sb)
{
	struct ouichefs_sysfs_snapshot_control *subdir;

	/*Some string operation to infer the string that would be the partitions
	 * associated superblocks s_id.
	 */
	char partname[15];

	strscpy(partname, sb->s_id, 15);

	subdir = kzalloc(sizeof(struct ouichefs_sysfs_snapshot_control),
			 GFP_KERNEL);

	/* Add reference associated superblock of the partition*/
	subdir->sb = sb;

	/* Add kset to kobject so `kobject_init_and_add` adds kobject to kset.*/
	subdir->kobj.kset = ouichefs_kset;
	int retval = kobject_init_and_add(&subdir->kobj, &ouichefs_sysfs_ktype,
					  &ouichefs_kset->kobj, partname);

	/* Announce new Kobj to kset*/
	kobject_uevent(&subdir->kobj, KOBJ_ADD);

	retval = sysfs_create_files(&subdir->kobj, snapshot_ctrl_attrs);

	if (retval) {
		pr_err("Failed to create snapshot controls for %s\n", partname);
		kobject_put(&subdir->kobj);
		return retval;
	}
	return 0;
}

int delete_ouichefs_partition_snapshot_controls(const char *dev_name)
{
	struct kobject *dir = kset_find_obj(ouichefs_kset, dev_name);

	if (!dir) {
		pr_err("Unable to find %s kobj!\n", dev_name);
		return -EINVAL;
	}
	sysfs_remove_files(dir, snapshot_ctrl_attrs);
	/* Announce removal of kobject to kset kobject_put would do that automatically
	 * but it doesn't call kobject_put so our _release function is never called.
	 * So do the kobject_uevent with the appropriate number of _puts instead.
	 */
	kobject_uevent(dir, KOBJ_REMOVE);
	kobject_put(dir); //kobject_init_and_add
	kobject_put(dir); //kset_find_obj

	return 0;
}

int init_sysfs(void)
{
	ouichefs_kset = kset_create_and_add("ouichefs", NULL, fs_kobj);

	if (!ouichefs_kset) {
		pr_err("Failed to create root kobj!");
		return -ENOMEM;
	}
	return 0;
}

void deinit_sysfs(void)
{
	/* This should actually not do much as all kobjects are unloaded on unmount
	 * of partition. All ouichefs partition have to be unloaded before we can
	 * rmmod ouichefs. Still a kset needs to be unregistered.
	 */
	kset_unregister(ouichefs_kset);
}
