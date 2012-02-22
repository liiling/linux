// SPDX-License-Identifier: GPL-2.0
#include <linux/refcount.h>
#include <linux/dcache.h>
#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/metricfs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/mm.h>

/*
 * Metricfs: A mechanism for exporting metrics from the kernel.
 *
 * Kernel code must provide:
 *   - A description of the metric
 *   - The subsystem for the metric (NULL is ok)
 *   - Type information about the metric, and
 *   - A callback function which supplies metric values.
 *
 * In return, metricfs provides files in debugfs at:
 *   /sys/kernel/debug/metricfs/<subsys>/<metric_name>/
 * The files are:
 *   - annotations, which provides streamz "annotations"-- the description, and
 *                  other metadata (e.g. if it's constant, deprecated, etc.)
 *   - fields, which provides type information about the metric and its fields.
 *   - values, which contains the actual metric value data.
 *   - version, which is kept around for future-proofing.
 *
 * Metrics only support a limited subset of types-- for fields, they only
 * support strings, integers, and boolean types. For simplicity, we only support
 * strings and integers and strictly control how the data is formatted when
 * displayed from debugfs.
 *
 * See kernel/metricfs_examples.c for example code.
 *
 * Limitations:
 *   - "values" files are at MOST 64K. We truncate the file at that point.
 *   - The list of fields and types is at most 1K.
 *   - Metrics may have at most 2 fields.
 *
 * Best Practices:
 *   - Emit the most important data first! Once the 64K per-metric buffer
 *     is full, the emit* functions won't do anything.
 *   - In userspace, open(), read(), and close() the file quickly! The kernel
 *     allocation for the metric is alive as long as the file is open. This
 *     permits users to seek around the contents of the file, while permitting
 *     an atomic view of the data.
 *
 * FAQ:
 *   - Why is memory allocated for file data at open()?
 *     Snapshots of data provided by the kernel should be as "atomic" as
 *     possible. If userspace code performs read()s smaller than the total
 *     amount of data, we'd like for that tool to still work, while providing a
 *     consistent view of the file.
 *
 * Questions:
 *   - Would it be simpler if we escaped spaces instead of wrapping strings in
 *     quotes?
 */
struct metric {
	const char *name;
	const char *description;

	/* Metric field names (optional, NULL if unused) */
	const char *fname0;
	const char *fname1;

	union {
		void (*emit_noparm)(struct metric_emitter *e); /* !has_parm */
		void (*emit_parm)(struct metric_emitter *e,
				  void *parm); /* has_parm */
	} emit_fn;
	void *eparm;
	bool is_string;
	bool is_cumulative;
	bool has_parm;

	/* dentry for the directory that contains the metric */
	struct dentry *dentry;

	struct module *owner;

	refcount_t refcnt;

	/* Inodes that have references to our metric, protected under
	 * big_mutex.
	 */
	struct inode *inodes[4];
};

/* Returns true if the refcount was successfully incremented for the metric */
static int metric_module_get(struct metric *m)
{
	if (!try_module_get(m->owner))
		return 0;

	if (!refcount_inc_not_zero(&m->refcnt)) {
		module_put(m->owner);
		return 0;
	}

	return 1;
}

/* Returns true if the last reference was put. */
static bool metric_put(struct metric *m)
{
	bool rc = refcount_dec_and_test(&m->refcnt);

	if (rc)
		kfree(m);
	return rc;
}

static void metric_module_put(struct metric *m)
{
	struct module *owner = m->owner;

	metric_put(m);
	module_put(owner);
}

struct metric_emitter {
	char *buf;
	char *orig_buf;  /* To calculate total written. */
	int size;  /* Size of underlying buffer. */
	struct metric *metric;  /* For type checking. */
};

#define METRICFS_ANNOTATIONS_BUF_SIZE (1 * 1024)
#define METRICFS_FIELDS_BUF_SIZE (1 * 1024)
#define METRICFS_VALUES_BUF_SIZE (64 * 1024)
#define METRICFS_VERSION_BUF_SIZE (8)

/* Maximum length for fields. They're truncated at this point. */
#define METRICFS_MAX_FIELD_LEN (100)

static int emit_bytes_left(const struct metric_emitter *e)
{
	WARN_ON(e->orig_buf > e->buf);
	return e->size - (e->buf - e->orig_buf);
}

struct char_tracker {
	char *dest;
	int size;
	int pos;
};

static void add_char(struct char_tracker *t, char c)
{
	if (t->pos < t->size)
		t->dest[t->pos] = c;
	/* Increment pos even if we don't print, so we know how many
	 * characters we'd print if we had room.
	 */
	t->pos++;
}

/* Escape backslashes, spaces, and newlines in string "s",
 * copying to "dest", to a maximum of "size" characters.
 *
 * examples:
 *  [Hi\ , "there"] -> [Hi\\\ ,\ "there"]
 *  [foo
 *   bar] - > [foo\nbar]
 *
 * Returns the number of characters that would be copied, if enough space
 * was available. Doesn't emit a trailing zero.
 */
static int escape_string(char *dest, const char *s, int size)
{
	struct char_tracker tracker = {
		.dest = dest,
		.size = size,
		.pos = 0,
	};

	/* We have to process the entire source string to ensure that
	 * we return a useful value for the total possible emitted length.
	 */
	while (*s != 0) {
		/* escape newlines */
		if (*s == '\n') {
			add_char(&tracker, '\\');
			add_char(&tracker, 'n');
			s++;
			continue;
		}

		/* escape spaces and backslashes. */
		if (*s == ' ' || *s == '\\')
			add_char(&tracker, '\\');
		add_char(&tracker, *s);
		s++;
	}

	return tracker.pos;
}

/* Emits a string into the emitter buffer, no escaping */
static bool emit_string(struct metric_emitter *e, const char *s)
{
	int bytes_left = emit_bytes_left(e);
	int rc = snprintf(e->buf, bytes_left, "%s", s);

	e->buf += min(rc, bytes_left);
	return rc < bytes_left;
}

/* Emits a string into the emitter buffer, escaping quotes and newlines. */
static bool emit_quoted_string(struct metric_emitter *e, const char *s)
{
	int bytes_left = emit_bytes_left(e);
	int rc = escape_string(e->buf, s, bytes_left);

	e->buf += min(rc, bytes_left);
	return rc < bytes_left;
}

/* Emits an int into the emitter buffer */
static bool emit_int(struct metric_emitter *e, int64_t i)
{
	int bytes_left = emit_bytes_left(e);
	int rc = snprintf(e->buf, bytes_left, "%lld", i);

	e->buf += min(rc, bytes_left);
	return rc < bytes_left;
}

static void check_field_mismatch(struct metric *m, const char *f0,
				 const char *f1)
{
	WARN_ON(m->fname0 && !f0);
	WARN_ON(!m->fname0 && f0);
	WARN_ON(m->fname1 && !f1);
	WARN_ON(!m->fname1 && f1);
}

void metric_emit_int_value(struct metric_emitter *e, int64_t v,
			   const char *f0, const char *f1)
{
	char *ckpt = e->buf;
	bool ok = true;

	WARN_ON_ONCE(e->metric->is_string);
	check_field_mismatch(e->metric, f0, f1);
	if (f0) {
		ok &= emit_quoted_string(e, f0);
		ok &= emit_string(e, " ");
		if (f1) {
			ok &= emit_quoted_string(e, f1);
			ok &= emit_string(e, " ");
		}
	}
	ok &= emit_int(e, v);
	ok &= emit_string(e, "\n");
	if (!ok)
		e->buf = ckpt;
}
EXPORT_SYMBOL(metric_emit_int_value);

void metric_emit_str_value(struct metric_emitter *e, const char *v,
			   const char *f0, const char *f1)
{
	char *ckpt = e->buf;
	bool ok = true;

	WARN_ON_ONCE(!e->metric->is_string);
	check_field_mismatch(e->metric, f0, f1);
	if (f0) {
		ok &= emit_quoted_string(e, f0);
		ok &= emit_string(e, " ");
		if (f1) {
			ok &= emit_quoted_string(e, f1);
			ok &= emit_string(e, " ");
		}
	}
	ok &= emit_quoted_string(e, v);
	ok &= emit_string(e, "\n");
	if (!ok)
		e->buf = ckpt;
}
EXPORT_SYMBOL(metric_emit_str_value);

/* Contains file data generated at open() */
struct metricfs_file_private {
	size_t bytes_written;
	char buf[0];
};

/* A mutex to prevent races involving the pointer to the inode stored in
 * inode->i_private. We'll remove this if we can get a callback at inode
 * deletion in debugfs.
 */
static DEFINE_MUTEX(big_mutex);

/* Returns 1 on success, <0 otherwise. */
static int metric_open_helper(struct inode *inode, struct file *filp,
			      int buf_size,
			      struct metric **m,
			      struct metricfs_file_private **p)
{
	int size;

	mutex_lock(&big_mutex);
	/* Debugfs stores the "data" parameter from debugfs_create_file in
	 * inode->i_private.
	 */
	*m = (struct metric *)inode->i_private;
	if (!(*m) || !metric_module_get(*m)) {
		mutex_unlock(&big_mutex);
		return -ENXIO;
	}
	mutex_unlock(&big_mutex);

	size = sizeof(struct metricfs_file_private) + buf_size;
	*p = kvmalloc(size, GFP_KERNEL);
	if (!*p) {
		metric_module_put(*m);
		return -ENOMEM;
	}
	filp->private_data = *p;
	return 1;
}

static int metricfs_generic_release(struct inode *inode, struct file *filp)
{
	struct metricfs_file_private *p =
			(struct metricfs_file_private *)filp->private_data;
	kvfree(p);

	filp->private_data = (void *)(0xDEADBEEFul + POISON_POINTER_DELTA);
	/* FIXME here too? */
	metric_module_put((struct metric *)inode->i_private);
	return 0;
}

static int metricfs_annotations_open(struct inode *inode, struct file *filp)
{
	struct metric_emitter e;
	struct metric *m;
	struct metricfs_file_private *p;
	bool ok = true;

	int rc = metric_open_helper(inode, filp, METRICFS_ANNOTATIONS_BUF_SIZE,
				    &m, &p);
	if (rc < 0)
		return rc;

	e.buf = p->buf;
	e.orig_buf = p->buf;
	e.size = METRICFS_ANNOTATIONS_BUF_SIZE;
	ok &= emit_string(&e, "DESCRIPTION ");
	ok &= emit_quoted_string(&e, m->description);
	ok &= emit_string(&e, "\n");
	if (m->is_cumulative)
		ok &= emit_string(&e, "CUMULATIVE\n");

	/* Emit all or nothing. */
	if (ok) {
		p->bytes_written = e.buf - e.orig_buf;
	} else {
		metricfs_generic_release(inode, filp);
		return -ENOMEM;
	}

	return 0;
}

static int metricfs_fields_open(struct inode *inode, struct file *filp)
{
	struct metric_emitter e;
	struct metric *m;
	struct metricfs_file_private *p;
	bool ok = true;

	int rc = metric_open_helper(inode, filp, METRICFS_FIELDS_BUF_SIZE,
				    &m, &p);
	if (rc < 0)
		return rc;

	e.buf = p->buf;
	e.orig_buf = p->buf;
	e.size = METRICFS_FIELDS_BUF_SIZE;
	e.metric = m;

	/* We don't have to do string escaping on fields, as quotes aren't
	 * permitted in field names.
	 */
	if (m->fname0) {
		ok &= emit_string(&e, m->fname0);
		ok &= emit_string(&e, " ");
	}
	if (m->fname1) {
		ok &= emit_string(&e, m->fname1);
		ok &= emit_string(&e, " ");
	}
	ok &= emit_string(&e, "value\n");

	if (m->fname0)
		ok &= emit_string(&e, "str ");
	if (m->fname1)
		ok &= emit_string(&e, "str ");
	ok &= emit_string(&e, (m->is_string) ? "str\n" : "int\n");

	/* Emit all or nothing. */
	if (ok) {
		p->bytes_written = e.buf - e.orig_buf;
	} else {
		metricfs_generic_release(inode, filp);
		return -ENOMEM;
	}

	return 0;
}

static int metricfs_version_open(struct inode *inode, struct file *filp)
{
	struct metric *m;
	struct metricfs_file_private *p;
	int rc = metric_open_helper(inode, filp, METRICFS_VERSION_BUF_SIZE,
				    &m, &p);
	if (rc < 0)
		return rc;

	p->bytes_written = snprintf(p->buf, METRICFS_VERSION_BUF_SIZE,
				    "1\n");

	if (p->bytes_written >= METRICFS_VERSION_BUF_SIZE) {
		metricfs_generic_release(inode, filp);
		return -ENOMEM;
	}

	return 0;
}

static int metricfs_values_open(struct inode *inode, struct file *filp)
{
	struct metric_emitter e;

	struct metric *m;
	struct metricfs_file_private *p;
	int rc = metric_open_helper(inode, filp, METRICFS_VALUES_BUF_SIZE,
				    &m, &p);
	if (rc < 0)
		return rc;

	e.buf = p->buf;
	e.orig_buf = p->buf;
	e.size = METRICFS_VALUES_BUF_SIZE;
	e.metric = m;

	if (m->has_parm) {
		if (m->emit_fn.emit_parm)
			(m->emit_fn.emit_parm)(&e, m->eparm);
	} else {
		if (m->emit_fn.emit_noparm)
			(m->emit_fn.emit_noparm)(&e);
	}
	p->bytes_written = e.buf - e.orig_buf;
	return 0;
}

static ssize_t metricfs_generic_read(struct file *filp, char __user *ubuf,
				     size_t cnt, loff_t *ppos)
{
	struct metricfs_file_private *p =
			(struct metricfs_file_private *)filp->private_data;
	return simple_read_from_buffer(ubuf, cnt, ppos, p->buf,
					p->bytes_written);
}

static const struct file_operations metricfs_annotations_ops = {
	.open = metricfs_annotations_open,
	.read = metricfs_generic_read,
	.release = metricfs_generic_release,
};

static const struct file_operations metricfs_fields_ops = {
	.open = metricfs_fields_open,
	.read = metricfs_generic_read,
	.release = metricfs_generic_release,
};

static const struct file_operations metricfs_values_ops = {
	.open = metricfs_values_open,
	.read = metricfs_generic_read,
	.release = metricfs_generic_release,
};

static const struct file_operations metricfs_version_ops = {
	.open = metricfs_version_open,
	.read = metricfs_generic_read,
	.release = metricfs_generic_release,
};

static struct dentry *d_metricfs;

static struct dentry *metricfs_init_dentry(void)
{
	static int once;

	if (d_metricfs)
		return d_metricfs;

	if (!debugfs_initialized())
		return NULL;

	d_metricfs = debugfs_create_dir("metricfs", NULL);

	if (!d_metricfs && !once) {
		once = 1;
		pr_warn("Could not create debugfs directory 'metricfs'\n");
		return NULL;
	}

	return d_metricfs;
}

/* We always cast in and out to struct dentry. */
struct metricfs_subsys {
	struct dentry dentry;
};

static struct dentry *metricfs_create_file(const char *name,
					   mode_t mode,
					   struct dentry *parent,
					   void *data,
					   const struct file_operations *fops)
{
	struct dentry *ret;

	ret = debugfs_create_file(name, mode, parent, data, fops);
	if (!ret)
		pr_warn("Could not create debugfs '%s' entry\n", name);

	return ret;
}

static struct dentry *metricfs_create_dir(const char *name,
					  struct metricfs_subsys *s)
{
	struct dentry *d;

	if (!s)
		d = d_metricfs;
	else
		d = &s->dentry;

	if (!d) {
		pr_warn("Couldn't create %s, subsys doesn't exist.", name);
		return NULL;
	}
	return debugfs_create_dir(name, d);
}

static int metricfs_initialized;

struct metric *metric_register(const char *name,
				struct metricfs_subsys *parent,
				const char *description,
				const char *fname0,
				const char *fname1,
				void (*fn)(struct metric_emitter *e),
				bool is_string,
				bool is_cumulative,
				struct module *owner)
{
	struct metric *m;
	struct dentry *d, *t;

	if (!metricfs_initialized) {
		pr_warn("Could not create metric before initing metricfs\n");
		return NULL;
	}

	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return NULL;

	d = metricfs_create_dir(name, parent);
	if (!d) {
		pr_warn("Could not create dir '%s' in metricfs.\n", name);
		kfree(m);
		return NULL;
	}

	m->description = description;
	m->fname0 = fname0;
	m->fname1 = fname1;
	m->has_parm = false;
	m->emit_fn.emit_noparm = fn;
	m->eparm = NULL;
	m->is_string = is_string;
	m->is_cumulative = is_cumulative;
	refcount_set(&m->refcnt, 1);
	m->owner = owner;
	m->dentry = d;


	mutex_lock(&big_mutex);
	t = metricfs_create_file("annotations", 0444, d, m,
					&metricfs_annotations_ops);
	if (!t)
		goto done;
	m->inodes[0] = t->d_inode;

	t = metricfs_create_file("fields", 0444, d, m,
					&metricfs_fields_ops);
	if (!t)
		goto done;
	m->inodes[1] = t->d_inode;

	t = metricfs_create_file("values", 0444, d, m,
					&metricfs_values_ops);
	if (!t)
		goto done;
	m->inodes[2] = t->d_inode;

	t = metricfs_create_file("version", 0444, d, m,
					&metricfs_version_ops);
	if (!t)
		goto done;
	m->inodes[3] = t->d_inode;

done:
	/* Unregister the metric before anyone calls open() if we had any
	 * errors on file creation.
	 */
	if (!t) {
		metric_unregister(m);
		m = NULL;
	}
	mutex_unlock(&big_mutex);

	return m;
}
EXPORT_SYMBOL(metric_register);

struct metric *metric_register_parm(const char *name,
				    struct metricfs_subsys *parent,
				    const char *description,
				    const char *fname0,
				    const char *fname1,
				    void (*fn)(struct metric_emitter *e,
					       void *parm),
				    void *eparm,
				    bool is_string,
				    bool is_cumulative,
				    struct module *owner)
{
	struct metric *metric =
		metric_register(name, parent, description,
				fname0, fname1,
				(void (*)(struct metric_emitter *))NULL,
				is_string,
				is_cumulative, owner);
	if (metric) {
		metric->has_parm = true;
		metric->emit_fn.emit_parm = fn;
		metric->eparm = eparm;
	}
	return metric;
}
EXPORT_SYMBOL(metric_register_parm);

void metric_unregister(struct metric *m)
{
	/* We have to NULL out the i_private pointers here so that no other
	 * callers come into open, getting a pointer to the metric that we
	 * freed.
	 */
	mutex_lock(&big_mutex);
	m->inodes[0]->i_private = NULL;
	m->inodes[1]->i_private = NULL;
	m->inodes[2]->i_private = NULL;
	m->inodes[3]->i_private = NULL;
	mutex_unlock(&big_mutex);

	debugfs_remove_recursive(m->dentry);
	metric_put(m);
}
EXPORT_SYMBOL(metric_unregister);

struct metricfs_subsys *metricfs_create_subsys(const char *name,
					       struct metricfs_subsys *parent)
{
	struct dentry *d = metricfs_create_dir(name, parent);

	return container_of(d, struct metricfs_subsys, dentry);
}
EXPORT_SYMBOL(metricfs_create_subsys);

void metricfs_destroy_subsys(struct metricfs_subsys *s)
{
	if (s)
		debugfs_remove(&s->dentry);
}
EXPORT_SYMBOL(metricfs_destroy_subsys);

static void metricfs_presence_fn(struct metric_emitter *e)
{
	METRIC_EMIT_INT(e, 1, NULL, NULL);
}
METRIC_EXPORT_INT(metricfs_presence, "A basic presence metric.",
			NULL, NULL, metricfs_presence_fn);

static int __init metricfs_init(void)
{
	if (!metricfs_init_dentry())
		return -ENOMEM;
	metricfs_initialized = 1;

	/* Create a basic "presence" metric. */
	metric_init_metricfs_presence(NULL);

	mutex_init(&big_mutex);
	return 0;
}

/*
 * Debugfs should be fine by the time we're at fs_initcall.
 */
fs_initcall(metricfs_init);
