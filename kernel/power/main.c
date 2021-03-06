/*
 * kernel/power/main.c - PM subsystem core functionality.
 *
 * Copyright (c) 2003 Patrick Mochel
 * Copyright (c) 2003 Open Source Development Lab
 * 
 * This file is released under the GPLv2
 *
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/resume-trace.h>
#include <linux/workqueue.h>

#include <linux/moduleparam.h>

#include "power.h"

#ifdef CONFIG_DVFS_LIMIT
#include <linux/cpufreq.h>
#include <linux/mfd/dbx500-prcmu.h>
#endif /* CONFIG_DVFS_LIMIT */

DEFINE_MUTEX(pm_mutex);

static bool debug_mask = false;
module_param(debug_mask, bool, 0644);

#ifdef CONFIG_PM_SLEEP

/* Routines for PM-transition notifications */

static BLOCKING_NOTIFIER_HEAD(pm_chain_head);

int register_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(register_pm_notifier);

int unregister_pm_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&pm_chain_head, nb);
}
EXPORT_SYMBOL_GPL(unregister_pm_notifier);

int pm_notifier_call_chain(unsigned long val)
{
	return (blocking_notifier_call_chain(&pm_chain_head, val, NULL)
			== NOTIFY_BAD) ? -EINVAL : 0;
}

/* If set, devices may be suspended and resumed asynchronously. */
int pm_async_enabled = 1;

static ssize_t pm_async_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_async_enabled);
}

static ssize_t pm_async_store(struct kobject *kobj, struct kobj_attribute *attr,
			      const char *buf, size_t n)
{
	unsigned long val;

	if (strict_strtoul(buf, 10, &val))
		return -EINVAL;

	if (val > 1)
		return -EINVAL;

	pm_async_enabled = val;
	return n;
}

power_attr(pm_async);

#ifdef CONFIG_PM_DEBUG
int pm_test_level = TEST_NONE;

static const char * const pm_tests[__TEST_AFTER_LAST] = {
	[TEST_NONE] = "none",
	[TEST_CORE] = "core",
	[TEST_CPUS] = "processors",
	[TEST_PLATFORM] = "platform",
	[TEST_DEVICES] = "devices",
	[TEST_FREEZER] = "freezer",
};

static ssize_t pm_test_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	char *s = buf;
	int level;

	for (level = TEST_FIRST; level <= TEST_MAX; level++)
		if (pm_tests[level]) {
			if (level == pm_test_level)
				s += sprintf(s, "[%s] ", pm_tests[level]);
			else
				s += sprintf(s, "%s ", pm_tests[level]);
		}

	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';

	return (s - buf);
}

static ssize_t pm_test_store(struct kobject *kobj, struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	const char * const *s;
	int level;
	char *p;
	int len;
	int error = -EINVAL;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	mutex_lock(&pm_mutex);

	level = TEST_FIRST;
	for (s = &pm_tests[level]; level <= TEST_MAX; s++, level++)
		if (*s && len == strlen(*s) && !strncmp(buf, *s, len)) {
			pm_test_level = level;
			error = 0;
			break;
		}

	mutex_unlock(&pm_mutex);

	return error ? error : n;
}

power_attr(pm_test);
#endif /* CONFIG_PM_DEBUG */

#endif /* CONFIG_PM_SLEEP */

struct kobject *power_kobj;

/**
 *	state - control system power state.
 *
 *	show() returns what states are supported, which is hard-coded to
 *	'standby' (Power-On Suspend), 'mem' (Suspend-to-RAM), and
 *	'disk' (Suspend-to-Disk).
 *
 *	store() accepts one of those strings, translates it into the 
 *	proper enumerated value, and initiates a suspend transition.
 */
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	char *s = buf;
#ifdef CONFIG_SUSPEND
	int i;

	for (i = 0; i < PM_SUSPEND_MAX; i++) {
		if (pm_states[i] && valid_state(i))
			s += sprintf(s,"%s ", pm_states[i]);
	}
#endif
#ifdef CONFIG_HIBERNATION
	s += sprintf(s, "%s\n", "disk");
#else
	if (s != buf)
		/* convert the last space to a newline */
		*(s-1) = '\n';
#endif
	return (s - buf);
}

static ssize_t state_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t n)
{
#ifdef CONFIG_SUSPEND
#ifdef CONFIG_EARLYSUSPEND
	suspend_state_t state = PM_SUSPEND_ON;
#else
	suspend_state_t state = PM_SUSPEND_STANDBY;
#endif
	const char * const *s;
#endif
	char *p;
	int len;
	int error = -EINVAL;

	p = memchr(buf, '\n', n);
	len = p ? p - buf : n;

	/* First, check if we are requested to hibernate */
	if (len == 4 && !strncmp(buf, "disk", len)) {
		error = hibernate();
  goto Exit;
	}

#ifdef CONFIG_SUSPEND
	for (s = &pm_states[state]; state < PM_SUSPEND_MAX; s++, state++) {
		if (*s && len == strlen(*s) && !strncmp(buf, *s, len))
			break;
	}
	if (state < PM_SUSPEND_MAX && *s)
#ifdef CONFIG_EARLYSUSPEND
		if (state == PM_SUSPEND_ON || valid_state(state)) {
			error = 0;
			request_suspend_state(state);
		}
#else
		error = enter_state(state);
#endif
#endif

 Exit:
	return error ? error : n;
}

power_attr(state);

#ifdef CONFIG_PM_SLEEP
/*
 * The 'wakeup_count' attribute, along with the functions defined in
 * drivers/base/power/wakeup.c, provides a means by which wakeup events can be
 * handled in a non-racy way.
 *
 * If a wakeup event occurs when the system is in a sleep state, it simply is
 * woken up.  In turn, if an event that would wake the system up from a sleep
 * state occurs when it is undergoing a transition to that sleep state, the
 * transition should be aborted.  Moreover, if such an event occurs when the
 * system is in the working state, an attempt to start a transition to the
 * given sleep state should fail during certain period after the detection of
 * the event.  Using the 'state' attribute alone is not sufficient to satisfy
 * these requirements, because a wakeup event may occur exactly when 'state'
 * is being written to and may be delivered to user space right before it is
 * frozen, so the event will remain only partially processed until the system is
 * woken up by another event.  In particular, it won't cause the transition to
 * a sleep state to be aborted.
 *
 * This difficulty may be overcome if user space uses 'wakeup_count' before
 * writing to 'state'.  It first should read from 'wakeup_count' and store
 * the read value.  Then, after carrying out its own preparations for the system
 * transition to a sleep state, it should write the stored value to
 * 'wakeup_count'.  If that fails, at least one wakeup event has occurred since
 * 'wakeup_count' was read and 'state' should not be written to.  Otherwise, it
 * is allowed to write to 'state', but the transition will be aborted if there
 * are any wakeup events detected after 'wakeup_count' was written to.
 */

static ssize_t wakeup_count_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	unsigned int val;

	return pm_get_wakeup_count(&val) ? sprintf(buf, "%u\n", val) : -EINTR;
}

static ssize_t wakeup_count_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	unsigned int val;

	if (sscanf(buf, "%u", &val) == 1) {
		if (pm_save_wakeup_count(val))
			return n;
	}
	return -EINVAL;
}

power_attr(wakeup_count);
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM_TRACE
int pm_trace_enabled;

static ssize_t pm_trace_show(struct kobject *kobj, struct kobj_attribute *attr,
			     char *buf)
{
	return sprintf(buf, "%d\n", pm_trace_enabled);
}

static ssize_t
pm_trace_store(struct kobject *kobj, struct kobj_attribute *attr,
	       const char *buf, size_t n)
{
	int val;

	if (sscanf(buf, "%d", &val) == 1) {
		pm_trace_enabled = !!val;
		return n;
	}
	return -EINVAL;
}

power_attr(pm_trace);

static ssize_t pm_trace_dev_match_show(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       char *buf)
{
	return show_trace_dev_match(buf, PAGE_SIZE);
}

static ssize_t
pm_trace_dev_match_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t n)
{
	return -EINVAL;
}

power_attr(pm_trace_dev_match);

#endif /* CONFIG_PM_TRACE */

#ifdef CONFIG_USER_WAKELOCK
power_attr(wake_lock);
power_attr(wake_unlock);
#endif

#ifdef CONFIG_DVFS_LIMIT
static int cpufreq_max_limit_val = -1;
static int cpufreq_min_limit_val = -1;
static int min_replacement = 0;

static ssize_t cpufreq_table_show(struct kobject *kobj,
				struct kobj_attribute *attr,
				char *buf)
{
	ssize_t count = 0;
	struct cpufreq_frequency_table *table;
	struct cpufreq_policy *policy;
	unsigned int min_freq = ~0;
	unsigned int max_freq = 0;
	int i = 0;
	unsigned int table_length = 0;

	table = cpufreq_frequency_get_table(0);
	if (!table) {
		printk(KERN_ERR "%s: Failed to get the cpufreq table\n",
			__func__);
		return sprintf(buf, "Failed to get the cpufreq table\n");
	}

	policy = cpufreq_cpu_get(0);
	if (policy) {
	#if 0 /* /sys/devices/system/cpu/cpu0/cpufreq/scaling_min&max_freq */
		min_freq = policy->min_freq;
		max_freq = policy->max_freq;
	#else /* /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min&max_freq */
		min_freq = policy->cpuinfo.min_freq;
		max_freq = policy->cpuinfo.max_freq;
	#endif
	}

	// Get frequency table length
	for(table_length = 0; (table[table_length].frequency != CPUFREQ_TABLE_END); table_length++) ;

	for (i = table_length-1; i >= 0; i--) {
		if ((table[i].frequency == CPUFREQ_ENTRY_INVALID) ||
		    (table[i].frequency > max_freq) ||
		    (table[i].frequency < min_freq))
			continue;
		count += sprintf(&buf[count], "%d ", table[i].frequency);
	}
	count += sprintf(&buf[count], "\n");

	return count;
}

static ssize_t cpufreq_table_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t n)
{
	printk(KERN_ERR "%s: cpufreq_table is read-only\n", __func__);
	return -EINVAL;
}

#define VALID_LEVEL 1
enum dvfs_lock_request_type {
	DVFS_MIN_LOCK_REQ = 0,
	DVFS_MAX_LOCK_REQ,
};

static int dvfs_cpufreq_notifier(struct notifier_block *nb,
				 unsigned long event, void *data)
{
	struct cpufreq_policy *policy = data;
	struct cpufreq_frequency_table *table;
	unsigned int table_length = 0;

	if (event != CPUFREQ_ADJUST)
		return NOTIFY_DONE;

	table = cpufreq_frequency_get_table(0);
	if (!table) {
		printk(KERN_ERR "%s: Failed to get the cpufreq table\n",
			__func__);
		return -EINVAL;
	}

	/* Get frequency table length */
	for(table_length = 0; (table[table_length].frequency != CPUFREQ_TABLE_END); table_length++) ;

	/* Update cpufreq policy max value */
	if (cpufreq_max_limit_val != -1 && policy->max > cpufreq_max_limit_val) {
		policy->max = cpufreq_max_limit_val;
	} else if (cpufreq_max_limit_val == -1) {
		policy->max = table[table_length-1].frequency;
	}

	return NOTIFY_DONE;
}

static struct notifier_block dvfs_cpufreq_notifier_block = {
	.notifier_call = dvfs_cpufreq_notifier,
};

static int get_cpufreq_level(unsigned int freq, unsigned int *level, int req_type)
{
	struct cpufreq_frequency_table *table;
	int i = 0;
	unsigned int table_length = 0;

	table = cpufreq_frequency_get_table(0);
	if (!table) {
		printk(KERN_ERR "%s: Failed to get the cpufreq table\n",
			__func__);
		return -EINVAL;
	}

	// Get frequency table length
	for(table_length = 0; (table[table_length].frequency != CPUFREQ_TABLE_END); table_length++) ;
	
	switch (req_type) {
	case 	DVFS_MIN_LOCK_REQ:
		for (i = 0; (table[i].frequency != CPUFREQ_TABLE_END); i++)
			if (table[i].frequency >= freq) {
				*level = table[i].frequency;
				if (debug_mask) {
					pr_info("%s: MIN_LOCK req_freq(%d), matched_freq(%d)\n", __func__, freq, table[i].frequency);
				}
				return VALID_LEVEL;
			}
		break;

	case DVFS_MAX_LOCK_REQ:
		for (i = table_length-1; i >= 0; i--)
			if (table[i].frequency <= freq) {
				*level = table[i].frequency;
				if (debug_mask) {
					pr_info("%s: MAX_LOCK req_freq(%d), matched_freq(%d)\n", __func__, freq, table[i].frequency);
				}
				return VALID_LEVEL;
			}
		break;
	};

	return -EINVAL;
}

static ssize_t cpufreq_max_limit_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", cpufreq_max_limit_val);
}

static ssize_t cpufreq_max_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int val;
	unsigned int cpufreq_level;
	ssize_t ret = -EINVAL;
	int cpu;

	if (sscanf(buf, "%d", &val) != 1) {
		printk(KERN_ERR "%s: Invalid cpufreq format\n", __func__);
		goto out;
	}

	if (val == -1) { /* Unlock request */
		if (cpufreq_max_limit_val != -1) {
			/* Reset lock value to default */
 			cpufreq_max_limit_val = -1;

			/* Update CPU frequency policy */
			for_each_online_cpu(cpu)
				cpufreq_update_policy(cpu);

			/* Update PRCMU QOS value to min value */
			if(min_replacement && cpufreq_min_limit_val != -1) {
				prcmu_qos_update_requirement(PRCMU_QOS_ARM_KHZ,
						"power", cpufreq_min_limit_val);
				/* Clear replacement flag */
				min_replacement = 0;
			}
		} else /* Already unlocked */
			printk(KERN_ERR "%s: Unlock request is ignored\n",
				__func__);
	} else { /* Lock request */
		if (get_cpufreq_level((unsigned int)val, &cpufreq_level, DVFS_MAX_LOCK_REQ)
		    == VALID_LEVEL) {
 			cpufreq_max_limit_val = val;

			/* Max lock has higher priority than Min lock */
			if (cpufreq_min_limit_val != -1 &&
			    cpufreq_min_limit_val > cpufreq_max_limit_val) {
				if (debug_mask) {
					printk(KERN_ERR "%s: Min lock forced to %d"
					" because of Max lock\n",
					__func__, cpufreq_max_limit_val);
				}
				/* Update PRCMU QOS value to max value */
				prcmu_qos_update_requirement(PRCMU_QOS_ARM_KHZ,
						"power", cpufreq_max_limit_val);
				/* Set replacement flag */
				min_replacement = 1;
			}

			/* Update CPU frequency policy */
			for_each_online_cpu(cpu)
				cpufreq_update_policy(cpu);
		} else /* Invalid lock request --> No action */
			printk(KERN_ERR "%s: Lock request is invalid\n",
				__func__);
	}

	ret = n;
out:
	return ret;
}

static ssize_t cpufreq_min_limit_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sprintf(buf, "%d\n", cpufreq_min_limit_val);
}

static ssize_t cpufreq_min_limit_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t n)
{
	int val;
	unsigned int cpufreq_level;
	ssize_t ret = -EINVAL;
	int cpu;

	if (sscanf(buf, "%d", &val) != 1) {
		printk(KERN_ERR "%s: Invalid cpufreq format\n", __func__);
		goto out;
	}

	if (val == -1) { /* Unlock request */
		if (cpufreq_min_limit_val != -1) {
			/* Reset lock value to default */
 			cpufreq_min_limit_val = -1;

			/* Update PRCMU QOS value to default */
			prcmu_qos_update_requirement(PRCMU_QOS_ARM_KHZ,
					"power", PRCMU_QOS_DEFAULT_VALUE);

			/* Clear replacement flag */
			min_replacement = 0;
		} else /* Already unlocked */
			printk(KERN_ERR "%s: Unlock request is ignored\n",
				__func__);
	} else { /* Lock request */
		if (get_cpufreq_level((unsigned int)val, &cpufreq_level, DVFS_MIN_LOCK_REQ)
			== VALID_LEVEL) {
 			cpufreq_min_limit_val = val;

			/* Max lock has higher priority than Min lock */
			if (cpufreq_max_limit_val != -1 &&
			    cpufreq_min_limit_val > cpufreq_max_limit_val) {
				if (debug_mask) {
					printk(KERN_ERR "%s: Min lock forced to %d"
					" because of Max lock\n",
					__func__, cpufreq_max_limit_val);
				}
				/* Update PRCMU QOS value to max value */
				prcmu_qos_update_requirement(PRCMU_QOS_ARM_KHZ,
						"power", cpufreq_max_limit_val);
				/* Set replacement flag */
				min_replacement = 1;
			} else {
				/* Update PRCMU QOS value to new value */
				prcmu_qos_update_requirement(PRCMU_QOS_ARM_KHZ,
						"power", cpufreq_min_limit_val);
			}
		} else /* Invalid lock request --> No action */
			printk(KERN_ERR "%s: Lock request is invalid\n",
				__func__);
	}

	ret = n;
out:
	return ret;
}

power_attr(cpufreq_table);
power_attr(cpufreq_max_limit);
power_attr(cpufreq_min_limit);
#endif /* CONFIG_DVFS_LIMIT */

static struct attribute * g[] = {
	&state_attr.attr,
#ifdef CONFIG_PM_TRACE
	&pm_trace_attr.attr,
	&pm_trace_dev_match_attr.attr,
#endif
#ifdef CONFIG_PM_SLEEP
	&pm_async_attr.attr,
	&wakeup_count_attr.attr,
#ifdef CONFIG_PM_DEBUG
	&pm_test_attr.attr,
#endif
#ifdef CONFIG_USER_WAKELOCK
	&wake_lock_attr.attr,
	&wake_unlock_attr.attr,
#endif
#endif
#ifdef CONFIG_DVFS_LIMIT
	&cpufreq_table_attr.attr,
	&cpufreq_max_limit_attr.attr,
	&cpufreq_min_limit_attr.attr,
#endif /* CONFIG_DVFS_LIMIT */
	NULL,
};

static struct attribute_group attr_group = {
	.attrs = g,
};

#ifdef CONFIG_PM_RUNTIME
struct workqueue_struct *pm_wq;
EXPORT_SYMBOL_GPL(pm_wq);

static int __init pm_start_workqueue(void)
{
	pm_wq = alloc_workqueue("pm", WQ_FREEZABLE, 0);

	return pm_wq ? 0 : -ENOMEM;
}
#else
static inline int pm_start_workqueue(void) { return 0; }
#endif

static int __init pm_init(void)
{
	int error = pm_start_workqueue();
	if (error)
		return error;
	hibernate_image_size_init();
	hibernate_reserved_size_init();
	power_kobj = kobject_create_and_add("power", NULL);
	if (!power_kobj)
		return -ENOMEM;
#ifdef CONFIG_DVFS_LIMIT
	cpufreq_register_notifier(&dvfs_cpufreq_notifier_block,
				  CPUFREQ_POLICY_NOTIFIER);
	prcmu_qos_add_requirement(PRCMU_QOS_ARM_KHZ, "power",
				  PRCMU_QOS_DEFAULT_VALUE);
#endif
	return sysfs_create_group(power_kobj, &attr_group);
}

core_initcall(pm_init);
