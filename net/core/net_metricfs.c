// SPDX-License-Identifier: GPL-2.0
/* net_metricfs: Exports network counters using metricfs.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/metricfs.h>
#include <linux/netdevice.h>
#include <linux/nsproxy.h>
#include <linux/rcupdate.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <net/net_namespace.h>

struct metric_def {
	struct metric *metric;
	size_t off;
	char *name;
	char *desc;
};

/* If needed, we could export this via a function for other /net users */
static struct metricfs_subsys *net_root_subsys;
static struct metricfs_subsys *dev_subsys;
static struct metricfs_subsys *dev_stats_subsys;

static struct metric_def metric_def[] = {
	{NULL, offsetof(struct rtnl_link_stats64, rx_bytes),
	 "rx_bytes", "net device received bytes count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_packets),
	 "rx_packets", "net device received packets count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_errors),
	 "rx_errors", "net device received errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_dropped),
	 "rx_dropped", "net device dropped packets count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_missed_errors),
	 "rx_missed_errors",  "net device missed errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_fifo_errors),
	 "rx_fifo_errors", "net device fifo errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_length_errors),
	 "rx_length_errors", "net device length errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_over_errors),
	 "rx_over_errors", "net device received overflow errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_crc_errors),
	 "rx_crc_errors", "net device received crc errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_frame_errors),
	 "rx_frame_errors", "net device received frame errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, rx_compressed),
	 "rx_compressed", "net device received compressed packet count"},
	{NULL, offsetof(struct rtnl_link_stats64, multicast),
	 "rx_multicast", "net device received multicast packet count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_bytes),
	 "tx_bytes", "net device transmited bytes count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_packets),
	 "tx_packets", "net device transmited packets count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_errors),
	 "tx_errors", "net device transmited errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_dropped),
	 "tx_dropped", "net device transmited packet drop count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_fifo_errors),
	 "tx_fifo_errors", "net device transmit fifo errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, collisions),
	 "tx_collision", "net device transmit collisions count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_carrier_errors),
	 "tx_carrier_errors", "net device transmit carrier errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_aborted_errors),
	 "tx_aborted_errors", "net device transmit aborted errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_window_errors),
	 "tx_window_errors", "net device transmit window errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_heartbeat_errors),
	 "tx_heartbeat_errors", "net device transmit heartbeat errors count"},
	{NULL, offsetof(struct rtnl_link_stats64, tx_compressed),
	 "tx_compressed_errors", "net device transmit compressed count"},
};

static __init int init_net_subsys(void)
{
	net_root_subsys = metricfs_create_subsys("net", NULL);
	if (!net_root_subsys) {
		WARN_ONCE(1, "Net metricfs root not created.");
		return -1;
	}
	return 0;
}

late_initcall(init_net_subsys);

static void dev_stats_emit(struct metric_emitter *e,
			   struct net_device *dev,
			   struct metric_def *metricd)
{
	struct rtnl_link_stats64 temp;
	const struct rtnl_link_stats64 *stats = dev_get_stats(dev, &temp);

	if (stats) {
		__u8 *ptr = (((__u8 *)stats) + metricd->off);

		METRIC_EMIT_INT(e, *(__u64 *)ptr, dev->name, NULL);
	}
}

/* metricfs export function */
static void dev_stats_fn(struct metric_emitter *e, void *parm)
{
	struct net_device *dev;
	struct net *net;
	struct nsproxy *nsproxy = current->nsproxy;

	rcu_read_lock();
	for_each_net_rcu(net) {
		/* skip namespaces not associated with the caller */
		if (nsproxy->net_ns != net)
			continue;
		for_each_netdev_rcu(net, dev) {
			dev_stats_emit(e, dev, (struct metric_def *)parm);
		}
	}
	rcu_read_unlock();
}

static void clean_dev_stats_subsys(void)
{
	int x;
	int metric_count = sizeof(metric_def) / sizeof(struct metric_def);

	for (x = 0; x < metric_count; x++) {
		if (metric_def[x].metric) {
			metric_unregister(metric_def[x].metric);
			metric_def[x].metric = NULL;
		}
	}
	if (dev_stats_subsys)
		metricfs_destroy_subsys(dev_stats_subsys);
	if (dev_subsys)
		metricfs_destroy_subsys(dev_subsys);
	dev_stats_subsys = NULL;
	dev_subsys = NULL;
}

static int __init init_dev_stats_subsys(void)
{
	int x;
	int metric_count = sizeof(metric_def) / sizeof(struct metric_def);

	dev_subsys = NULL;
	dev_stats_subsys = NULL;
	if (!net_root_subsys) {
		WARN_ONCE(1, "Net metricfs root not initialized.");
		goto error;
	}
	dev_subsys =
		metricfs_create_subsys("dev", net_root_subsys);
	if (!dev_subsys) {
		WARN_ONCE(1, "Net metricfs dev not created.");
		goto error;
	}
	dev_stats_subsys =
		metricfs_create_subsys("stats", dev_subsys);
	if (!dev_stats_subsys) {
		WARN_ONCE(1, "Dev metricfs stats not created.");
		goto error;
	}

	/* initialize each of the metrics */
	for (x = 0; x < metric_count; x++) {
		metric_def[x].metric =
			metric_register_parm(metric_def[x].name,
					     dev_stats_subsys,
					     metric_def[x].desc,
					     "interface",
					     NULL,
					     dev_stats_fn,
					     (void *)&metric_def[x],
					     false,
					     true,  /* this is a counter */
					     THIS_MODULE);
		if (!metric_def[x].metric) {
			WARN_ONCE(1, "Dev metricfs stats %s not registered.",
				  metric_def[x].name);
			goto error;
		}
	}
	return 0;
error:
	clean_dev_stats_subsys();
	return -1;
}

/* need to wait for metricfs and net metricfs root to be initialized */
late_initcall_sync(init_dev_stats_subsys);

static void __exit dev_stats_exit(void)
{
	clean_dev_stats_subsys();
}
