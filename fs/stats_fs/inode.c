// SPDX-License-Identifier: GPL-2.0
/*
 *  inode.c - part of stats_fs, a tiny little stats_fs file system
 *
 *  Copyright (C) 2020 Emanuele Giuseppe Esposito <eesposit@redhat.com>
 *  Copyright (C) 2020 Redhat
 */
#define pr_fmt(fmt)	"stats_fs: " fmt

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/init.h>
#include <linux/stats_fs.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "internal.h"

#define STATS_FS_DEFAULT_MODE	0700

static struct simple_fs stats_fs;
static bool stats_fs_registered;

struct stats_fs_mount_opts {
	kuid_t uid;
	kgid_t gid;
	umode_t mode;
};

enum {
	Opt_uid,
	Opt_gid,
	Opt_mode,
	Opt_err
};

static const match_table_t tokens = {
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_mode, "mode=%o"},
	{Opt_err, NULL}
};

struct stats_fs_fs_info {
	struct stats_fs_mount_opts mount_opts;
};

static int stats_fs_parse_options(char *data, struct stats_fs_mount_opts *opts)
{
	substring_t args[MAX_OPT_ARGS];
	int option;
	int token;
	kuid_t uid;
	kgid_t gid;
	char *p;

	opts->mode = STATS_FS_DEFAULT_MODE;

	while ((p = strsep(&data, ",")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_uid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			uid = make_kuid(current_user_ns(), option);
			if (!uid_valid(uid))
				return -EINVAL;
			opts->uid = uid;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return -EINVAL;
			gid = make_kgid(current_user_ns(), option);
			if (!gid_valid(gid))
				return -EINVAL;
			opts->gid = gid;
			break;
		case Opt_mode:
			if (match_octal(&args[0], &option))
				return -EINVAL;
			opts->mode = option & S_IALLUGO;
			break;
		/*
		 * We might like to report bad mount options here;
		 * but traditionally stats_fs has ignored all mount options
		 */
		}
	}

	return 0;
}

static int stats_fs_apply_options(struct super_block *sb)
{
	struct stats_fs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode = d_inode(sb->s_root);
	struct stats_fs_mount_opts *opts = &fsi->mount_opts;

	inode->i_mode &= ~S_IALLUGO;
	inode->i_mode |= opts->mode;

	inode->i_uid = opts->uid;
	inode->i_gid = opts->gid;

	return 0;
}

static int stats_fs_remount(struct super_block *sb, int *flags, char *data)
{
	int err;
	struct stats_fs_fs_info *fsi = sb->s_fs_info;

	sync_filesystem(sb);
	err = stats_fs_parse_options(data, &fsi->mount_opts);
	if (err)
		goto fail;

	stats_fs_apply_options(sb);

fail:
	return err;
}

static int stats_fs_show_options(struct seq_file *m, struct dentry *root)
{
	struct stats_fs_fs_info *fsi = root->d_sb->s_fs_info;
	struct stats_fs_mount_opts *opts = &fsi->mount_opts;

	if (!uid_eq(opts->uid, GLOBAL_ROOT_UID))
		seq_printf(m, ",uid=%u",
			   from_kuid_munged(&init_user_ns, opts->uid));
	if (!gid_eq(opts->gid, GLOBAL_ROOT_GID))
		seq_printf(m, ",gid=%u",
			   from_kgid_munged(&init_user_ns, opts->gid));
	if (opts->mode != STATS_FS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", opts->mode);

	return 0;
}


static void stats_fs_free_inode(struct inode *inode)
{
	kfree(inode->i_private);
	free_inode_nonrcu(inode);
}

static const struct super_operations stats_fs_super_operations = {
	.statfs		= simple_statfs,
	.remount_fs	= stats_fs_remount,
	.show_options	= stats_fs_show_options,
	.free_inode	= stats_fs_free_inode,
};

static int stats_fs_fill_super(struct super_block *sb, void *data, int silent)
{
	static const struct tree_descr stats_fs_files[] = {{""}};
	struct stats_fs_fs_info *fsi;
	int err;

	fsi = kzalloc(sizeof(struct stats_fs_fs_info), GFP_KERNEL);
	sb->s_fs_info = fsi;
	if (!fsi) {
		err = -ENOMEM;
		goto fail;
	}

	err = stats_fs_parse_options(data, &fsi->mount_opts);
	if (err)
		goto fail;

	err  =  simple_fill_super(sb, STATSFS_MAGIC, stats_fs_files);
	if (err)
		goto fail;

	sb->s_op = &stats_fs_super_operations;

	stats_fs_apply_options(sb);

	return 0;

fail:
	kfree(fsi);
	sb->s_fs_info = NULL;
	return err;
}

static struct dentry *stats_fs_mount(struct file_system_type *fs_type,
			int flags, const char *dev_name,
			void *data)
{
	return mount_single(fs_type, flags, data, stats_fs_fill_super);
}

static struct file_system_type stats_fs_fs_type = {
	.owner =	THIS_MODULE,
	.name =		"statsfs",
	.mount =	stats_fs_mount,
	.kill_sb =	kill_litter_super,
};
MODULE_ALIAS_FS("statsfs");


/**
 * stats_fs_create_file - create a file in the stats_fs filesystem
 * @val: a pointer to a stats_fs_value containing all the infos of
 * the file to create (name, permission)
 * @src: a pointer to a stats_fs_source containing the dentry of where
 * to add this file
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the stats_fs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, ERR_PTR(-ERROR) will be
 * returned.
 *
 * Val and src will be also inglobated in a ststsfs_data_inode struct
 * that will be internally stored as inode->i_private and used in the
 * get/set attribute functions (see stats_fs_ops in stats_fs.c).
 */
struct dentry *stats_fs_create_file(struct stats_fs_value *val, struct stats_fs_source *src)
{
	struct dentry *dentry;
	struct inode *inode;
	struct stats_fs_data_inode *val_inode;

	val_inode = kzalloc(sizeof(struct stats_fs_data_inode), GFP_KERNEL);
	if (!val_inode) {
		printk(KERN_ERR
			"Kzalloc failure in stats_fs_create_files (ENOMEM)\n");
		return ERR_PTR(-ENOMEM);
	}

	val_inode->src = src;
	val_inode->val = val;


	dentry = simplefs_create_file(&stats_fs, &stats_fs_fs_type,
				      val->name, stats_fs_val_get_mode(val),
					  src->source_dentry, val_inode, &inode);
	if (IS_ERR(dentry))
		return dentry;

	inode->i_fop = &stats_fs_attr_ops;

	return simplefs_finish_dentry(dentry, inode);
}

struct dentry *stats_fs_create_schema( struct stats_fs_source *src) {
	struct dentry *dentry;
	struct inode *inode;
	struct stats_fs_schema *schema;

	schema = kzalloc(sizeof(struct stats_fs_schema), GFP_KERNEL);
	if (!schema) {
		printk(KERN_ERR
			"Kzalloc failure in stats_fs_create_schema (ENOMEM)\n");
		return ERR_PTR(-ENOMEM);
	}

	schema->str = "SCHEMA";
	dentry = simplefs_create_file(&stats_fs, &stats_fs_fs_type,
				      ".schema", 0644,
					  src->source_dentry, schema, &inode);
	if (IS_ERR(dentry))
		return dentry;

	inode->i_fop = &stats_fs_schema_ops;

	return simplefs_finish_dentry(dentry, inode);
}

/**
 * stats_fs_create_dir - create a directory in the stats_fs filesystem
 * @name: a pointer to a string containing the name of the directory to
 *        create.
 * @parent: a pointer to the parent dentry for this file.  This should be a
 *          directory dentry if set.  If this parameter is NULL, then the
 *          directory will be created in the root of the stats_fs filesystem.
 *
 * This function creates a directory in stats_fs with the given name.
 *
 * This function will return a pointer to a dentry if it succeeds.  This
 * pointer must be passed to the stats_fs_remove() function when the file is
 * to be removed (no automatic cleanup happens if your module is unloaded,
 * you are responsible here.)  If an error occurs, ERR_PTR(-ERROR) will be
 * returned.
 */
struct dentry *stats_fs_create_dir(const char *name, struct dentry *parent)
{
	struct dentry *dentry;
	struct inode *inode;

	dentry = simplefs_create_dir(&stats_fs, &stats_fs_fs_type,
				     name, 0755, parent, &inode);
	if (IS_ERR(dentry))
		return dentry;

	inode->i_op = &simple_dir_inode_operations;
	return simplefs_finish_dentry(dentry, inode);
}

static void remove_one(struct dentry *victim)
{
	simple_release_fs(&stats_fs);
}

/**
 * stats_fs_remove - recursively removes a directory
 * @dentry: a pointer to a the dentry of the directory to be removed.  If this
 *          parameter is NULL or an error value, nothing will be done.
 *
 * This function recursively removes a directory tree in stats_fs that
 * was previously created with a call to another stats_fs function
 * (like stats_fs_create_file() or variants thereof.)
 *
 * This function is required to be called in order for the file to be
 * removed, no automatic cleanup of files will happen when a module is
 * removed, you are responsible here.
 */
void stats_fs_remove(struct dentry *dentry)
{
	if (IS_ERR_OR_NULL(dentry))
		return;

	simple_pin_fs(&stats_fs, &stats_fs_fs_type);
	simple_recursive_removal(dentry, remove_one);
	simple_release_fs(&stats_fs);
}
/**
 * stats_fs_initialized - Tells whether stats_fs has been registered
 */
bool stats_fs_initialized(void)
{
	return stats_fs_registered;
}
EXPORT_SYMBOL_GPL(stats_fs_initialized);

static int __init stats_fs_init(void)
{
	int retval;

	retval = sysfs_create_mount_point(kernel_kobj, "statsfs");
	if (retval)
		return retval;

	retval = register_filesystem(&stats_fs_fs_type);
	if (retval)
		sysfs_remove_mount_point(kernel_kobj, "statsfs");
	else
		stats_fs_registered = true;

	return retval;
}
core_initcall(stats_fs_init);
