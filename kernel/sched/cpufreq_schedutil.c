/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <trace/events/power.h>
#include <trace/events/sched.h>

#include "sched.h"
#include "tune.h"

#define LATENCY_MULTIPLIER			(1000)
#define SUGOV_KTHREAD_PRIORITY	50

struct sugov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
#ifdef CONFIG_FREQVAR_SCHEDTUNE
	struct freqvar_boost_data freqvar_boost;
#endif
};

struct sugov_policy {
	struct cpufreq_policy *policy;

	struct sugov_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int max_util;
	bool pending;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct kthread_work work;
	struct mutex work_lock;
	struct kthread_worker worker;
	struct task_struct *thread;
	bool work_in_progress;

	bool need_freq_update;
};

struct sugov_cpu {
	struct update_util_data update_util;
	struct sugov_policy *sg_policy;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	u64 last_update;
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);

/************************ Governor internals ***********************/
static unsigned int sugov_next_freq_shared(struct sugov_policy *sg_policy,
					   unsigned long util, unsigned long max);

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;
	struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, smp_processor_id());
	bool up_scale = (sg_cpu->util > sg_policy->max_util);

	if (up_scale) {
		if (!sg_policy->work_in_progress)
			return true;
		else {
			sg_policy->next_freq = sugov_next_freq_shared(sg_policy,
							sg_cpu->util, sg_cpu->max);
			sg_policy->pending = true;
			return false;
		}
	}

	if (sg_policy->work_in_progress)
		return false;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		/*
		 * This happens when limits change, so forget the previous
		 * next_freq value and force an update.
		 */
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;
	

	/* No need to recalculate next freq for min_rate_limit_us at least */
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;
 	delta_ns = time - sg_policy->last_freq_update_time;
 	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;
 	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;
 	return false;
}

static int sugov_select_scaling_cpu(void)
{
	int cpu;
	cpumask_t mask;

	cpumask_clear(&mask);
	cpumask_and(&mask, cpu_coregroup_mask(0), cpu_online_mask);

	/* Idle core of the boot cluster is selected to scaling cpu */
	for_each_cpu(cpu, &mask)
		if (idle_cpu(cpu))
			return cpu;

	return cpumask_weight(&mask) - 1;
}

static void sugov_update_commit(struct sugov_policy *sg_policy, u64 time,
				unsigned int next_freq)
{
	if (sugov_up_down_rate_limit(sg_policy, time, next_freq))
		return;

	sg_policy->last_freq_update_time = time;

	if (sg_policy->next_freq != next_freq) {
		sg_policy->next_freq = next_freq;
		sg_policy->work_in_progress = true;
		irq_work_queue_on(&sg_policy->irq_work,
			sugov_select_scaling_cpu());
	}
}

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @policy: cpufreq policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 */
static unsigned int get_next_freq(struct cpufreq_policy *policy,
				  unsigned long util, unsigned long max)
{
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->cpuinfo.max_freq : policy->cur;

	return (freq + (freq >> 2)) * util / max;
}

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned long util, unsigned long max)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int next_f;

	if (!sugov_should_update_freq(sg_policy, time))
		return;

	next_f = util == ULONG_MAX ? policy->cpuinfo.max_freq :
			get_next_freq(policy, util, max);

	trace_sched_freq_commit(time, util, max, next_f);

	sugov_update_commit(sg_policy, time, next_f);
}

static unsigned int sugov_next_freq_shared(struct sugov_policy *sg_policy,
					   unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int max_f = policy->cpuinfo.max_freq;
	u64 last_freq_update_time = sg_policy->last_freq_update_time;
	unsigned int j;

	if (util == ULONG_MAX)
		goto return_max;

	for_each_cpu(j, policy->cpus) {
		struct sugov_cpu *j_sg_cpu;
		unsigned long j_util, j_max;
		s64 delta_ns;

		if (j == smp_processor_id())
			continue;

		j_sg_cpu = &per_cpu(sugov_cpu, j);
		/*
		 * If the CPU utilization was last updated before the previous
		 * frequency update and the time elapsed between the last update
		 * of the CPU utilization and the last frequency update is long
		 * enough, don't take the CPU into account as it probably is
		 * idle now.
		 */
		delta_ns = last_freq_update_time - j_sg_cpu->last_update;
		if (delta_ns > TICK_NSEC)
			continue;

		j_util = j_sg_cpu->util;
		if (j_util == ULONG_MAX)
			goto return_max;

		j_max = j_sg_cpu->max;
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}
	}

	sg_policy->max_util = util;
	return get_next_freq(policy, util, max);

return_max:
	sg_policy->max_util = max;
	return max_f;

}

static void sugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned long util, unsigned long max)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned int next_f;

	raw_spin_lock(&sg_policy->update_lock);

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->last_update = time;

	if (sugov_should_update_freq(sg_policy, time)) {
		next_f = sugov_next_freq_shared(sg_policy, util, max);

		trace_sched_freq_commit(time, util, max, next_f);

		sugov_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct kthread_work *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);

	mutex_lock(&sg_policy->work_lock);

	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
			CPUFREQ_RELATION_L);
	/* if frequency up scaling is in pending, retry scaling */
	if (sg_policy->pending) {
		__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
		sg_policy->pending = false;
	}

	mutex_unlock(&sg_policy->work_lock);

	sg_policy->work_in_progress = false;
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);

	/*
	 * For Real Time and Deadline tasks, schedutil governor shoots the
	 * frequency to maximum. And special care must be taken to ensure that
	 * this kthread doesn't result in that.
	 *
	 * This is (mostly) guaranteed by the work_in_progress flag. The flag is
	 * updated only at the end of the sugov_work() and before that schedutil
	 * rejects all other frequency scaling requests.
	 *
	 * Though there is a very rare case where the RT thread yields right
	 * after the work_in_progress flag is cleared. The effects of that are
	 * neglected for now.
	 */
	queue_kthread_work(&sg_policy->worker, &sg_policy->work);
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_us(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
 	return sprintf(buf, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}

	return count;
}
#ifdef CONFIG_FREQVAR_SCHEDTUNE
static unsigned int *get_tokenized_data(const char *buf, int *num_tokens)
{
	const char *cp;
	int i;
	int ntokens = 1;
	unsigned int *tokenized_data;
	int err = -EINVAL;

	cp = buf;
	while ((cp = strpbrk(cp + 1, " :")))
		ntokens++;

	if (!(ntokens & 0x1))
		goto err;

	tokenized_data = kmalloc(ntokens * sizeof(unsigned int), GFP_KERNEL);
	if (!tokenized_data) {
		err = -ENOMEM;
		goto err;
	}

	cp = buf;
	i = 0;
	while (i < ntokens) {
		if (sscanf(cp, "%u", &tokenized_data[i++]) != 1)
			goto err_kfree;

		cp = strpbrk(cp, " :");
		if (!cp)
			break;
		cp++;
	}

	if (i != ntokens)
		goto err_kfree;

	*num_tokens = ntokens;
	return tokenized_data;

err_kfree:
	kfree(tokenized_data);
err:
	return ERR_PTR(err);
}

static ssize_t freqvar_boost_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct freqvar_boost_data *data = &tunables->freqvar_boost;
	struct freqvar_boost_table *pos = data->table;
	int ret = 0;

	for (; pos->frequency != CPUFREQ_TABLE_END; pos++)
		ret += sprintf(buf + ret, "%8d ratio:%3d \n", pos->frequency,
					pos->boost / SCHEDTUNE_LOAD_BOOST_UTIT);
	return ret;
}

static ssize_t freqvar_boost_store(struct gov_attr_set *attr_set, const char *buf,
				   size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct freqvar_boost_data *data = &tunables->freqvar_boost;
	int *new_table = NULL;
	int ntokens;

	new_table = get_tokenized_data(buf, &ntokens);
	if (IS_ERR(new_table))
		return PTR_RET(new_table);

	schedtune_freqvar_update_table(new_table, ntokens, data->table);

	kfree(new_table);

	return count;
}
static struct governor_attr freqvar_boost = __ATTR_RW(freqvar_boost);
#endif /* CONFIG_FREQVAR_SCHEDTUNE */

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;
 	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;
 	tunables->down_rate_limit_us = rate_limit_us;
 	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}
 	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);

static struct attribute *sugov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
#ifdef CONFIG_FREQVAR_SCHEDTUNE
	&freqvar_boost.attr,
#endif
	NULL
};

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

struct cpufreq_governor schedutil_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	mutex_init(&sg_policy->work_lock);
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	mutex_destroy(&sg_policy->work_lock);
	kfree(sg_policy);
}

static int sugov_kthread_create(struct sugov_policy *sg_policy)
{
	struct task_struct *thread;
	struct sched_param param = { .sched_priority = MAX_USER_RT_PRIO / 2 };
	struct cpufreq_policy *policy = sg_policy->policy;
	int ret;

	init_kthread_work(&sg_policy->work, sugov_work);
	init_kthread_worker(&sg_policy->worker);
	thread = kthread_create(kthread_worker_fn, &sg_policy->worker,
				"sugov:%d",
				cpumask_first(policy->related_cpus));
	if (IS_ERR(thread)) {
		pr_err("failed to create sugov thread: %ld\n", PTR_ERR(thread));
		return PTR_ERR(thread);
	}

	ret = sched_setscheduler_nocheck(thread, SCHED_FIFO, &param);
	if (ret) {
		kthread_stop(thread);
		pr_warn("%s: failed to set SCHED_FIFO\n", __func__);
		return ret;
	}

	sg_policy->thread = thread;
	kthread_bind_mask(thread, policy->related_cpus);
	wake_up_process(thread);

	return 0;
}

static void sugov_kthread_stop(struct sugov_policy *sg_policy)
{
	flush_kthread_worker(&sg_policy->worker);
	kthread_stop(sg_policy->thread);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_free(struct sugov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	unsigned int lat;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy)
		return -ENOMEM;

	ret = sugov_kthread_create(sg_policy);
	if (ret)
		goto free_sg_policy;

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto stop_kthread;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_sg_policy;
	}

	tunables->up_rate_limit_us = LATENCY_MULTIPLIER;
	tunables->down_rate_limit_us = LATENCY_MULTIPLIER;
	lat = policy->cpuinfo.transition_latency / NSEC_PER_USEC;
	if (lat) {
		tunables->up_rate_limit_us *= lat;
		tunables->down_rate_limit_us *= lat;
	}

	/* init freqvar_boost */
	schedtune_freqvar_boost_init(policy, &tunables->freqvar_boost);

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedutil_gov.name);
	if (ret)
		goto fail;

 out:
	mutex_unlock(&global_tunables_lock);

	return 0;

 fail:
	policy->governor_data = NULL;
	schedtune_freqvar_boost_exit(policy, &tunables->freqvar_boost);
	sugov_tunables_free(tunables);

 stop_kthread:
	sugov_kthread_stop(sg_policy);

 free_sg_policy:
	mutex_unlock(&global_tunables_lock);

	sugov_kthread_stop(sg_policy);
	sugov_policy_free(sg_policy);
	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static int sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count) {
		schedtune_freqvar_boost_exit(policy, &tunables->freqvar_boost);
		sugov_tunables_free(tunables);
	}

	mutex_unlock(&global_tunables_lock);

	sugov_policy_free(sg_policy);
	return 0;
}

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_us(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->max_util = 0;
	sg_policy->pending = false;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->sg_policy = sg_policy;
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
						policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}
	return 0;
}

static int sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus)
		cpufreq_remove_update_util_hook(cpu);

	synchronize_sched();

	irq_work_sync(&sg_policy->irq_work);
	kthread_cancel_work_sync(&sg_policy->work);
	return 0;
}

static int sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	mutex_lock(&sg_policy->work_lock);

	if (policy->max < policy->cur)
		__cpufreq_driver_target(policy, policy->max,
					CPUFREQ_RELATION_H);
	else if (policy->min > policy->cur)
		__cpufreq_driver_target(policy, policy->min,
					CPUFREQ_RELATION_L);

	mutex_unlock(&sg_policy->work_lock);

	sg_policy->need_freq_update = true;
	return 0;
}

int sugov_governor(struct cpufreq_policy *policy, unsigned int event)
{
	if (event == CPUFREQ_GOV_POLICY_INIT) {
		return sugov_init(policy);
	} else if (policy->governor_data) {
		switch (event) {
		case CPUFREQ_GOV_POLICY_EXIT:
			return sugov_exit(policy);
		case CPUFREQ_GOV_START:
			return sugov_start(policy);
		case CPUFREQ_GOV_STOP:
			return sugov_stop(policy);
		case CPUFREQ_GOV_LIMITS:
			return sugov_limits(policy);
		}
	}
	return -EINVAL;
}

struct cpufreq_governor schedutil_gov = {
	.name = "schedutil",
	.governor = sugov_governor,
	.owner = THIS_MODULE,
};

static int __init sugov_module_init(void)
{
	return cpufreq_register_governor(&schedutil_gov);
}

static void __exit sugov_module_exit(void)
{
	cpufreq_unregister_governor(&schedutil_gov);
}

MODULE_AUTHOR("Rafael J. Wysocki <rafael.j.wysocki@intel.com>");
MODULE_DESCRIPTION("Utilization-based CPU frequency selection");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}

fs_initcall(sugov_module_init);
#else
module_init(sugov_module_init);
#endif
module_exit(sugov_module_exit);
