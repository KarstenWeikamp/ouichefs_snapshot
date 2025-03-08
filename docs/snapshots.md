# Ouichefs Snapshots

## Snapshot bitmap

Each inode has a bit associated to it which is equal to 1 iff the inode
is contained within at least one snapshot. Any inode with the "snap" bit
set is read-only and never changed by the file system. Instead, if any
attribute of the inode or the file content are changed, all parents up
to the file system root whose snap bit is 1 are replaced by new inodes
with the snap bit set to zero.

Any time a snapshot is created, the snap bits of all inodes are set to
`1`. To reduce the overhead of this step, the snap bit is implemented as
a bitmap rather than a flag within the inode.

## Sysfs Interface

Snapshots for each Ouichefs partitions are managed via a sysfs interface.
Mounting an ouichefs partition creates a subfolder with the same name as
the partition in `/sys/fs/ouichefs`, mounting `sda`, for example, will create
`/sys/fs/ouichefs/sda/`.

This directory will contain the files `create`, `destroy`, `restore`, and `list`.

### Example sysfs layout

```
/sys/fs/ouichefs
|-- sda
|   |-- create
|   |-- destroy
|   |-- list
|   `-- restore
`-- sdb
    |-- create
    |-- destroy
    |-- list
    `-- restore
```

### Usage

#### create

The `create` file will assign a unique ID to the snapshot and save the current
state of the partition. To use it, simply write to the file associated with the partition.

```sh
$ echo 1 > /sys/fs/ouichefs/sda/create
```

#### destroy

Write a snapshot ID to the `destroy` file to permanently delete a snapshot from
the list.

```sh
$ echo 1 > /sys/fs/ouichefs/sda/destroy
```

#### list

The list `file` should list all the snapshots previously saved, printing the
unique ID and the creation date. The format is ID: `dd.mm.yy HH:MM:SS`.

```sh
$ cat /sys/fs/ouichefs/sda/list
```

```sh
$ cat /sys/fs/ouichefs/sda/list
1: 25.12.24 19:12:37
2: 29.12.24 21:18:01
3: 31.12.24 08:59:47
4: 02.01.25 23:28:31
5: 07.01.25 08:34:21
```

#### restore

Finally, write a snapshot ID to the `restore` file to restore a snapshot,
effectively rolling back the partition to the state it was at the time of the snapshot.

```sh
$ echo 4 > /sys/fs/ouichefs/sda/restore
```

### Implementation decisions

The sysfs interface for controlling the snapshot system of the Ouichefs
filesystem uses a kset as the base sysfs directory, which allows easy searching
of kobjects by string, particularly useful in the `superblock_kill` function, as
the most easily available information we have at mount and unmount of a
partition is the name string.

Each partition's snapshot control is represented by a
`ouichefs_sysfs_snapshot_control` structure, containing a kobject and
snapshot-related data structures.

Using kobjects and ksets ensures proper reference counting and cleanup,
preventing resource leaks. The `create_ouichefs_partition_snapshot_controls`
function initializes and adds kobjects to the kset, while
`delete_ouichefs_partition_snapshot_controls` handles their removal.
The `init_sysfs` and `deinit_sysfs` functions manage the kset's lifecycle.