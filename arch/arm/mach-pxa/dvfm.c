/*
 * DVFM Abstract Layer
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2007 Marvell International Ltd.
 * All Rights Reserved
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/sysdev.h>
#include <linux/spinlock.h>
#include <linux/notifier.h>
#include <linux/string.h>
#include <linux/kobject.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <asm/atomic.h>
#include <mach/dvfm.h>

#ifdef CONFIG_BPMD
#include <mach/bpm.h>

extern int bpm_enable_op(int index, int dev_idx);
extern int bpm_disable_op(int index, int dev_idx);
extern int bpm_enable_op_name(char *name, int dev_idx, char *sid);
extern int bpm_disable_op_name(char *name, int dev_idx, char *sid);
#endif

#define MAX_DEVNAME_LEN	32
/* This structure is used to dump device name list */
struct name_list {
	int	id;
	char	name[MAX_DEVNAME_LEN];
};

static ATOMIC_NOTIFIER_HEAD(dvfm_freq_notifier_list);

/* This list links log of dvfm operation */
struct info_head dvfm_trace_list = {
	.list = LIST_HEAD_INIT(dvfm_trace_list.list),
	.lock = RW_LOCK_UNLOCKED,
	.device = 0,
};

#ifndef CONFIG_BPMD
/* This idx is used for user debug */
static int dvfm_dev_idx;
#endif

struct dvfm_driver *dvfm_driver = NULL;
struct info_head *dvfm_op_list = NULL;

unsigned int cur_op;			/* current operating point */
unsigned int def_op;			/* default operating point */
unsigned int op_nums = 0;		/* number of operating point */

static atomic_t lp_count = ATOMIC_INIT(0);	/* number of blocking lowpower mode */

extern struct sysdev_class cpu_sysdev_class;

int dvfm_find_op(int index, struct op_info **op)
{
	struct op_info *p = NULL;

	read_lock(&dvfm_op_list->lock);
	if (list_empty(&dvfm_op_list->list)) {
		read_unlock(&dvfm_op_list->lock);
		return -ENOENT;
	}
	list_for_each_entry(p, &dvfm_op_list->list, list) {
		if (p->index == index) {
			*op = p;
			read_unlock(&dvfm_op_list->lock);
			return 0;
		}
	}
	read_unlock(&dvfm_op_list->lock);
	return -ENOENT;
}

#ifndef CONFIG_BPMD
/* Display current operating point */
static ssize_t op_show(struct sys_device *sys_dev, struct sysdev_attribute *attr,char *buf)
{
	struct op_info *op = NULL;
	int len = 0;

	if (dvfm_driver->dump) {
		if (!dvfm_find_op(cur_op, &op)) {
			len = dvfm_driver->dump(dvfm_driver->priv, op, buf);
		}
	}

	return len;
}

/* Set current operating point */
static ssize_t op_store(struct sys_device *sys_dev, struct sysdev_attribute *attr, const char *buf,
				size_t len)
{
	struct dvfm_freqs freqs;
	int new_op;

	sscanf(buf, "%u", &new_op);
	dvfm_request_op(new_op);
	return len;
}
SYSDEV_ATTR(op, 0644, op_show, op_store);

/* Dump all operating point */
static ssize_t ops_show(struct sys_device *sys_dev, struct sysdev_attribute *attr, char *buf)
{
	struct op_info *entry = NULL;
	int len = 0;
	char *p = NULL;

	if (!dvfm_driver->dump)
		return 0;
	read_lock(&dvfm_op_list->lock);
	if (!list_empty(&dvfm_op_list->list)) {
		list_for_each_entry(entry, &dvfm_op_list->list, list) {
			p = buf + len;
			len += dvfm_driver->dump(dvfm_driver->priv, entry, p);
		}
	}
	read_unlock(&dvfm_op_list->lock);

	return len;
}
SYSDEV_ATTR(ops, 0444, ops_show, NULL);

/* Dump all enabled operating point */
static ssize_t enable_op_show(struct sys_device *sys_dev, struct sysdev_attribute *attr, char *buf)
{
	struct op_info *entry = NULL;
	int len = 0;
	char *p = NULL;

	if (!dvfm_driver->dump)
		return 0;
	read_lock(&dvfm_op_list->lock);
	if (!list_empty(&dvfm_op_list->list)) {
		list_for_each_entry(entry, &dvfm_op_list->list, list) {
			if (!entry->device) {
				p = buf + len;
				len += dvfm_driver->dump(dvfm_driver->priv, entry, p);
			}
		}
	}
	read_unlock(&dvfm_op_list->lock);

	return len;
}

static ssize_t enable_op_store(struct sys_device *sys_dev, struct sysdev_attribute *attr, const char *buf,
				size_t len)
{
	int op, level;

	sscanf(buf, "%u,%u", &op, &level);
	if (level) {
		dvfm_enable_op(op, dvfm_dev_idx);
	} else
		dvfm_disable_op(op, dvfm_dev_idx);
	return len;
}
SYSDEV_ATTR(enable_op, 0644, enable_op_show, enable_op_store);

/*
 * Dump blocked device on specified OP.
 * And dump the device list that is tracked.
 */
static ssize_t trace_show(struct sys_device *sys_dev, struct sysdev_attribute *attr, char *buf)
{
	struct op_info *op_entry = NULL;
	struct dvfm_trace_info *entry = NULL;
	int len = 0, i;
	unsigned int blocked_dev;

	for (i = 0; i < op_nums; i++) {
		blocked_dev = 0;
		read_lock(&dvfm_op_list->lock);
		/* op list shouldn't be empty because op_nums is valid */
		list_for_each_entry(op_entry, &dvfm_op_list->list, list) {
			if (op_entry->index == i)
				blocked_dev = op_entry->device;
		}
		read_unlock(&dvfm_op_list->lock);
		if (!blocked_dev)
			continue;

		len += sprintf(buf + len, "Blocked devices on OP%d:", i);
		read_lock(&dvfm_trace_list.lock);
		list_for_each_entry(entry, &dvfm_trace_list.list, list) {
			if (test_bit(entry->index, (void *)&blocked_dev))
				len += sprintf(buf + len, "%s, ", entry->name);
		}
		read_unlock(&dvfm_trace_list.lock);
		len += sprintf(buf + len, "\n");
	}
	if (len == 0)
		len += sprintf(buf + len, "None device block OP\n");
	len += sprintf(buf + len, "Trace device list:\n");
	read_lock(&dvfm_trace_list.lock);
	list_for_each_entry(entry, &dvfm_trace_list.list, list) {
		len += sprintf(buf + len, "%s, ", entry->name);
	}
	read_unlock(&dvfm_trace_list.lock);
	len += sprintf(buf + len, "\n");
	return len;
}
SYSDEV_ATTR(trace, 0444, trace_show, NULL);

#ifdef CONFIG_CPU_PXA310
static ssize_t freq_show(struct sys_device *sys_dev, struct sysdev_attribute *attr, char *buf)
{
	struct op_info *op = NULL;
	int len = 0;

	if (dvfm_driver->freq_show) {
		if (!dvfm_find_op(cur_op, &op)) {
			len = dvfm_driver->freq_show(dvfm_driver->priv, op, buf);
		}
	}

	return len;
}
/* 
 * We can define a freq_store to set frequencies with a lot of parameters,
 * If a new set of frequencies is inputed by that way, it will only be treated 
 * as a non-standard op, not a new op. So the freq_store function isn't defined.
 */
SYSDEV_ATTR(frequency, 0644, freq_show, NULL);
#endif

static struct attribute *dvfm_attr[] = {
	&attr_op.attr,
	&attr_ops.attr,
	&attr_enable_op.attr,
	&attr_trace.attr,
#ifdef CONFIG_CPU_PXA310
	&attr_frequency.attr,
#endif
};
#endif

int dvfm_op_count(void)
{
	int ret = -EINVAL;

	if (dvfm_driver && dvfm_driver->count)
		ret = dvfm_driver->count(dvfm_driver->priv, dvfm_op_list);
	return ret;
}
EXPORT_SYMBOL(dvfm_op_count);

int dvfm_get_op(struct op_info **p)
{
	if (dvfm_find_op(cur_op, p))
		return -EINVAL;
	return cur_op;
}
EXPORT_SYMBOL(dvfm_get_op);

int dvfm_dump_op(int idx, char *buf)
{
        struct op_info *op = NULL;
        int len = 0;

        if (dvfm_driver && dvfm_driver->dump && !dvfm_find_op(idx, &op)) 
        	len = dvfm_driver->dump(dvfm_driver->priv, op, buf);

        return len;
}
EXPORT_SYMBOL(dvfm_dump_op);

int dvfm_get_op_freq(int idx, struct op_freq *pf)
{
        struct op_info *op = NULL;
        int ret = 0;

        if (dvfm_driver && dvfm_driver->get_freq && !dvfm_find_op(idx, &op))
                ret = dvfm_driver->get_freq(dvfm_driver->priv, op, pf);

        return ret;
}
EXPORT_SYMBOL(dvfm_get_op_freq);

int dvfm_check_active_op(int idx)
{
        struct op_info *op = NULL;
        int ret = 0;

        if (dvfm_driver && dvfm_driver->check_active_op && !dvfm_find_op(idx, &op))
                ret = dvfm_driver->check_active_op(dvfm_driver->priv, op);

        return ret;
}
EXPORT_SYMBOL(dvfm_check_active_op);

int dvfm_get_defop(void)
{
	return def_op;
}
EXPORT_SYMBOL(dvfm_get_defop);

int dvfm_get_opinfo(int index, struct op_info **p)
{
	if (dvfm_find_op(index, p))
		return -EINVAL;
	return 0;
}
EXPORT_SYMBOL(dvfm_get_opinfo);


const char* dvfm_get_op_name(int idx)
{
        struct op_info *op = NULL;

        if (dvfm_driver && dvfm_driver->name && !dvfm_find_op(idx, &op))
		return dvfm_driver->name(dvfm_driver->priv, op);

        return NULL;
}
EXPORT_SYMBOL(dvfm_get_op_name);


int dvfm_set_op(struct dvfm_freqs *freqs, unsigned int new,
		unsigned int relation)
{
	int ret = -EINVAL;

	/* check whether dvfm is enabled */
	if (!dvfm_driver || !dvfm_driver->count)
		return -EINVAL;
	if (dvfm_driver->set)
		ret = dvfm_driver->set(dvfm_driver->priv, freqs, new, relation);
	return ret;
}

/* Request operating point. System may set higher frequency because of
 * device constraint.
 */
int dvfm_request_op(int index)
{
	int ret = -EFAULT;

	/* check whether dvfm is enabled */
	if (!dvfm_driver || !dvfm_driver->count)
		return -EINVAL;
#ifdef CONFIG_BPMD
	printk(KERN_ERR "please don't use this API\n");
	WARN_ON(1);
#endif

	if (dvfm_driver->request_set)
		ret = dvfm_driver->request_set(dvfm_driver->priv, index);

	return ret;
}
EXPORT_SYMBOL(dvfm_request_op);

/*
 * Device remove the constraint on OP.
 */
int __dvfm_enable_op(int index, int dev_idx)
{
	struct op_info *p = NULL;
	int num;

	/* check whether dvfm is enabled */
	if (!dvfm_driver || !dvfm_driver->count)
		return -EINVAL;
	/* only registered device can invoke DVFM operation */
	if ((dev_idx >= DVFM_MAX_DEVICE) || dev_idx < 0)
		return -ENOENT;
	num = dvfm_driver->count(dvfm_driver->priv, dvfm_op_list);
	if (num <= index)
		return -ENOENT;
	if (!dvfm_find_op(index, &p)) {
		write_lock(&dvfm_op_list->lock);
		/* remove device ID */
		clear_bit(dev_idx, (void *)&p->device);
		write_unlock(&dvfm_op_list->lock);
#ifndef CONFIG_BPMD
		dvfm_driver->enable_op(dvfm_driver->priv, index, RELATION_LOW);
#endif

	}
	return 0;
}

/*
 * Device set constraint on OP
 */
int __dvfm_disable_op(int index, int dev_idx)
{
	struct op_info *p = NULL;
	int num;

	/* check whether dvfm is enabled */
	if (!dvfm_driver || !dvfm_driver->count)
		return -EINVAL;
	/* only registered device can invoke DVFM operation */
	if ((dev_idx >= DVFM_MAX_DEVICE) || dev_idx < 0)
		return -ENOENT;
	num = dvfm_driver->count(dvfm_driver->priv, dvfm_op_list);
	if (num <= index)
		return -ENOENT;
	if (!dvfm_find_op(index, &p)) {
		write_lock(&dvfm_op_list->lock);
		/* set device ID */
		set_bit(dev_idx, (void *)&p->device);
		write_unlock(&dvfm_op_list->lock);
		dvfm_driver->disable_op(dvfm_driver->priv, index, RELATION_LOW);
	}
	return 0;
}

int __dvfm_disable_op2(int index, int dev_idx)
{
        struct op_info *p = NULL;
        int num;

        if (!dvfm_driver || !dvfm_driver->count) {
                return -ENOENT;
        }
        num = dvfm_driver->count(dvfm_driver->priv, dvfm_op_list);
        if (num <= index)
                return -ENOENT;
        if (!dvfm_find_op(index, &p)) {
                write_lock(&dvfm_op_list->lock);
		set_bit(dev_idx, (void *)&p->device);
                write_unlock(&dvfm_op_list->lock);
        }
        return 0;
}

int dvfm_enable_op(int index, int dev_idx)
{
#ifdef CONFIG_BPMD
	bpm_enable_op(index, dev_idx);
#else
	__dvfm_enable_op(index, dev_idx);
#endif
	return 0;
}

int dvfm_disable_op(int index, int dev_idx)
{
#ifdef CONFIG_BPMD
	bpm_disable_op(index, dev_idx);
#else
        __dvfm_disable_op(index, dev_idx);
#endif
        return 0;
}

EXPORT_SYMBOL(dvfm_enable_op);
EXPORT_SYMBOL(dvfm_disable_op);

int __dvfm_enable_op_name(char *name, int dev_idx)
{
	struct op_info *p = NULL;
	int index;

	if (!dvfm_driver || !dvfm_driver->name || !name)
		return -EINVAL;
	/* only registered device can invoke DVFM operation */
	if ((dev_idx >= DVFM_MAX_DEVICE) || dev_idx < 0)
		return -ENOENT;
	list_for_each_entry(p, &dvfm_op_list->list, list) {
		if (!strcmp(dvfm_driver->name(dvfm_driver->priv, p), name)) {
			index = p->index;
			write_lock(&dvfm_op_list->lock);
			clear_bit(dev_idx, (void *)&p->device);
			write_unlock(&dvfm_op_list->lock);
			dvfm_driver->enable_op(dvfm_driver->priv,
					index, RELATION_LOW);
			break;
		}
	}
	return 0;
}

int __dvfm_disable_op_name(char *name, int dev_idx)
{
	struct op_info *p = NULL;
	int index;

	if (!dvfm_driver || !dvfm_driver->name || !name)
		return -EINVAL;
	/* only registered device can invoke DVFM operation */
	if ((dev_idx >= DVFM_MAX_DEVICE) || dev_idx < 0)
		return -ENOENT;
	list_for_each_entry(p, &dvfm_op_list->list, list) {
		if (!strcmp(dvfm_driver->name(dvfm_driver->priv, p), name)) {
			index = p->index;
			write_lock(&dvfm_op_list->lock);
			set_bit(dev_idx, (void *)&p->device);
			write_unlock(&dvfm_op_list->lock);
			dvfm_driver->disable_op(dvfm_driver->priv,
					index, RELATION_LOW);
			break;
		}
	}
	return 0;
}

/*
EXPORT_SYMBOL(dvfm_enable_op_name);
EXPORT_SYMBOL(dvfm_disable_op_name);
*/

int _dvfm_enable_op_name(char *name, int dev_idx, char *sid)
{
	int ret;
#ifdef CONFIG_BPMD
	ret = bpm_enable_op_name(name, dev_idx, sid); 
#else
	ret = __dvfm_enable_op_name(name, dev_idx);
#endif
	return ret; 
}

int _dvfm_disable_op_name(char *name, int dev_idx, char *sid)
{
	int ret;
#ifdef CONFIG_BPMD
        ret = bpm_disable_op_name(name, dev_idx, sid);
#else
        ret = __dvfm_disable_op_name(name, dev_idx);
#endif
	return ret;
}
 
EXPORT_SYMBOL(_dvfm_enable_op_name);
EXPORT_SYMBOL(_dvfm_disable_op_name);

/* Only enable those safe operating point */
int dvfm_enable(int dev_idx)
{
	printk(KERN_WARNING "dvfm_enable() is not preferred\n");
	WARN_ON(1);
	if (!dvfm_driver || !dvfm_driver->count || !dvfm_driver->enable_dvfm)
		return -ENOENT;
	return dvfm_driver->enable_dvfm(dvfm_driver->priv, dev_idx);
}

/* return whether the result is zero */
int dvfm_disable(int dev_idx)
{
	printk(KERN_WARNING "dvfm_disable() is not preferred\n");
	WARN_ON(1);
	if (!dvfm_driver || !dvfm_driver->count || !dvfm_driver->disable_dvfm)
		return -ENOENT;
	return dvfm_driver->disable_dvfm(dvfm_driver->priv, dev_idx);
}

/* return whether the result is zero */
int dvfm_enable_pm(void)
{
	return atomic_inc_and_test(&lp_count);
}

/* return whether the result is zero */
int dvfm_disable_pm(void)
{
	return atomic_dec_and_test(&lp_count);
}

int dvfm_notifier_frequency(struct dvfm_freqs *freqs, unsigned int state)
{
	int ret;

	switch (state) {
	case DVFM_FREQ_PRECHANGE:
		ret = atomic_notifier_call_chain(&dvfm_freq_notifier_list,
					DVFM_FREQ_PRECHANGE, freqs);
		if (ret != NOTIFY_DONE)
			pr_debug("Failure in device driver before "
				"switching frequency\n");
		break;
	case DVFM_FREQ_POSTCHANGE:
		ret = atomic_notifier_call_chain(&dvfm_freq_notifier_list,
					DVFM_FREQ_POSTCHANGE, freqs);
		if (ret != NOTIFY_DONE)
			pr_debug("Failure in device driver after "
				"switching frequency\n");
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}

int dvfm_register_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	switch (list) {
	case DVFM_FREQUENCY_NOTIFIER:
		ret = atomic_notifier_chain_register(
				&dvfm_freq_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL(dvfm_register_notifier);

int dvfm_unregister_notifier(struct notifier_block *nb, unsigned int list)
{
	int ret;

	switch (list) {
	case DVFM_FREQUENCY_NOTIFIER:
		ret = atomic_notifier_chain_unregister(
				&dvfm_freq_notifier_list, nb);
		break;
	default:
		ret = -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL(dvfm_unregister_notifier);

/*
 * add device into trace list
 * return device index
 */
static int add_device(char *name)
{
	struct dvfm_trace_info	*entry = NULL, *new = NULL;
	int min;

	min = find_first_zero_bit(&dvfm_trace_list.device, DVFM_MAX_DEVICE);
	if (min == DVFM_MAX_DEVICE)
		return -EINVAL;

	/* If device trace table is NULL */
	new = kzalloc(sizeof(struct dvfm_trace_info), GFP_ATOMIC);
	if (new == NULL)
		goto out_mem;
	/* add new item */
	strcpy(new->name, name);
	new->index = min;
	/* insert the new item in increasing order */
	list_for_each_entry(entry, &dvfm_trace_list.list, list) {
		if (entry->index > min) {
			list_add_tail(&(new->list), &(entry->list));
			goto inserted;
		}
	}
	list_add_tail(&(new->list), &(dvfm_trace_list.list));
inserted:
	set_bit(min, (void *)&dvfm_trace_list.device);

	return min;
out_mem:
	return -ENOMEM;
}

/*
 * Query the device number that registered in DVFM
 */
int dvfm_query_device_num(void)
{
	int count = 0;
	struct dvfm_trace_info *entry = NULL;

	read_lock(&dvfm_trace_list.lock);
	list_for_each_entry(entry, &dvfm_trace_list.list, list) {
		count++;
	}
	read_unlock(&dvfm_trace_list.lock);
	return count;
}
EXPORT_SYMBOL(dvfm_query_device_num);

/*
 * Query all device name that registered in DVFM
 */
int dvfm_query_device_list(void *mem, int len)
{
	int count = 0, size;
	struct dvfm_trace_info *entry = NULL;
	struct name_list *p = (struct name_list *)mem;

	count = dvfm_query_device_num();
	size = sizeof(struct name_list);
	if (len < count * size)
		return -ENOMEM;

	read_lock(&dvfm_trace_list.lock);
	list_for_each_entry(entry, &dvfm_trace_list.list, list) {
		p->id = entry->index;
		strcpy(p->name, entry->name);
		p++;
	}
	read_unlock(&dvfm_trace_list.lock);
	return 0;
}
EXPORT_SYMBOL(dvfm_query_device_list);

/*
 * Device driver register itself to DVFM before any operation.
 * The number of registered device is limited in 32.
 */
int dvfm_register(char *name, int *id)
{
	struct dvfm_trace_info	*p = NULL;
	int len, idx;

	if (name == NULL)
		return -EINVAL;

	/* device name is stricted in 32 bytes */
	len = strlen(name);
	if (len > DVFM_MAX_NAME)
		len = DVFM_MAX_NAME;
	write_lock(&dvfm_trace_list.lock);
	list_for_each_entry(p, &dvfm_trace_list.list, list) {
		if (!strcmp(name, p->name)) {
			/*
			 * Find device in device trace table
			 * Skip to allocate new ID
			 */
			*id = p->index;
			goto out;
		}
	}
	idx = add_device(name);
	if (idx < 0)
		goto out_num;
	*id = idx;
out:
	write_unlock(&dvfm_trace_list.lock);
	return 0;
out_num:
	write_unlock(&dvfm_trace_list.lock);
	return -EINVAL;
}
EXPORT_SYMBOL(dvfm_register);

/*
 * Release the device and free the device index.
 */
int dvfm_unregister(char *name, int *id)
{
	struct op_info *q = NULL;
	struct dvfm_trace_info	*p = NULL;
	int len, num, i;

	if (!dvfm_driver || !dvfm_driver->count || (name == NULL))
		return -EINVAL;

	/* device name is stricted in 32 bytes */
	len = strlen(name);
	if (len > DVFM_MAX_NAME)
		len = DVFM_MAX_NAME;

	num = dvfm_driver->count(dvfm_driver->priv, dvfm_op_list);

	write_lock(&dvfm_trace_list.lock);
	if (list_empty(&dvfm_trace_list.list))
		goto out;
	list_for_each_entry(p, &dvfm_trace_list.list, list) {
		if (!strncmp(name, p->name, len)) {
			for (i = 0; i < num; ++i) {
			        if (!dvfm_find_op(i, &q)) {
                			write_lock(&dvfm_op_list->lock);
                			if (test_bit(p->index, (void *)&q->device)) {
						printk(KERN_ERR "%s uses PM interface unrightly, please clean the constraint before quit!\n", name);
						dvfm_enable_op(i, p->index);
					}
			                write_unlock(&dvfm_op_list->lock);
				}
			}

			/* clear the device index */
			clear_bit(*id, (void *)&dvfm_trace_list.device);
			*id = -1;
			list_del(&p->list);
			kfree(p);
			break;
		}
	}
	write_unlock(&dvfm_trace_list.lock);
	return 0;
out:
	write_unlock(&dvfm_trace_list.lock);
	return -ENOENT;
}
EXPORT_SYMBOL(dvfm_unregister);

#ifndef CONFIG_BPMD
static int dvfm_add(struct sys_device *sys_dev)
{
	int i, n;
	int ret;

	n = ARRAY_SIZE(dvfm_attr);
	for (i = 0; i < n; i++) {
		ret = sysfs_create_file(&(sys_dev->kobj), dvfm_attr[i]);
		if (ret)
			return -EIO;
	}
	return 0;
}

static int dvfm_rm(struct sys_device *sys_dev)
{
	int i, n;
	n = ARRAY_SIZE(dvfm_attr);
	for (i = 0; i < n; i++) {
		sysfs_remove_file(&(sys_dev->kobj), dvfm_attr[i]);
	}
	return 0;
}

static int dvfm_suspend(struct sys_device *sysdev, pm_message_t pmsg)
{
	return 0;
}

static int dvfm_resume(struct sys_device *sysdev)
{
	return 0;
}

static struct sysdev_driver dvfm_sysdev_driver = {
	.add		= dvfm_add,
	.remove		= dvfm_rm,
	.suspend	= dvfm_suspend,
	.resume		= dvfm_resume,
};
#endif

int dvfm_register_driver(struct dvfm_driver *driver_data, struct info_head *op_list)
{
	int ret = 0;
	if (!driver_data || !driver_data->set)
		return -EINVAL;
	if (dvfm_driver)
		return -EBUSY;
	dvfm_driver = driver_data;

	if (!op_list)
		return -EINVAL;
	dvfm_op_list = op_list;

#ifndef CONFIG_BPMD
	/* enable_op need to invoke dvfm operation */
	dvfm_register("User", &dvfm_dev_idx);
	ret = sysdev_driver_register(&cpu_sysdev_class, &dvfm_sysdev_driver);
#endif
	return ret;
}

int dvfm_unregister_driver(struct dvfm_driver *driver)
{
#ifndef CONFIG_BPMD
	sysdev_driver_unregister(&cpu_sysdev_class, &dvfm_sysdev_driver);
	dvfm_unregister("User", &dvfm_dev_idx);
#endif
	dvfm_driver = NULL;
	return 0;
}

unsigned int NextWakeupTimeAbs;
unsigned int AppsSyncEnabled = 0;

//this function should be called form ACIPC driver when comm relenquish events occurs
int dvfm_notify_next_comm_wakeup_time(unsigned int NextWakeupTimeRel)
{
	unsigned int TimeStamp;

	TimeStamp = dvfm_driver->read_time();

	if (NextWakeupTimeRel == 0)
	{
		AppsSyncEnabled = 0;
	}
	else
	{
		AppsSyncEnabled = 1;
	}
	//we receive the next relative comm wakeup time and add to current TS to get the absolute time of the next comm wakeup.
	//this value is stored in a global variable for future use. this should be done every time the comm side goes to D2
	NextWakeupTimeAbs = NextWakeupTimeRel + TimeStamp;
	return 0;
}

//this function should be called from mspm_idle when we want to go to D2 to check when the next wakeup will occur.
int dvfm_is_comm_wakep_near(void)
{
	unsigned int TimeStamp;
	TimeStamp = dvfm_driver->read_time();

	//if the feature is not enabled we should not prevent D2.
	if (!AppsSyncEnabled)
		return 0;

	if (NextWakeupTimeAbs - TimeStamp < APPS_COMM_D2_THRESHOLD)
	{
		return (NextWakeupTimeAbs - TimeStamp);     //preventing D2
	}
	else
	{
		return 0;    //allowing D2
	}
}

MODULE_DESCRIPTION("Basic DVFM support for Monahans");
MODULE_LICENSE("GPL");
