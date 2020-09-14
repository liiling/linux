/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _STATS_FS_INTERNAL_H_
#define _STATS_FS_INTERNAL_H_

#include <linux/list.h>
#include <linux/kref.h>
#include <linux/rwsem.h>
#include <linux/stats_fs.h>

/* values, grouped by base */
struct stats_fs_value_source {
	void *base_addr;
	bool files_created;
	struct stats_fs_value *values;
	struct list_head list_element;
};

struct stats_fs_data_inode {
	struct stats_fs_source *src;
	struct stats_fs_value *val;
};

extern const struct file_operations stats_fs_attr_ops;
extern const struct file_operations stats_fs_schema_ops;

struct dentry *stats_fs_create_file(struct stats_fs_value *val,
				   struct stats_fs_source *src);

struct dentry *stats_fs_create_schema(struct stats_fs_source *src);

struct dentry *stats_fs_create_dir(const char *name, struct dentry *parent);

void stats_fs_remove(struct dentry *dentry);
#define stats_fs_remove_recursive stats_fs_remove

int stats_fs_val_get_mode(struct stats_fs_value *val);

#endif /* _STATS_FS_INTERNAL_H_ */
