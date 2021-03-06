/*
 * Intelli Hotplug Driver
 *
 * Copyright (c) 2013-2014, Paul Reioux <reioux@gmail.com>
 * Copyright (c) 2010-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>

#include <linux/kobject.h>
#include <linux/cpufreq.h>

#define INTELLI_PLUG			"intelli_plug"
#define INTELLI_PLUG_MAJOR_VERSION	5
#define INTELLI_PLUG_MINOR_VERSION	1

#define DEF_SAMPLING_MS			600
#define RESUME_SAMPLING_MS		HZ / 10
#define START_DELAY_MS			HZ * 20
#define DEFAULT_MIN_CPUS_ONLINE		1
#define DEFAULT_MAX_CPUS_ONLINE		NR_CPUS
#define DEFAULT_NR_FSHIFT		DEFAULT_MAX_CPUS_ONLINE - 1
#define DEFAULT_DOWN_LOCK_DUR		4000
#define DEFAULT_MAX_CPUS_ONLINE_SUSP	1

#define CAPACITY_RESERVE		50
#if defined(CONFIG_ARCH_APQ8084) || defined(CONFIG_ARM64)
#define THREAD_CAPACITY (430 - CAPACITY_RESERVE)
#elif defined(CONFIG_ARCH_MSM8960) || defined(CONFIG_ARCH_APQ8064) || \
defined(CONFIG_ARCH_MSM8974)
#define THREAD_CAPACITY			(339 - CAPACITY_RESERVE)
#elif defined(CONFIG_ARCH_MSM8226) || defined (CONFIG_ARCH_MSM8926) || \
defined (CONFIG_ARCH_MSM8610) || defined (CONFIG_ARCH_MSM8228)
#define THREAD_CAPACITY			(190 - CAPACITY_RESERVE)
#else
#define THREAD_CAPACITY			(250 - CAPACITY_RESERVE)
#endif
#define CPU_NR_THRESHOLD		((THREAD_CAPACITY << 1) + (THREAD_CAPACITY / 2))
#define MULT_FACTOR			4
#define DIV_FACTOR			100000



static struct delayed_work intelli_plug_work;
static struct work_struct up_down_work;
static struct workqueue_struct *intelliplug_wq;
static struct mutex intelli_plug_mutex;

struct ip_cpu_info {
	unsigned long cpu_nr_running;
};
static DEFINE_PER_CPU(struct ip_cpu_info, ip_info);

/* HotPlug Driver controls */
static atomic_t intelli_plug_active = ATOMIC_INIT(0);

static unsigned int min_cpus_online = DEFAULT_MIN_CPUS_ONLINE;
static unsigned int max_cpus_online = DEFAULT_MAX_CPUS_ONLINE;
static unsigned int full_mode_profile = 0;
static unsigned int cpu_nr_run_threshold = CPU_NR_THRESHOLD;




/* HotPlug Driver Tuning */
static unsigned int target_cpus = 0;
static unsigned int def_sampling_ms = DEF_SAMPLING_MS;
static unsigned int nr_fshift = DEFAULT_NR_FSHIFT;
static unsigned int nr_run_hysteresis = DEFAULT_MAX_CPUS_ONLINE * 2;
static unsigned int debug_intelli_plug = 0;

#define dprintk(msg...)		\
do { 				\
	if (debug_intelli_plug)		\
		pr_info(msg);	\
} while (0)

static unsigned int nr_run_thresholds_balance[] = {
	(THREAD_CAPACITY * 625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 1125 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int nr_run_thresholds_performance[] = {
	(THREAD_CAPACITY * 380 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int nr_run_thresholds_conservative[] = {
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 1625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 2125 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int nr_run_thresholds_disable[] = {
	0,  0,  0,  UINT_MAX
};

static unsigned int nr_run_thresholds_tri[] = {
	(THREAD_CAPACITY * 625 * MULT_FACTOR) / DIV_FACTOR,
	(THREAD_CAPACITY * 875 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int nr_run_thresholds_eco[] = {
        (THREAD_CAPACITY * 380 * MULT_FACTOR) / DIV_FACTOR,
	UINT_MAX
};

static unsigned int nr_run_thresholds_strict[] = {
	UINT_MAX
};

static unsigned int *nr_run_profiles[] = {
	nr_run_thresholds_balance,
	nr_run_thresholds_performance,
	nr_run_thresholds_conservative,
	nr_run_thresholds_disable,
	nr_run_thresholds_tri,
	nr_run_thresholds_eco,
	nr_run_thresholds_strict
	};

static unsigned int nr_run_last;
static unsigned int down_lock_dur = DEFAULT_DOWN_LOCK_DUR;

struct down_lock {
	unsigned int locked;
	struct delayed_work lock_rem;
};
static DEFINE_PER_CPU(struct down_lock, lock_info);

static void apply_down_lock(unsigned int cpu)
{
	struct down_lock *dl = &per_cpu(lock_info, cpu);

	dl->locked = 1;
	queue_delayed_work_on(0, intelliplug_wq, &dl->lock_rem,
			      msecs_to_jiffies(down_lock_dur));
}

static void remove_down_lock(struct work_struct *work)
{
	struct down_lock *dl = container_of(work, struct down_lock,
					    lock_rem.work);
	dl->locked = 0;
}

static int check_down_lock(unsigned int cpu)
{
	struct down_lock *dl = &per_cpu(lock_info, cpu);
	return dl->locked;
}

static unsigned int calculate_thread_stats(void)
{
	unsigned int avg_nr_run = avg_nr_running();
	unsigned int nr_run;
	unsigned int threshold_size;
	unsigned int *current_profile;

	threshold_size = max_cpus_online;
	nr_run_hysteresis = max_cpus_online * 2;
	nr_fshift = max_cpus_online - 1;

	for (nr_run = 1; nr_run < threshold_size; nr_run++) {
		unsigned int nr_threshold;
		if (max_cpus_online >= 4)
			current_profile = nr_run_profiles[full_mode_profile];
		else if (max_cpus_online == 3)
			current_profile = nr_run_profiles[4];
		else if (max_cpus_online == 2)
			current_profile = nr_run_profiles[5];
		else
			current_profile = nr_run_profiles[6];

		nr_threshold = current_profile[nr_run - 1];

		if (nr_run_last <= nr_run)
			nr_threshold += nr_run_hysteresis;
		if (avg_nr_run <= (nr_threshold << (FSHIFT - nr_fshift)))
			break;
	}
	nr_run_last = nr_run;

	return nr_run;
}

static void update_per_cpu_stat(void)
{
	unsigned int cpu;
	struct ip_cpu_info *l_ip_info;

	for_each_online_cpu(cpu) {
		l_ip_info = &per_cpu(ip_info, cpu);
		l_ip_info->cpu_nr_running = avg_cpu_nr_running(cpu);
	}
}

static void __ref cpu_up_down_work(struct work_struct *work)
{
	int online_cpus, cpu, l_nr_threshold;
	int target = target_cpus;
	struct ip_cpu_info *l_ip_info;

	if (target < min_cpus_online)
		target = min_cpus_online;
	else if (target > max_cpus_online)
		target = max_cpus_online;

	online_cpus = num_online_cpus();

	if (target < online_cpus) {

		update_per_cpu_stat();
		for_each_online_cpu(cpu) {
			if (cpu == 0)
				continue;
			if (check_down_lock(cpu))
				break;
			l_nr_threshold =
				cpu_nr_run_threshold << 1 / (num_online_cpus());
			l_ip_info = &per_cpu(ip_info, cpu);
			if (l_ip_info->cpu_nr_running < l_nr_threshold)
				cpu_down(cpu);
			if (target >= num_online_cpus())
				break;
		}
	} else if (target > online_cpus) {
		for_each_cpu_not(cpu, cpu_online_mask) {
			if (cpu == 0)
				continue;
			cpu_up(cpu);
			apply_down_lock(cpu);
			if (target <= num_online_cpus())
				break;
		}
	}
}

static void intelli_plug_work_fn(struct work_struct *work)
{

	target_cpus = calculate_thread_stats();
	queue_work_on(0, intelliplug_wq, &up_down_work);

	if (atomic_read(&intelli_plug_active) == 1)
		queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_work,
					msecs_to_jiffies(def_sampling_ms));
}

static int __ref intelli_plug_start(void)
{
	int cpu, ret = 0;
	struct down_lock *dl;

	intelliplug_wq = create_freezable_workqueue("intelliplug");	
	if (!intelliplug_wq) {
		pr_err("%s: Failed to allocate hotplug workqueue\n",
		       INTELLI_PLUG);
		ret = -ENOMEM;
		goto err_out;
	}



	mutex_init(&intelli_plug_mutex);

	INIT_WORK(&up_down_work, cpu_up_down_work);
	INIT_DELAYED_WORK(&intelli_plug_work, intelli_plug_work_fn);
	for_each_possible_cpu(cpu) {
		dl = &per_cpu(lock_info, cpu);
		INIT_DELAYED_WORK(&dl->lock_rem, remove_down_lock);
	}

	/* Fire up all CPUs */
	for_each_cpu_not(cpu, cpu_online_mask) {
		if (cpu == 0)
			continue;
		cpu_up(cpu);
		apply_down_lock(cpu);
	}

	queue_delayed_work_on(0, intelliplug_wq, &intelli_plug_work,
			      START_DELAY_MS);

	return ret;

err_out:
	atomic_set(&intelli_plug_active, 0);
	return ret;
}

static void intelli_plug_stop(void)
{
	int cpu;
	struct down_lock *dl;

	for_each_possible_cpu(cpu) {
		dl = &per_cpu(lock_info, cpu);
		cancel_delayed_work_sync(&dl->lock_rem);
	}
	flush_workqueue(intelliplug_wq);
	cancel_work_sync(&up_down_work);
	cancel_delayed_work_sync(&intelli_plug_work);
	mutex_destroy(&intelli_plug_mutex);
	destroy_workqueue(intelliplug_wq);
}

static void intelli_plug_active_eval_fn(unsigned int status)
{
	int ret = 0;

	if (status == 1) {
		ret = intelli_plug_start();
		if (ret)
			status = 0;
	} else
		intelli_plug_stop();

	atomic_set(&intelli_plug_active, status);
}

#define show_one(file_name, object)				\
static ssize_t show_##file_name					\
(struct kobject *kobj, struct kobj_attribute *attr, char *buf)	\
{								\
	return sprintf(buf, "%u\n", object);			\
}


show_one(min_cpus_online, min_cpus_online);
show_one(max_cpus_online, max_cpus_online);
show_one(full_mode_profile, full_mode_profile);
show_one(cpu_nr_run_threshold, cpu_nr_run_threshold);
show_one(def_sampling_ms, def_sampling_ms);
show_one(debug_intelli_plug, debug_intelli_plug);
show_one(nr_fshift, nr_fshift);
show_one(nr_run_hysteresis, nr_run_hysteresis);
show_one(down_lock_duration, down_lock_dur);

#define store_one(file_name, object)		\
static ssize_t store_##file_name		\
(struct kobject *kobj, 				\
 struct kobj_attribute *attr, 			\
 const char *buf, size_t count)			\
{						\
	unsigned int input;			\
	int ret;				\
	ret = sscanf(buf, "%u", &input);	\
	if (ret != 1 || input > 100)		\
		return -EINVAL;			\
	if (input == object) {			\
		return count;			\
	}					\
	object = input;				\
	return count;				\
}


store_one(full_mode_profile, full_mode_profile);
store_one(cpu_nr_run_threshold, cpu_nr_run_threshold);
store_one(def_sampling_ms, def_sampling_ms);
store_one(debug_intelli_plug, debug_intelli_plug);
store_one(nr_fshift, nr_fshift);
store_one(nr_run_hysteresis, nr_run_hysteresis);
store_one(down_lock_duration, down_lock_dur);

static ssize_t show_intelli_plug_active(struct kobject *kobj,
					struct kobj_attribute *attr, 
					char *buf)
{
	return sprintf(buf, "%d\n",
			atomic_read(&intelli_plug_active));
}

static ssize_t store_intelli_plug_active(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	int ret;
	unsigned int input;

	ret = sscanf(buf, "%d", &input);
	if (ret < 0)
		return ret;

	if (input < 0)
		input = 0;
	else if (input > 0)
		input = 1;

	if (input == atomic_read(&intelli_plug_active))
		return count;

	intelli_plug_active_eval_fn(input);

	return count;
}




static ssize_t store_min_cpus_online(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > NR_CPUS)
		return -EINVAL;

	if (max_cpus_online < val)
		max_cpus_online = val;

	min_cpus_online = val;

	return count;
}

static ssize_t store_max_cpus_online(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = sscanf(buf, "%u", &val);
	if (ret != 1 || val < 1 || val > NR_CPUS)
		return -EINVAL;

	if (min_cpus_online > val)
		min_cpus_online = val;

	max_cpus_online = val;

	return count;
}


#define KERNEL_ATTR_RW(_name) \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0664, show_##_name, store_##_name)

KERNEL_ATTR_RW(intelli_plug_active);

KERNEL_ATTR_RW(min_cpus_online);
KERNEL_ATTR_RW(max_cpus_online);
KERNEL_ATTR_RW(full_mode_profile);
KERNEL_ATTR_RW(cpu_nr_run_threshold);

KERNEL_ATTR_RW(def_sampling_ms);
KERNEL_ATTR_RW(debug_intelli_plug);
KERNEL_ATTR_RW(nr_fshift);
KERNEL_ATTR_RW(nr_run_hysteresis);
KERNEL_ATTR_RW(down_lock_duration);

static struct attribute *intelli_plug_attrs[] = {
	&intelli_plug_active_attr.attr,
	&min_cpus_online_attr.attr,
	&max_cpus_online_attr.attr,
	&full_mode_profile_attr.attr,
	&cpu_nr_run_threshold_attr.attr,
	&def_sampling_ms_attr.attr,
	&debug_intelli_plug_attr.attr,
	&nr_fshift_attr.attr,
	&nr_run_hysteresis_attr.attr,
	&down_lock_duration_attr.attr,
	NULL,
};

static struct attribute_group intelli_plug_attr_group = {
	.attrs = intelli_plug_attrs,
	.name = "intelli_plug",
};

static int __init intelli_plug_init(void)
{
	int rc;

	rc = sysfs_create_group(kernel_kobj, &intelli_plug_attr_group);

	pr_info("intelli_plug: version %d.%d\n",
		 INTELLI_PLUG_MAJOR_VERSION,
		 INTELLI_PLUG_MINOR_VERSION);

	if (atomic_read(&intelli_plug_active) == 1)
		intelli_plug_start();

	return 0;
}

static void __exit intelli_plug_exit(void)
{

	if (atomic_read(&intelli_plug_active) == 1)
		intelli_plug_stop();
	sysfs_remove_group(kernel_kobj, &intelli_plug_attr_group);
}

late_initcall(intelli_plug_init);
module_exit(intelli_plug_exit);

MODULE_AUTHOR("Paul Reioux <reioux@gmail.com>");
MODULE_DESCRIPTION("'intell_plug' - An intelligent cpu hotplug driver for "
	"Low Latency Frequency Transition capable processors");
MODULE_LICENSE("GPLv2");
