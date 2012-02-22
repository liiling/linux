// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/metricfs.h>
#include <linux/module.h>

/* A metric to force truncation of the values file. "values" files in
 * metricfs can be at most 64K in size. It truncates to the last record
 * that fits entirely in the output file.
 *
 * Creates a metric with a values file that looks like:
 * val"0" 0
 * val"1" 1
 * val"2" 2
 * ...
 * "val"3565" 3565
 */
static void more_than_64k_fn(struct metric_emitter *e)
{
	char buf[80];
	int i;

	for (i = 0; i < 10000; i++) {
		sprintf(buf, "val\"%d\"", i);
		/* Argument order is (emitter, value, field0, field1...) */
		METRIC_EMIT_INT(e, i, buf, NULL);
	}
}
METRIC_EXPORT_INT(more_than_64k, "Stress test metric.",
			"v", NULL, more_than_64k_fn);


/* A metric with two string fields and int64 values.
 *
 * # cat /sys/kernel/debug/metricfs/two_string_fields/annotations
 * DESCRIPTION "Two fields example."
 * # cat /sys/kernel/debug/metricfs/two_string_fields/fields
 * disk cgroup value
 * str str int
 * # cat /sys/kernel/debug/metricfs/two_string_fields/values
 * sda /map_reduce1 0
 * sda /sys 50
 * sdb /map_reduce2 12
 */
static void two_string_fields_fn(struct metric_emitter *e)
{
#define NR_ENTRIES 3
	const char *disk[NR_ENTRIES] = {"sda", "sda", "sdb"};
	const char *cgroups[NR_ENTRIES] = {
				"/map_reduce1", "/sys", "/map_reduce2"};
	const int64_t counters[NR_ENTRIES] = {0, 50, 12};
	int i;

	for (i = 0; i < NR_ENTRIES; i++) {
		METRIC_EMIT_INT(e,
				counters[i], disk[i], cgroups[i]);
	}
}
#undef NR_ENTRIES
METRIC_EXPORT_INT(two_string_fields, "Two fields example.",
			"disk", "cgroup", two_string_fields_fn);


/* A metric with zero fields and a string value.
 *
 * # cat /sys/kernel/debug/metricfs/string_valued_metric/annotations
 * DESCRIPTION "String metric."
 * # cat /sys/kernel/debug/metricfs/string_valued_metric/fields
 * value
 * str
 * # cat /sys/kernel/debug/metricfs/string_valued_metric/values
 * Test\ninfo.
 */
static void string_valued_metric_fn(struct metric_emitter *e)
{
	METRIC_EMIT_STR(e, "Test\ninfo.", NULL, NULL);
}
METRIC_EXPORT_STR(string_valued_metric, "String metric.",
			NULL, NULL, string_valued_metric_fn);

/* Test metric to ensure we behave properly with a large annotation string. */
static void huge_annotation_fn(struct metric_emitter *e)
{
	METRIC_EMIT_STR(e, "test\n", NULL, NULL);
}
static const char *huge_annotation_s =
	"1231231231231231231231231231231241241212895781930750981347503485"
	"7029348750923847502384750923847590234857902348759023475028934751"
	"1111111111111112312312312312312312312312312312412412128957819307"
	"5098134750348570293487509238475023847509238475902348579023487590"
	"2347502893475 23123123123123123123123123123124124121289578193075"
	"0981347503485702934875092384750238475092384759023485790234875902"
	"347502893475 231231231231231231231231231231241241212895781930750"
	"9813475034857029348750923847502384750923847590234857902348759023"
	"47502893475 2312312312312312312312312312312412412128957819307509"
	"8134750348570293487509238475023847509238475902348579023487590234"
	"7502893475 23123123123123123123123123123124124121289578193075098"
	"1347503485702934875092384750238475092384759023485790234875902347"
	"502893475 231231231231231231231231231231241241212895781930750981"
	"3475034857029348750923847502384750923847590234857902348759023475"
	"02893475 2312312312312312312312312312312412412128957819307509813"
	"4750348570293487509238475023847509238475902348579023487590234750"
	"2893475 23123123123123123123123123123124124121289578193075098134"
	"7503485702934875092384750238475092384759023485790234875902347502"
	"893475 231231231231231231231231231231241241212895781930750981347"
	"5034857029348750923847502384750923847590234857902348759023475028"
	"93475 2312312312312312312312312312312412412128957819307509813475"
	"0348570293487509238475023847509238475902348579023487590234750289"
	"3475 23123123123123123123123123123124124121289578193075098134750"
	"3485702934875092384750238475092384759023485790234875902347502893"
	"475 231231231231231231231231231231241241212895781930750981347503"
	"4857029348750923847502384750923847590234857902348759023475028934"
	"75 2312312312312312312312312312312412412128957819307509813475034"
	"8570293487509238475023847509238475902348579023487590234750289347"
	"5 23123123123123123123123123123124124121289578193075098134750348"
	"5702934875092384750238475092384759023485790234875902347502893475"
	" 231231231231231231231231231231241241212895781930750981347503485"
	"702934875092384750238475092384759023485790234875902347502893475 "
	"2312312312312312312312312312312412412128957819307509813475034857"
	"02934875092384750238475092384759023485790234875902347502893475";

METRIC_EXPORT_STR(huge_annotation, huge_annotation_s, NULL, NULL,
			huge_annotation_fn);


struct metricfs_subsys *examples_subsys;

static int __init metricfs_examples_init(void)
{
	examples_subsys = metricfs_create_subsys("examples", NULL);
	metric_init_more_than_64k(examples_subsys);
	metric_init_two_string_fields(examples_subsys);
	metric_init_string_valued_metric(examples_subsys);
	metric_init_huge_annotation(examples_subsys);

	return 0;
}

static void __exit metricfs_examples_exit(void)
{
	metric_exit_more_than_64k();
	metric_exit_two_string_fields();
	metric_exit_string_valued_metric();
	metric_exit_huge_annotation();

	metricfs_destroy_subsys(examples_subsys);
}

module_init(metricfs_examples_init);
module_exit(metricfs_examples_exit);

MODULE_LICENSE("GPL");
