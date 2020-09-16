// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/stats_fs.h>

#include "internal.h"

struct stats_fs_aggregate_value {
	uint64_t sum, min, max;
	uint32_t count, count_zero;
};

static void stats_fs_source_remove_files(struct stats_fs_source *src);

static int is_val_signed(struct stats_fs_value *val)
{
	return val->type & STATS_FS_SIGN;
}

static int stats_fs_attr_get(void *data, u64 *val)
{
	int r = -EFAULT;
	struct stats_fs_data_inode *val_inode =
		(struct stats_fs_data_inode *)data;

	r = stats_fs_source_get_value(val_inode->src, val_inode->val, val);
	return r;
}

static int stats_fs_schema_get(void *data, u64 *val)
{
	struct stats_fs_schema *schema =
		(struct stats_fs_schema *)data;
	*val = schema->place_holder;
	return 0;

}

static int stats_fs_attr_clear(void *data, u64 val)
{
	int r = -EFAULT;
	struct stats_fs_data_inode *val_inode =
		(struct stats_fs_data_inode *)data;

	if (val)
		return -EINVAL;

	r = stats_fs_source_clear(val_inode->src, val_inode->val);
	return r;
}

int stats_fs_val_get_mode(struct stats_fs_value *val)
{
	return val->mode ? val->mode : 0644;
}

static int stats_fs_schema_data_open(struct inode *inode, struct file *file)
{
//	struct stats_fs_schema *schema;
//	schema = (struct stats_fs_schema *)inode->i_private;

	return simple_attr_open(inode, file, stats_fs_schema_get, NULL, "%lld\n");

}

static int stats_fs_attr_data_open(struct inode *inode, struct file *file)
{
	struct stats_fs_data_inode *val_inode;
	char *fmt;

	val_inode = (struct stats_fs_data_inode *)inode->i_private;

	/* Inodes hold a  pointer to the source which is not included in the
	 * refcount, so they files be opened while destroy is running, but
	 * values are removed (base_addr = NULL) before the source is destroyed.
	 */
	if (!kref_get_unless_zero(&val_inode->src->refcount))
		return -ENOENT;

	if (is_val_signed(val_inode->val))
		fmt = "%lld\n";
	else
		fmt = "%llu\n";

	if (simple_attr_open(inode, file, stats_fs_attr_get,
			     stats_fs_val_get_mode(val_inode->val) & 0222 ?
				     stats_fs_attr_clear :
				     NULL,
			     fmt)) {
		stats_fs_source_put(val_inode->src);
		return -ENOMEM;
	}
	return 0;
}

static int stats_fs_attr_release(struct inode *inode, struct file *file)
{
	struct stats_fs_data_inode *val_inode;

	val_inode = (struct stats_fs_data_inode *)inode->i_private;

	simple_attr_release(inode, file);
	stats_fs_source_put(val_inode->src);

	return 0;
}

const struct file_operations stats_fs_attr_ops = {
	.owner = THIS_MODULE,
	.open = stats_fs_attr_data_open,
	.release = stats_fs_attr_release,
	.read = simple_attr_read,
	.write = simple_attr_write,
	.llseek = no_llseek,
};

const struct file_operations stats_fs_schema_ops = {
	.owner = THIS_MODULE,
	.open = stats_fs_schema_data_open,
	.release = simple_attr_release,
	.read = simple_attr_read,
	.write = simple_attr_write,
	.llseek = no_llseek,
};

/* Called with rwsem held for writing */
static void stats_fs_source_remove_files_locked(struct stats_fs_source *src)
{
	struct stats_fs_source *child;

	if (src->source_dentry == NULL)
		return;

	list_for_each_entry (child, &src->subordinates_head, list_element)
		stats_fs_source_remove_files(child);

	stats_fs_remove_recursive(src->source_dentry);
	src->source_dentry = NULL;
}

static void stats_fs_source_remove_files(struct stats_fs_source *src)
{
	down_write(&src->rwsem);
	stats_fs_source_remove_files_locked(src);
	up_write(&src->rwsem);
}

static struct stats_fs_value *find_value(struct stats_fs_value_source *src,
					 struct stats_fs_value *val)
{
	struct stats_fs_value *entry;

	for (entry = src->values; entry->name; entry++) {
		if (entry == val)
			return entry;
	}
	return NULL;
}

static struct stats_fs_value *
search_value_in_source(struct stats_fs_source *src, struct stats_fs_value *arg,
		       struct stats_fs_value_source **val_src)
{
	struct stats_fs_value *entry;
	struct stats_fs_value_source *src_entry;

	list_for_each_entry (src_entry, &src->values_head, list_element) {
		entry = find_value(src_entry, arg);
		if (entry) {
			*val_src = src_entry;
			return entry;
		}
	}

	return NULL;
}

/* Called with rwsem held for writing */
static void stats_fs_create_files_locked(struct stats_fs_source *source)
{
	struct stats_fs_value_source *val_src;
	struct stats_fs_value *val;

	if (!source->source_dentry)
		return;

	list_for_each_entry (val_src, &source->values_head, list_element) {
		if (val_src->files_created)
			continue;

		for (val = val_src->values; val->name; val++)
			stats_fs_create_file(val, source);

		val_src->files_created = true;
	}
}

/* Called with rwsem held for writing */
static void
stats_fs_create_files_recursive_locked(struct stats_fs_source *source,
				       struct dentry *parent_dentry)
{
	struct stats_fs_source *child;

	/* first check values in this folder, since it might be new */
	if (!source->source_dentry) {
		source->source_dentry =
			stats_fs_create_dir(source->name, parent_dentry);

		source->schema_dentry = stats_fs_create_schema(source);
	}

	stats_fs_create_files_locked(source);

	list_for_each_entry (child, &source->subordinates_head, list_element) {
		if (child->source_dentry == NULL) {
			/* assume that if child has a folder,
			 * also the sub-child have that.
			 */
			down_write(&child->rwsem);
			stats_fs_create_files_recursive_locked(
				child, source->source_dentry);
			up_write(&child->rwsem);
		}
	}
}

void stats_fs_source_register(struct stats_fs_source *source)
{
	down_write(&source->rwsem);
	stats_fs_create_files_recursive_locked(source, NULL);
	up_write(&source->rwsem);
}
EXPORT_SYMBOL_GPL(stats_fs_source_register);

/* Called with rwsem held for writing */
static struct stats_fs_value_source *create_value_source(void *base)
{
	struct stats_fs_value_source *val_src;

	val_src = kzalloc(sizeof(struct stats_fs_value_source), GFP_KERNEL);
	if (!val_src)
		return ERR_PTR(-ENOMEM);

	val_src->base_addr = base;
	INIT_LIST_HEAD(&val_src->list_element);

	return val_src;
}

int stats_fs_source_add_values(struct stats_fs_source *source,
			       struct stats_fs_value *stat, void *ptr)
{
	struct stats_fs_value_source *val_src;
	struct stats_fs_value_source *entry;

	down_write(&source->rwsem);

	list_for_each_entry (entry, &source->values_head, list_element) {
		if (entry->base_addr == ptr && entry->values == stat) {
			up_write(&source->rwsem);
			return -EEXIST;
		}
	}

	val_src = create_value_source(ptr);
	val_src->values = (struct stats_fs_value *)stat;

	/* add the val_src to the source list */
	list_add(&val_src->list_element, &source->values_head);

	/* create child if it's the case */
	stats_fs_create_files_locked(source);

	up_write(&source->rwsem);

	return 0;
}
EXPORT_SYMBOL_GPL(stats_fs_source_add_values);

void stats_fs_source_add_subordinate(struct stats_fs_source *source,
				     struct stats_fs_source *sub)
{
	down_write(&source->rwsem);

	stats_fs_source_get(sub);
	list_add(&sub->list_element, &source->subordinates_head);
	if (source->source_dentry)
		stats_fs_create_files_recursive_locked(sub,
						       source->source_dentry);

	up_write(&source->rwsem);
}
EXPORT_SYMBOL_GPL(stats_fs_source_add_subordinate);

/* Called with rwsem held for writing */
static void
stats_fs_source_remove_subordinate_locked(struct stats_fs_source *source,
					  struct stats_fs_source *sub)
{
	struct stats_fs_source *src_entry;

	list_for_each_entry (src_entry, &source->subordinates_head,
			     list_element) {
		if (src_entry == sub) {
			list_del_init(&src_entry->list_element);
			stats_fs_source_remove_files(src_entry);
			stats_fs_source_put(src_entry);
			return;
		}
	}
}

void stats_fs_source_remove_subordinate(struct stats_fs_source *source,
					struct stats_fs_source *sub)
{
	down_write(&source->rwsem);
	stats_fs_source_remove_subordinate_locked(source, sub);
	up_write(&source->rwsem);
}
EXPORT_SYMBOL_GPL(stats_fs_source_remove_subordinate);

/* Called with rwsem held for reading */
static uint64_t get_simple_value(struct stats_fs_value_source *src,
				 struct stats_fs_value *val)
{
	uint64_t value_found;
	void *address;

	address = src->base_addr + val->offset;

	switch (val->type) {
	case STATS_FS_U8:
		value_found = *((uint8_t *)address);
		break;
	case STATS_FS_U8 | STATS_FS_SIGN:
		value_found = *((int8_t *)address);
		break;
	case STATS_FS_U16:
		value_found = *((uint16_t *)address);
		break;
	case STATS_FS_U16 | STATS_FS_SIGN:
		value_found = *((int16_t *)address);
		break;
	case STATS_FS_U32:
		value_found = *((uint32_t *)address);
		break;
	case STATS_FS_U32 | STATS_FS_SIGN:
		value_found = *((int32_t *)address);
		break;
	case STATS_FS_U64:
		value_found = *((uint64_t *)address);
		break;
	case STATS_FS_U64 | STATS_FS_SIGN:
		value_found = *((int64_t *)address);
		break;
	case STATS_FS_BOOL:
		value_found = *((uint8_t *)address);
		break;
	default:
		value_found = 0;
		break;
	}

	return value_found;
}

/* Called with rwsem held for reading */
static void clear_simple_value(struct stats_fs_value_source *src,
			       struct stats_fs_value *val)
{
	void *address;

	address = src->base_addr + val->offset;

	switch (val->type) {
	case STATS_FS_U8:
		*((uint8_t *)address) = 0;
		break;
	case STATS_FS_U8 | STATS_FS_SIGN:
		*((int8_t *)address) = 0;
		break;
	case STATS_FS_U16:
		*((uint16_t *)address) = 0;
		break;
	case STATS_FS_U16 | STATS_FS_SIGN:
		*((int16_t *)address) = 0;
		break;
	case STATS_FS_U32:
		*((uint32_t *)address) = 0;
		break;
	case STATS_FS_U32 | STATS_FS_SIGN:
		*((int32_t *)address) = 0;
		break;
	case STATS_FS_U64:
		*((uint64_t *)address) = 0;
		break;
	case STATS_FS_U64 | STATS_FS_SIGN:
		*((int64_t *)address) = 0;
		break;
	case STATS_FS_BOOL:
		*((uint8_t *)address) = 0;
		break;
	default:
		break;
	}
}

/* Called with rwsem held for reading */
static void
search_all_simple_values(struct stats_fs_source *src,
			 struct stats_fs_value_source *ref_src_entry,
			 struct stats_fs_value *val,
			 struct stats_fs_aggregate_value *agg)
{
	struct stats_fs_value_source *src_entry;
	uint64_t value_found;

	list_for_each_entry (src_entry, &src->values_head, list_element) {
		/* skip aggregates */
		if (src_entry->base_addr == NULL)
			continue;

		/* useless to search here */
		if (src_entry->values != ref_src_entry->values)
			continue;

		/* must be here */
		value_found = get_simple_value(src_entry, val);
		agg->sum += value_found;
		agg->count++;
		agg->count_zero += (value_found == 0);

		if (is_val_signed(val)) {
			agg->max = (((int64_t)value_found) >=
				    ((int64_t)agg->max)) ?
					   value_found :
					   agg->max;
			agg->min = (((int64_t)value_found) <=
				    ((int64_t)agg->min)) ?
					   value_found :
					   agg->min;
		} else {
			agg->max = (value_found >= agg->max) ? value_found :
							       agg->max;
			agg->min = (value_found <= agg->min) ? value_found :
							       agg->min;
		}
	}
}

/* Called with rwsem held for reading */
static void
do_recursive_aggregation(struct stats_fs_source *root,
			 struct stats_fs_value_source *ref_src_entry,
			 struct stats_fs_value *val,
			 struct stats_fs_aggregate_value *agg)
{
	struct stats_fs_source *subordinate;

	/* search all simple values in this folder */
	search_all_simple_values(root, ref_src_entry, val, agg);

	/* recursively search in all subfolders */
	list_for_each_entry (subordinate, &root->subordinates_head,
			     list_element) {
		down_read(&subordinate->rwsem);
		do_recursive_aggregation(subordinate, ref_src_entry, val, agg);
		up_read(&subordinate->rwsem);
	}
}

/* Called with rwsem held for reading */
static void init_aggregate_value(struct stats_fs_aggregate_value *agg,
				 struct stats_fs_value *val)
{
	agg->count = agg->count_zero = agg->sum = 0;
	if (is_val_signed(val)) {
		agg->max = S64_MIN;
		agg->min = S64_MAX;
	} else {
		agg->max = 0;
		agg->min = U64_MAX;
	}
}

/* Called with rwsem held for reading */
static void store_final_value(struct stats_fs_aggregate_value *agg,
			      struct stats_fs_value *val, uint64_t *ret)
{
	int operation;

	operation = val->aggr_kind | is_val_signed(val);

	switch (operation) {
	case STATS_FS_AVG:
		*ret = agg->count ? agg->sum / agg->count : 0;
		break;
	case STATS_FS_AVG | STATS_FS_SIGN:
		*ret = agg->count ? ((int64_t)agg->sum) / agg->count : 0;
		break;
	case STATS_FS_SUM:
	case STATS_FS_SUM | STATS_FS_SIGN:
		*ret = agg->sum;
		break;
	case STATS_FS_MIN:
	case STATS_FS_MIN | STATS_FS_SIGN:
		*ret = agg->min;
		break;
	case STATS_FS_MAX:
	case STATS_FS_MAX | STATS_FS_SIGN:
		*ret = agg->max;
		break;
	case STATS_FS_COUNT_ZERO:
	case STATS_FS_COUNT_ZERO | STATS_FS_SIGN:
		*ret = agg->count_zero;
		break;
	default:
		break;
	}
}

/* Called with rwsem held for reading */
static int stats_fs_source_get_value_locked(struct stats_fs_source *source,
					    struct stats_fs_value *arg,
					    uint64_t *ret)
{
	struct stats_fs_value_source *src_entry;
	struct stats_fs_value *found;
	struct stats_fs_aggregate_value aggr;

	*ret = 0;

	if (!arg)
		return -ENOENT;

	/* look in simple values */
	found = search_value_in_source(source, arg, &src_entry);

	if (!found) {
		printk(KERN_ERR "Stats_fs: Value in source \"%s\" not found!\n",
		       source->name);
		return -ENOENT;
	}

	if (src_entry->base_addr != NULL) {
		*ret = get_simple_value(src_entry, found);
		return 0;
	}

	/* look in aggregates */
	init_aggregate_value(&aggr, found);
	do_recursive_aggregation(source, src_entry, found, &aggr);
	store_final_value(&aggr, found, ret);

	return 0;
}

int stats_fs_source_get_value(struct stats_fs_source *source,
			      struct stats_fs_value *arg, uint64_t *ret)
{
	int retval;

	down_read(&source->rwsem);
	retval = stats_fs_source_get_value_locked(source, arg, ret);
	up_read(&source->rwsem);

	return retval;
}
EXPORT_SYMBOL_GPL(stats_fs_source_get_value);

/* Called with rwsem held for reading */
static void set_all_simple_values(struct stats_fs_source *src,
				  struct stats_fs_value_source *ref_src_entry,
				  struct stats_fs_value *val)
{
	struct stats_fs_value_source *src_entry;

	list_for_each_entry (src_entry, &src->values_head, list_element) {
		/* skip aggregates */
		if (src_entry->base_addr == NULL)
			continue;

		/* wrong to search here */
		if (src_entry->values != ref_src_entry->values)
			continue;

		if (src_entry->base_addr &&
		    src_entry->values == ref_src_entry->values)
			clear_simple_value(src_entry, val);
	}
}

/* Called with rwsem held for reading */
static void do_recursive_clean(struct stats_fs_source *root,
			       struct stats_fs_value_source *ref_src_entry,
			       struct stats_fs_value *val)
{
	struct stats_fs_source *subordinate;

	/* search all simple values in this folder */
	set_all_simple_values(root, ref_src_entry, val);

	/* recursively search in all subfolders */
	list_for_each_entry (subordinate, &root->subordinates_head,
			     list_element) {
		down_read(&subordinate->rwsem);
		do_recursive_clean(subordinate, ref_src_entry, val);
		up_read(&subordinate->rwsem);
	}
}

/* Called with rwsem held for reading */
static int stats_fs_source_clear_locked(struct stats_fs_source *source,
					struct stats_fs_value *val)
{
	struct stats_fs_value_source *src_entry;
	struct stats_fs_value *found;

	if (!val)
		return -ENOENT;

	/* look in simple values */
	found = search_value_in_source(source, val, &src_entry);

	if (!found) {
		printk(KERN_ERR "Stats_fs: Value in source \"%s\" not found!\n",
		       source->name);
		return -ENOENT;
	}

	if (src_entry->base_addr != NULL) {
		clear_simple_value(src_entry, found);
		return 0;
	}

	/* look in aggregates */
	do_recursive_clean(source, src_entry, found);

	return 0;
}

int stats_fs_source_clear(struct stats_fs_source *source,
			  struct stats_fs_value *val)
{
	int retval;

	down_read(&source->rwsem);
	retval = stats_fs_source_clear_locked(source, val);
	up_read(&source->rwsem);

	return retval;
}

/* Called with rwsem held for reading */
static struct stats_fs_value *
find_value_by_name(struct stats_fs_value_source *src, char *val)
{
	struct stats_fs_value *entry;

	for (entry = src->values; entry->name; entry++)
		if (!strcmp(entry->name, val))
			return entry;

	return NULL;
}

/* Called with rwsem held for reading */
static struct stats_fs_value *
search_in_source_by_name(struct stats_fs_source *src, char *name)
{
	struct stats_fs_value *entry;
	struct stats_fs_value_source *src_entry;

	list_for_each_entry (src_entry, &src->values_head, list_element) {
		entry = find_value_by_name(src_entry, name);
		if (entry)
			return entry;
	}

	return NULL;
}

int stats_fs_source_get_value_by_name(struct stats_fs_source *source,
				      char *name, uint64_t *ret)
{
	struct stats_fs_value *val;
	int retval;

	down_read(&source->rwsem);
	val = search_in_source_by_name(source, name);

	if (!val) {
		*ret = 0;
		up_read(&source->rwsem);
		return -ENOENT;
	}

	retval = stats_fs_source_get_value_locked(source, val, ret);
	up_read(&source->rwsem);

	return retval;
}
EXPORT_SYMBOL_GPL(stats_fs_source_get_value_by_name);

void stats_fs_source_get(struct stats_fs_source *source)
{
	kref_get(&source->refcount);
}
EXPORT_SYMBOL_GPL(stats_fs_source_get);

void stats_fs_source_revoke(struct stats_fs_source *source)
{
	struct stats_fs_value_source *val_src_entry;

	down_write(&source->rwsem);

	list_for_each_entry (val_src_entry, &source->values_head, list_element)
		val_src_entry->base_addr = NULL;

	up_write(&source->rwsem);
}
EXPORT_SYMBOL_GPL(stats_fs_source_revoke);

/* Called with rwsem held for writing
 *
 * The refcount is 0 and the lock was taken before refcount
 * went from 1 to 0
 */
static void stats_fs_source_destroy(struct kref *kref_source)
{
	struct stats_fs_value_source *val_src_entry;
	struct list_head *it, *safe;
	struct stats_fs_source *child, *source;

	source = container_of(kref_source, struct stats_fs_source, refcount);

	/* iterate through the values and delete them */
	list_for_each_safe (it, safe, &source->values_head) {
		val_src_entry = list_entry(it, struct stats_fs_value_source,
					   list_element);
		kfree(val_src_entry);
	}

	/* iterate through the subordinates and delete them */
	list_for_each_safe (it, safe, &source->subordinates_head) {
		child = list_entry(it, struct stats_fs_source, list_element);
		stats_fs_source_remove_subordinate_locked(source, child);
	}

	stats_fs_source_remove_files_locked(source);

	up_write(&source->rwsem);
	kfree(source->name);
	kfree(source);
}

void stats_fs_source_put(struct stats_fs_source *source)
{
	kref_put_rwsem(&source->refcount, stats_fs_source_destroy,
		       &source->rwsem);
}
EXPORT_SYMBOL_GPL(stats_fs_source_put);

struct stats_fs_source *stats_fs_source_create(const char *fmt, ...)
{
	va_list ap;
	char buf[100];
	struct stats_fs_source *ret;
	int char_needed;

	va_start(ap, fmt);
	char_needed = vsnprintf(buf, 100, fmt, ap);
	va_end(ap);

	ret = kzalloc(sizeof(struct stats_fs_source), GFP_KERNEL);
	if (!ret)
		return ERR_PTR(-ENOMEM);

	ret->name = kstrdup(buf, GFP_KERNEL);
	if (!ret->name) {
		kfree(ret);
		return ERR_PTR(-ENOMEM);
	}

	kref_init(&ret->refcount);
	init_rwsem(&ret->rwsem);

	INIT_LIST_HEAD(&ret->values_head);
	INIT_LIST_HEAD(&ret->subordinates_head);
	INIT_LIST_HEAD(&ret->list_element);

	return ret;
}
EXPORT_SYMBOL_GPL(stats_fs_source_create);
