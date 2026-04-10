// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU core parking for Snapdragon X Elite.
 */

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/bitops.h>

#define DRIVER_NAME	"cpu_parking"
#define MAX_CPUS	16

static int set_enabled(const char *val, const struct kernel_param *kp);
static const struct kernel_param_ops enabled_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

static bool enabled = true;
module_param_cb(enabled, &enabled_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "Enable/disable parking (default: 1)");

static unsigned int sample_interval_ms = 1000;
module_param(sample_interval_ms, uint, 0644);
MODULE_PARM_DESC(sample_interval_ms, "Polling interval in ms (default: 1000)");

static unsigned int busy_up_pct = 75;
module_param(busy_up_pct, uint, 0644);
MODULE_PARM_DESC(busy_up_pct, "Avg util%% above which to unpark a core (default: 75)");

static unsigned int busy_down_pct = 30;
module_param(busy_down_pct, uint, 0644);
MODULE_PARM_DESC(busy_down_pct, "Avg util%% below which to park a core (default: 30)");

static unsigned int min_online;
module_param(min_online, uint, 0644);
MODULE_PARM_DESC(min_online, "Minimum P-cores to keep online (default: 0)");

static unsigned int parkable_cpus = 0xFF0;
module_param(parkable_cpus, uint, 0644);
MODULE_PARM_DESC(parkable_cpus, "Bitmask of parkable CPUs (default: 0xFF0 = CPUs 4-11)");

static unsigned int parked_mask;
module_param_named(parked_cpus, parked_mask, uint, 0444);
MODULE_PARM_DESC(parked_cpus, "Current parked CPU bitmask (read-only)");

static unsigned int last_avg_util;
module_param(last_avg_util, uint, 0444);
MODULE_PARM_DESC(last_avg_util, "Last average utilization %% (read-only)");

static u64 prev_idle[MAX_CPUS];
static u64 prev_wall[MAX_CPUS];
static struct delayed_work parking_work;

static void snapshot_cpu(unsigned int cpu)
{
	u64 wall;

	if (cpu >= MAX_CPUS)
		return;
	prev_idle[cpu] = get_cpu_idle_time_us(cpu, &wall);
	prev_wall[cpu] = wall;
}

static void snapshot_all_online(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu)
		snapshot_cpu(cpu);
}

static void park_cpu(unsigned int cpu)
{
	if (!cpu_online(cpu))
		return;
	if (remove_cpu(cpu)) {
		pr_warn(DRIVER_NAME ": failed to park cpu%u\n", cpu);
		return;
	}
	parked_mask |= BIT(cpu);
	pr_info(DRIVER_NAME ": parked cpu%u (parked=0x%03x)\n", cpu, parked_mask);
}

static void unpark_cpu(unsigned int cpu)
{
	if (cpu_online(cpu))
		return;
	if (add_cpu(cpu)) {
		pr_warn(DRIVER_NAME ": failed to unpark cpu%u\n", cpu);
		return;
	}
	parked_mask &= ~BIT(cpu);
	snapshot_cpu(cpu);
	pr_info(DRIVER_NAME ": unparked cpu%u (parked=0x%03x)\n", cpu, parked_mask);
}

static void unpark_all(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		if (parked_mask & BIT(cpu))
			unpark_cpu(cpu);
	}
}

static void parking_work_fn(struct work_struct *work)
{
	unsigned int cpu, nr_sampled = 0, total_util = 0;
	unsigned int online_parkable = 0, parked_parkable;
	u64 idle, wall, d_idle, d_wall;
	int target;

	if (!enabled) {
		unpark_all();
		return;
	}

	for_each_online_cpu(cpu) {
		if (cpu >= MAX_CPUS)
			continue;

		idle = get_cpu_idle_time_us(cpu, &wall);
		if (idle == (u64)-1)
			continue;

		d_idle = idle - prev_idle[cpu];
		d_wall = wall - prev_wall[cpu];
		prev_idle[cpu] = idle;
		prev_wall[cpu] = wall;

		if (d_wall > 0 && d_idle <= d_wall)
			total_util += (unsigned int)((d_wall - d_idle) * 100 / d_wall);

		nr_sampled++;
		if (parkable_cpus & BIT(cpu))
			online_parkable++;
	}

	if (nr_sampled == 0)
		goto resched;

	last_avg_util = total_util / nr_sampled;
	parked_parkable = hweight32(parked_mask & parkable_cpus);

	if (last_avg_util > busy_up_pct && parked_parkable > 0) {
		for (cpu = 0; cpu < MAX_CPUS; cpu++) {
			if ((parked_mask & BIT(cpu)) &&
			    (parkable_cpus & BIT(cpu))) {
				unpark_cpu(cpu);
				break;
			}
		}
	} else if (last_avg_util < busy_down_pct &&
		   online_parkable > min_online) {
		for (target = MAX_CPUS - 1; target >= 0; target--) {
			if ((parkable_cpus & BIT(target)) &&
			    cpu_online(target) &&
			    !(parked_mask & BIT(target))) {
				park_cpu(target);
				break;
			}
		}
	}

resched:
	schedule_delayed_work(&parking_work,
			      msecs_to_jiffies(sample_interval_ms));
}

static int set_enabled(const char *val, const struct kernel_param *kp)
{
	bool was_enabled = enabled;
	int ret;

	ret = param_set_bool(val, kp);
	if (ret)
		return ret;

	if (!was_enabled && enabled) {
		snapshot_all_online();
		schedule_delayed_work(&parking_work,
				      msecs_to_jiffies(sample_interval_ms));
		pr_info(DRIVER_NAME ": re-enabled\n");
	}
	return 0;
}

static int __init cpu_parking_init(void)
{
	snapshot_all_online();
	INIT_DELAYED_WORK(&parking_work, parking_work_fn);
	schedule_delayed_work(&parking_work,
			      msecs_to_jiffies(sample_interval_ms));

	pr_info(DRIVER_NAME ": loaded (parkable=0x%03x interval=%ums up=%u%% down=%u%%)\n",
		parkable_cpus, sample_interval_ms, busy_up_pct, busy_down_pct);
	return 0;
}

static void __exit cpu_parking_exit(void)
{
	cancel_delayed_work_sync(&parking_work);
	unpark_all();
	pr_info(DRIVER_NAME ": unloaded, all cores online\n");
}

module_init(cpu_parking_init);
module_exit(cpu_parking_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CPU core parking for Snapdragon X Elite (x1e80100)");
