// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/anon_inodes.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/rwsem.h>
#include <linux/list.h>
#include <linux/kref.h>

#include <linux/limits.h>
#include <linux/stats_fs.h>
#include <kunit/test.h>
#include "internal.h"

#define STATS_FS_STAT(el, x, ...)                                              \
	{                                                                      \
		.name = #x, .offset = offsetof(struct container, el.x),        \
		##__VA_ARGS__                                                  \
	}

#define ARR_SIZE(el) ((int)(sizeof(el) / sizeof(struct stats_fs_value) - 1))

struct test_values_struct {
	uint64_t u64;
	int32_t s32;
	bool bo;
	uint8_t u8;
	int16_t s16;
};

struct container {
	struct test_values_struct vals;
};

struct stats_fs_value test_values[6] = {
	STATS_FS_STAT(vals, u64, .type = STATS_FS_U64,
		      .aggr_kind = STATS_FS_NONE, .mode = 0),
	STATS_FS_STAT(vals, s32, .type = STATS_FS_S32,
		      .aggr_kind = STATS_FS_NONE, .mode = 0),
	STATS_FS_STAT(vals, bo, .type = STATS_FS_BOOL,
		      .aggr_kind = STATS_FS_NONE, .mode = 0),
	STATS_FS_STAT(vals, u8, .type = STATS_FS_U8, .aggr_kind = STATS_FS_NONE,
		      .mode = 0),
	STATS_FS_STAT(vals, s16, .type = STATS_FS_S16,
		      .aggr_kind = STATS_FS_NONE, .mode = 0),
	{ NULL },
};

struct stats_fs_value test_aggr[4] = {
	STATS_FS_STAT(vals, s32, .type = STATS_FS_S32,
		      .aggr_kind = STATS_FS_MIN, .mode = 0),
	STATS_FS_STAT(vals, bo, .type = STATS_FS_BOOL,
		      .aggr_kind = STATS_FS_MAX, .mode = 0),
	STATS_FS_STAT(vals, u64, .type = STATS_FS_U64,
		      .aggr_kind = STATS_FS_SUM, .mode = 0),
	{ NULL },
};

struct stats_fs_value test_same_name[3] = {
	STATS_FS_STAT(vals, s32, .type = STATS_FS_S32,
		      .aggr_kind = STATS_FS_NONE, .mode = 0),
	STATS_FS_STAT(vals, s32, .type = STATS_FS_S32,
		      .aggr_kind = STATS_FS_MIN, .mode = 0),
	{ NULL },
};

struct stats_fs_value test_all_aggr[6] = {
	STATS_FS_STAT(vals, s32, .type = STATS_FS_S32,
		      .aggr_kind = STATS_FS_MIN, .mode = 0),
	STATS_FS_STAT(vals, bo, .type = STATS_FS_BOOL,
		      .aggr_kind = STATS_FS_COUNT_ZERO, .mode = 0),
	STATS_FS_STAT(vals, u64, .type = STATS_FS_U64,
		      .aggr_kind = STATS_FS_SUM, .mode = 0),
	STATS_FS_STAT(vals, u8, .type = STATS_FS_U8, .aggr_kind = STATS_FS_AVG,
		      .mode = 0),
	STATS_FS_STAT(vals, s16, .type = STATS_FS_S16,
		      .aggr_kind = STATS_FS_MAX, .mode = 0),
	{ NULL },
};

#define def_u64 ((uint64_t)64)

#define def_val_s32 ((int32_t)S32_MIN)
#define def_val_bool ((bool)true)
#define def_val_u8 ((uint8_t)127)
#define def_val_s16 ((int16_t)10000)

#define def_val2_s32 ((int32_t)S16_MAX)
#define def_val2_bool ((bool)false)
#define def_val2_u8 ((uint8_t)255)
#define def_val2_s16 ((int16_t)-20000)

struct container cont = {
	.vals = {
			.u64 = def_u64,
			.s32 = def_val_s32,
			.bo = def_val_bool,
			.u8 = def_val_u8,
			.s16 = def_val_s16,
		},
};

struct container cont2 = {
	.vals = {
			.u64 = def_u64,
			.s32 = def_val2_s32,
			.bo = def_val2_bool,
			.u8 = def_val2_u8,
			.s16 = def_val2_s16,
		},
};

static void get_stats_at_addr(struct stats_fs_source *src, void *addr,
			      int *aggr, int *val, int use_addr)
{
	struct stats_fs_value *entry;
	struct stats_fs_value_source *src_entry;
	int counter_val = 0, counter_aggr = 0;

	list_for_each_entry (src_entry, &src->values_head, list_element) {
		if (use_addr && src_entry->base_addr != addr)
			continue;

		for (entry = src_entry->values; entry->name; entry++) {
			if (entry->aggr_kind == STATS_FS_NONE)
				counter_val++;
			else
				counter_aggr++;
		}
	}

	if (aggr)
		*aggr = counter_aggr;

	if (val)
		*val = counter_val;
}

int source_has_subsource(struct stats_fs_source *src,
			 struct stats_fs_source *sub)
{
	struct stats_fs_source *entry;

	list_for_each_entry (entry, &src->subordinates_head, list_element) {
		if (entry == sub)
			return 1;
	}
	return 0;
}

int get_number_subsources(struct stats_fs_source *src)
{
	struct stats_fs_source *entry;
	int counter = 0;

	list_for_each_entry (entry, &src->subordinates_head, list_element) {
		counter++;
	}
	return counter;
}

int get_number_values(struct stats_fs_source *src)
{
	int counter = 0;

	get_stats_at_addr(src, NULL, NULL, &counter, 0);
	return counter;
}

int get_total_number_values(struct stats_fs_source *src)
{
	struct stats_fs_source *sub_entry;
	int counter = 0;

	get_stats_at_addr(src, NULL, NULL, &counter, 0);

	list_for_each_entry (sub_entry, &src->subordinates_head, list_element) {
		counter += get_total_number_values(sub_entry);
	}

	return counter;
}

int get_number_aggregates(struct stats_fs_source *src)
{
	int counter = 0;

	get_stats_at_addr(src, NULL, &counter, NULL, 1);
	return counter;
}

int get_number_values_with_base(struct stats_fs_source *src, void *addr)
{
	int counter = 0;

	get_stats_at_addr(src, addr, NULL, &counter, 1);
	return counter;
}

int get_number_aggr_with_base(struct stats_fs_source *src, void *addr)
{
	int counter = 0;

	get_stats_at_addr(src, addr, &counter, NULL, 1);
	return counter;
}

int get_number_labels(struct stats_fs_source *src)
{
	struct stats_fs_schema_label *label;
	int counter = 0;

	list_for_each_entry(label, &src->labels_head, label_element) {
		counter ++;
	}
	return counter;
}

static void test_empty_folder(struct kunit *test)
{
	struct stats_fs_source *src;

	src = stats_fs_source_create("kvm_%d", "subsystem_%s", 123, "name");
	KUNIT_EXPECT_EQ(test, strcmp(src->name, "kvm_123"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(src->label_key, "subsystem_name"), 0);
	KUNIT_EXPECT_EQ(test, get_number_subsources(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	stats_fs_source_put(src);
}

static void test_add_subfolder(struct kunit *test)
{
	struct stats_fs_source *src, *sub;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");
	stats_fs_source_add_subordinate(src, sub);
	KUNIT_EXPECT_EQ(test, source_has_subsource(src, sub), true);
	KUNIT_EXPECT_EQ(test, get_number_subsources(src), 1);
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_values(sub), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(sub), 0);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	stats_fs_source_put(sub);
	sub = stats_fs_source_create("not a child", "not_child_dir");
	KUNIT_EXPECT_EQ(test, source_has_subsource(src, sub), false);
	KUNIT_EXPECT_EQ(test, get_number_subsources(src), 1);

	stats_fs_source_put(sub);
	stats_fs_source_put(src);
}

static void test_labels(struct kunit *test)
{
	struct stats_fs_source *src, *sub, *subsub;
	struct stats_fs_schema_label *label;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");
	subsub = stats_fs_source_create("grandchild", "grandchild_dir");
	stats_fs_source_add_subordinate(src, sub);
	stats_fs_source_add_subordinate(sub, subsub);

	/* labels of src */
	KUNIT_EXPECT_EQ(test, get_number_labels(src), 1);
	label = list_entry(&src->labels_head, struct stats_fs_schema_label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "parent_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "parent"), 0);

	/* labels of sub */
	KUNIT_EXPECT_EQ(test, get_number_labels(sub), 2);
	label = list_entry(&sub->labels_head, struct stats_fs_schema_label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "child_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "child"), 0);
	label = list_next_entry(label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "parent_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "parent"), 0);

	/* labels of subsub */
	KUNIT_EXPECT_EQ(test, get_number_labels(subsub), 3);
	label = list_entry(&subsub->labels_head, struct stats_fs_schema_label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "grandchild_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "grandchild"), 0);
	label = list_next_entry(label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "child_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "child"), 0);
	label = list_next_entry(label, label_element);
	KUNIT_EXPECT_EQ(test, strcmp(label->key, "parent_dir"), 0);
	KUNIT_EXPECT_EQ(test, strcmp(label->value, "parent"), 0);

	stats_fs_source_put(subsub);
	stats_fs_source_put(sub);
	stats_fs_source_put(src);
}

static void test_add_value(struct kunit *test)
{
	struct stats_fs_source *src;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");

	// add values
	n = stats_fs_source_add_values(src, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));

	// add same values, nothing happens
	n = stats_fs_source_add_values(src, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, -EEXIST);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));

	// size is invaried
	KUNIT_EXPECT_EQ(test, get_number_values(src), ARR_SIZE(test_values));

	// no aggregates
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, get_number_values(src), ARR_SIZE(test_values));
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);

	stats_fs_source_put(src);
}

static void test_add_value_in_subfolder(struct kunit *test)
{
	struct stats_fs_source *src, *sub, *sub_not;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");

	// src -> sub
	stats_fs_source_add_subordinate(src, sub);

	// add values
	n = stats_fs_source_add_values(sub, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src),
			ARR_SIZE(test_values));

	KUNIT_EXPECT_EQ(test, get_number_values(sub), ARR_SIZE(test_values));
	// no values in sub
	KUNIT_EXPECT_EQ(test, get_number_aggregates(sub), 0);

	// different folder
	sub_not = stats_fs_source_create("not a child", "not_child_dir");

	// add values
	n = stats_fs_source_add_values(sub_not, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub_not, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src),
			ARR_SIZE(test_values));

	// remove sub, check values is 0
	stats_fs_source_remove_subordinate(src, sub);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	// re-add sub, check value are added
	stats_fs_source_add_subordinate(src, sub);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src),
			ARR_SIZE(test_values));

	// add sub_not, check value are twice as many
	stats_fs_source_add_subordinate(src, sub_not);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src),
			ARR_SIZE(test_values) * 2);

	KUNIT_EXPECT_EQ(test, get_number_values(sub_not),
			ARR_SIZE(test_values));
	KUNIT_EXPECT_EQ(test, get_number_aggregates(sub_not), 0);

	stats_fs_source_put(sub);
	stats_fs_source_put(sub_not);
	stats_fs_source_put(src);
}

static void test_search_value(struct kunit *test)
{
	struct stats_fs_source *src;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");

	// add values
	n = stats_fs_source_add_values(src, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));

	// get u64
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, def_u64);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ((bool)ret), def_val_bool);
	KUNIT_EXPECT_EQ(test, n, 0);

	// get a non-added value
	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	stats_fs_source_put(src);
}

static void test_search_value_in_subfolder(struct kunit *test)
{
	struct stats_fs_source *src, *sub;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");

	// src -> sub
	stats_fs_source_add_subordinate(src, sub);

	// add values to sub
	n = stats_fs_source_add_values(sub, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));

	n = stats_fs_source_get_value_by_name(sub, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, def_u64);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ((bool)ret), def_val_bool);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);
	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	stats_fs_source_put(sub);
	stats_fs_source_put(src);
}

static void test_search_value_in_empty_folder(struct kunit *test)
{
	struct stats_fs_source *src;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("empty folder", "parent_dir");
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_subsources(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);

	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	stats_fs_source_put(src);
}

static void test_add_aggregate(struct kunit *test)
{
	struct stats_fs_source *src;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");

	// add aggr to src, no values
	n = stats_fs_source_add_values(src, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);

	// count values
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));

	// add same array again, should not be added
	n = stats_fs_source_add_values(src, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, -EEXIST);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));

	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), ARR_SIZE(test_aggr));

	stats_fs_source_put(src);
}

static void test_add_aggregate_in_subfolder(struct kunit *test)
{
	struct stats_fs_source *src, *sub, *sub_not;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");
	// src->sub
	stats_fs_source_add_subordinate(src, sub);

	// add aggr to sub
	n = stats_fs_source_add_values(sub, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	KUNIT_EXPECT_EQ(test, get_number_values(sub), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(sub), ARR_SIZE(test_aggr));

	// not a child
	sub_not = stats_fs_source_create("not a child", "not_child_dir");

	// add aggr to "not a child"
	n = stats_fs_source_add_values(sub_not, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub_not, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));
	KUNIT_EXPECT_EQ(test, get_number_values(src), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), 0);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	// remove sub
	stats_fs_source_remove_subordinate(src, sub);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	// re-add both
	stats_fs_source_add_subordinate(src, sub);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);
	stats_fs_source_add_subordinate(src, sub_not);
	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	KUNIT_EXPECT_EQ(test, get_number_values(sub_not), 0);
	KUNIT_EXPECT_EQ(test, get_number_aggregates(sub_not),
			ARR_SIZE(test_aggr));

	stats_fs_source_put(sub);
	stats_fs_source_put(sub_not);
	stats_fs_source_put(src);
}

static void test_search_aggregate(struct kunit *test)
{
	struct stats_fs_source *src;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	n = stats_fs_source_add_values(src, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, S64_MAX);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);
	stats_fs_source_put(src);
}

static void test_search_aggregate_in_subfolder(struct kunit *test)
{
	struct stats_fs_source *src, *sub;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");

	stats_fs_source_add_subordinate(src, sub);

	n = stats_fs_source_add_values(sub, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));
	n = get_number_aggr_with_base(sub, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);

	// no u64 in test_aggr
	n = stats_fs_source_get_value_by_name(sub, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "s32", &ret);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, S64_MAX);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	n = stats_fs_source_get_value_by_name(sub, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);
	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	stats_fs_source_put(sub);
	stats_fs_source_put(src);
}

void test_search_same(struct kunit *test)
{
	struct stats_fs_source *src;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	n = stats_fs_source_add_values(src, test_same_name, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 1);
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 1);

	n = stats_fs_source_add_values(src, test_same_name, &cont);
	KUNIT_EXPECT_EQ(test, n, -EEXIST);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 1);
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, 1);

	// returns first the value
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	KUNIT_EXPECT_EQ(test, n, 0);

	stats_fs_source_put(src);
}

static void test_add_mixed(struct kunit *test)
{
	struct stats_fs_source *src;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");

	n = stats_fs_source_add_values(src, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_add_values(src, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));

	n = stats_fs_source_add_values(src, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, -EEXIST);
	n = get_number_values_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));
	n = stats_fs_source_add_values(src, test_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, -EEXIST);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));

	KUNIT_EXPECT_EQ(test, get_number_values(src), ARR_SIZE(test_values));
	KUNIT_EXPECT_EQ(test, get_number_aggregates(src), ARR_SIZE(test_aggr));
	stats_fs_source_put(src);
}

static void test_search_mixed(struct kunit *test)
{
	struct stats_fs_source *src, *sub;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub = stats_fs_source_create("child", "child_dir");
	stats_fs_source_add_subordinate(src, sub);

	// src has the aggregates, sub the values. Just search
	n = stats_fs_source_add_values(sub, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));
	n = stats_fs_source_add_values(src, test_aggr, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_aggr));

	// u64 is sum so again same value
	n = stats_fs_source_get_value_by_name(sub, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, def_u64);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, ret, def_u64);
	KUNIT_EXPECT_EQ(test, n, 0);

	// s32 is min so return the value also in the aggregate
	n = stats_fs_source_get_value_by_name(sub, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	KUNIT_EXPECT_EQ(test, n, 0);

	// bo is max
	n = stats_fs_source_get_value_by_name(sub, "bo", &ret);
	KUNIT_EXPECT_EQ(test, (bool)ret, def_val_bool);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, (bool)ret, def_val_bool);
	KUNIT_EXPECT_EQ(test, n, 0);

	n = stats_fs_source_get_value_by_name(sub, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);
	n = stats_fs_source_get_value_by_name(src, "does not exist", &ret);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	KUNIT_EXPECT_EQ(test, n, -ENOENT);

	stats_fs_source_put(sub);
	stats_fs_source_put(src);
}

static void test_all_aggregations_agg_val_val(struct kunit *test)
{
	struct stats_fs_source *src, *sub1, *sub2;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub1 = stats_fs_source_create("child1", "child_dir");
	sub2 = stats_fs_source_create("child2", "child_dir");
	stats_fs_source_add_subordinate(src, sub1);
	stats_fs_source_add_subordinate(src, sub2);

	n = stats_fs_source_add_values(sub1, test_all_aggr, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub1, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));
	n = stats_fs_source_add_values(sub2, test_all_aggr, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub2, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_add_values(src, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	// sum
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64 * 2);

	// min
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);

	// count_0
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 1ull);

	// avg
	n = stats_fs_source_get_value_by_name(src, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 191ull);

	// max
	n = stats_fs_source_get_value_by_name(src, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val_s16);

	stats_fs_source_put(sub1);
	stats_fs_source_put(sub2);
	stats_fs_source_put(src);
}

static void test_all_aggregations_val_agg_val(struct kunit *test)
{
	struct stats_fs_source *src, *sub1, *sub2;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub1 = stats_fs_source_create("child1", "child_dir");
	sub2 = stats_fs_source_create("child2", "child_dir");
	stats_fs_source_add_subordinate(src, sub1);
	stats_fs_source_add_subordinate(src, sub2);

	n = stats_fs_source_add_values(src, test_all_aggr, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));
	n = stats_fs_source_add_values(sub2, test_all_aggr, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub2, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_add_values(sub1, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub1, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64);
	n = stats_fs_source_get_value_by_name(sub1, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	n = stats_fs_source_get_value_by_name(sub2, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64);

	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);
	n = stats_fs_source_get_value_by_name(sub1, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, S64_MAX); // MIN
	n = stats_fs_source_get_value_by_name(sub2, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val2_s32);

	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (bool)ret, def_val_bool);
	n = stats_fs_source_get_value_by_name(sub1, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	n = stats_fs_source_get_value_by_name(sub2, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (bool)ret, def_val2_bool);

	n = stats_fs_source_get_value_by_name(src, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (uint8_t)ret, def_val_u8);
	n = stats_fs_source_get_value_by_name(sub1, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 0ull);
	n = stats_fs_source_get_value_by_name(sub2, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (uint8_t)ret, def_val2_u8);

	n = stats_fs_source_get_value_by_name(src, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val_s16);
	n = stats_fs_source_get_value_by_name(sub1, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int64_t)ret, S64_MIN); // MAX
	n = stats_fs_source_get_value_by_name(sub2, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val2_s16);

	stats_fs_source_put(sub1);
	stats_fs_source_put(sub2);
	stats_fs_source_put(src);
}

static void test_all_aggregations_agg_val_val_sub(struct kunit *test)
{
	struct stats_fs_source *src, *sub1, *sub11;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub1 = stats_fs_source_create("child1", "child_dir");
	sub11 = stats_fs_source_create("child11", "child_dir");
	stats_fs_source_add_subordinate(src, sub1);
	stats_fs_source_add_subordinate(sub1, sub11); // changes here!

	n = stats_fs_source_add_values(sub1, test_values, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub1, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));
	n = stats_fs_source_add_values(sub11, test_values, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_values_with_base(sub11, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_values));

	KUNIT_EXPECT_EQ(test, get_total_number_values(src),
			ARR_SIZE(test_values) * 2);

	n = stats_fs_source_add_values(sub1, test_all_aggr, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub1, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));
	n = stats_fs_source_add_values(sub11, test_all_aggr, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub11, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_add_values(src, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	// sum
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64 * 2);

	// min
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);

	// count_0
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 1ull);

	// avg
	n = stats_fs_source_get_value_by_name(src, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 191ull);

	// max
	n = stats_fs_source_get_value_by_name(src, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val_s16);

	stats_fs_source_put(sub1);
	stats_fs_source_put(sub11);
	stats_fs_source_put(src);
}

static void test_all_aggregations_agg_no_val_sub(struct kunit *test)
{
	struct stats_fs_source *src, *sub1, *sub11;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub1 = stats_fs_source_create("child1", "child_dir");
	sub11 = stats_fs_source_create("child11", "child_dir");
	stats_fs_source_add_subordinate(src, sub1);
	stats_fs_source_add_subordinate(sub1, sub11);

	n = stats_fs_source_add_values(sub11, test_all_aggr, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub11, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	n = stats_fs_source_add_values(src, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	// sum
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64);

	// min
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val2_s32);

	// count_0
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 1ull);

	// avg
	n = stats_fs_source_get_value_by_name(src, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (uint8_t)ret, def_val2_u8);

	// max
	n = stats_fs_source_get_value_by_name(src, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val2_s16);

	stats_fs_source_put(sub1);
	stats_fs_source_put(sub11);
	stats_fs_source_put(src);
}

static void test_all_aggregations_agg_agg_val_sub(struct kunit *test)
{
	struct stats_fs_source *src, *sub1, *sub11, *sub12;
	uint64_t ret;
	int n;

	src = stats_fs_source_create("parent", "parent_dir");
	sub1 = stats_fs_source_create("child1", "child_dir");
	sub11 = stats_fs_source_create("child11", "grandchild_dir");
	sub12 = stats_fs_source_create("child12", "grandchild_dir");
	stats_fs_source_add_subordinate(src, sub1);
	stats_fs_source_add_subordinate(sub1, sub11);
	stats_fs_source_add_subordinate(sub1, sub12);

	n = stats_fs_source_add_values(sub11, test_all_aggr, &cont2);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub11, &cont2);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_add_values(sub12, test_all_aggr, &cont);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub12, &cont);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	KUNIT_EXPECT_EQ(test, get_total_number_values(src), 0);

	n = stats_fs_source_add_values(src, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(src, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	n = stats_fs_source_add_values(sub1, test_all_aggr, NULL);
	KUNIT_EXPECT_EQ(test, n, 0);
	n = get_number_aggr_with_base(sub1, NULL);
	KUNIT_EXPECT_EQ(test, n, ARR_SIZE(test_all_aggr));

	// sum
	n = stats_fs_source_get_value_by_name(src, "u64", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, def_u64 * 2);

	// min
	n = stats_fs_source_get_value_by_name(src, "s32", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ((int32_t)ret), def_val_s32);

	// count_0
	n = stats_fs_source_get_value_by_name(src, "bo", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, ret, 1ull);

	// avg
	n = stats_fs_source_get_value_by_name(src, "u8", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (uint8_t)ret,
			(uint8_t)((def_val2_u8 + def_val_u8) / 2));

	// max
	n = stats_fs_source_get_value_by_name(src, "s16", &ret);
	KUNIT_EXPECT_EQ(test, n, 0);
	KUNIT_EXPECT_EQ(test, (int16_t)ret, def_val_s16);

	stats_fs_source_put(sub1);
	stats_fs_source_put(sub11);
	stats_fs_source_put(sub12);
	stats_fs_source_put(src);
}

static struct kunit_case stats_fs_test_cases[] = {
	KUNIT_CASE(test_empty_folder),
	KUNIT_CASE(test_add_subfolder),
	KUNIT_CASE(test_labels),
	KUNIT_CASE(test_add_value),
	KUNIT_CASE(test_add_value_in_subfolder),
	KUNIT_CASE(test_search_value),
	KUNIT_CASE(test_search_value_in_subfolder),
	KUNIT_CASE(test_search_value_in_empty_folder),
	KUNIT_CASE(test_add_aggregate),
	KUNIT_CASE(test_add_aggregate_in_subfolder),
	KUNIT_CASE(test_search_aggregate),
	KUNIT_CASE(test_search_aggregate_in_subfolder),
	KUNIT_CASE(test_search_same),
	KUNIT_CASE(test_add_mixed),
	KUNIT_CASE(test_search_mixed),
	KUNIT_CASE(test_all_aggregations_agg_val_val),
	KUNIT_CASE(test_all_aggregations_val_agg_val),
	KUNIT_CASE(test_all_aggregations_agg_val_val_sub),
	KUNIT_CASE(test_all_aggregations_agg_no_val_sub),
	KUNIT_CASE(test_all_aggregations_agg_agg_val_sub),
	{}
};

static struct kunit_suite stats_fs_test_suite = {
	.name = "stats_fs",
	.test_cases = stats_fs_test_cases,
};

kunit_test_suite(stats_fs_test_suite);
