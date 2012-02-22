/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _METRICFS_H_
#define _METRICFS_H_

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stringify.h>

struct metric;
struct metricfs_subsys;

#define METRIC_EXPORT_GENERIC(name, desc, fname0, fname1, fn, is_str, cumulative) \
static struct metric *metric_##name; \
void metric_init_##name(struct metricfs_subsys *parent) \
{ \
	metric_##name = metric_register(__stringify(name), (parent), (desc), \
					(fname0), (fname1), (fn), (is_str), \
					(cumulative), THIS_MODULE); \
} \
void metric_exit_##name(void) \
{ \
	metric_unregister(metric_##name); \
}

/*
 * Metricfs only deals with two types: int64_t and const char*.
 *
 * If a metric has fewer than two fields, pass NULL for the field name
 * arguments.
 *
 * The metric does not take ownership of any of the strings passed in.
 *
 * See kernel/metricfs_examples.c for a set of example metrics, with
 * corresponding output.
 *
 * METRIC_EXPORT_INT - An integer-valued metric.
 * METRIC_EXPORT_COUNTER - An integer-valued cumulative metric.
 * METRIC_EXPORT_STR - A string-valued metric.
 */
#define METRIC_EXPORT_INT(name, desc, fname0, fname1, fn) \
	METRIC_EXPORT_GENERIC(name, (desc), (fname0), (fname1), (fn), \
				false, false)
#define METRIC_EXPORT_COUNTER(name, desc, fname0, fname1, fn) \
	METRIC_EXPORT_GENERIC(name, (desc), (fname0), (fname1), (fn), \
				false, true)
#define METRIC_EXPORT_STR(name, desc, fname0, fname1, fn) \
	METRIC_EXPORT_GENERIC(name, (desc), (fname0), (fname1), (fn), \
				true, false)

/* Subsystem support. */
/* Pass NULL as 'parent' to create a new top-level subsystem. */
struct metricfs_subsys *metricfs_create_subsys(const char *name,
						struct metricfs_subsys *parent);
void metricfs_destroy_subsys(struct metricfs_subsys *d);

/*
 * An opaque struct that metric emit functions use to keep our internal
 * state.
 */
struct metric_emitter;

/* The number of non-NULL arguments passed to EMIT macros must match the number
 * of arguments passed to the EXPORT macro for a given metric.
 *
 * Failure to do so will cause data to be mangled (or dropped) by userspace or
 * Monarch.
 */
#define METRIC_EMIT_INT(e, v, f0, f1) \
	metric_emit_int_value((e), (v), (f0), (f1))
#define METRIC_EMIT_STR(e, v, f0, f1) \
	metric_emit_str_value((e), (v), (f0), (f1))

/* Users don't have to call any functions below;
 * use the macro definitions above instead.
 */
void metric_emit_int_value(struct metric_emitter *e,
			   int64_t v, const char *f0, const char *f1);
void metric_emit_str_value(struct metric_emitter *e,
			   const char *v, const char *f0, const char *f1);

struct metric *metric_register(const char *name,
			       struct metricfs_subsys *parent,
			       const char *description,
			       const char *fname0, const char *fname1,
			       void (*fn)(struct metric_emitter *e),
			       bool is_string,
			       bool is_cumulative,
			       struct module *owner);

struct metric *metric_register_parm(const char *name,
				    struct metricfs_subsys *parent,
			  const char *description,
				    const char *fname0, const char *fname1,
				    void (*fn)(struct metric_emitter *e,
					       void *parm),
				    void *parm,
				    bool is_string,
				    bool is_cumulative,
				    struct module *owner);

void metric_unregister(struct metric *m);

#endif /* _METRICFS_H_ */
