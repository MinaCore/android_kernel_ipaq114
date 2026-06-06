#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/platform_device.h>

static struct wake_lock main_wake_lock;
static struct wake_lock unknown_wakelock;
static DEFINE_SPINLOCK(locks_lock);

/* request_state: "mem" или "on" */
static ssize_t request_state_store(struct kobject *kobj,
        struct kobj_attribute *attr, const char *buf, size_t n)
{
    if (!strncmp(buf, "mem", 3))
        wake_unlock(&main_wake_lock);
    else
        wake_lock(&main_wake_lock);
    return n;
}
static struct kobj_attribute request_state_attr =
    __ATTR(request_state, 0220, NULL, request_state_store);

/* acquire_partial_wake_lock */
static ssize_t acquire_partial_store(struct kobject *kobj,
        struct kobj_attribute *attr, const char *buf, size_t n)
{
    wake_lock(&unknown_wakelock);
    return n;
}
static struct kobj_attribute acquire_partial_attr =
    __ATTR(acquire_partial_wake_lock, 0220, NULL, acquire_partial_store);

/* release_wake_lock */
static ssize_t release_wake_store(struct kobject *kobj,
        struct kobj_attribute *attr, const char *buf, size_t n)
{
    wake_unlock(&unknown_wakelock);
    return n;
}
static struct kobj_attribute release_wake_attr =
    __ATTR(release_wake_lock, 0220, NULL, release_wake_store);

/* auto_off_timeout */
static unsigned long auto_off_timeout_ms = 0;
static ssize_t auto_off_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%lu\n", auto_off_timeout_ms);
}
static ssize_t auto_off_store(struct kobject *kobj,
        struct kobj_attribute *attr, const char *buf, size_t n)
{
    sscanf(buf, "%lu", &auto_off_timeout_ms);
    return n;
}
static struct kobj_attribute auto_off_attr =
    __ATTR(auto_off_timeout, 0660, auto_off_show, auto_off_store);

/* wait_for_fb_sleep / wait_for_fb_wake — заглушки */
static ssize_t fb_sleep_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "\n");
}
static struct kobj_attribute fb_sleep_attr =
    __ATTR(wait_for_fb_sleep, 0440, fb_sleep_show, NULL);

static ssize_t fb_wake_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "\n");
}
static struct kobj_attribute fb_wake_attr =
    __ATTR(wait_for_fb_wake, 0440, fb_wake_show, NULL);

static struct attribute *android_power_attrs[] = {
    &request_state_attr.attr,
    &acquire_partial_attr.attr,
    &release_wake_attr.attr,
    &auto_off_attr.attr,
    &fb_sleep_attr.attr,
    &fb_wake_attr.attr,
    NULL,
};
static struct attribute_group android_power_attr_group = {
    .attrs = android_power_attrs,
};

static struct kobject *android_power_kobj;

static int __init android_power_init(void)
{
    int ret;

    wake_lock_init(&main_wake_lock, WAKE_LOCK_SUSPEND, "main");
    wake_lock_init(&unknown_wakelock, WAKE_LOCK_SUSPEND, "unknown-wakelock");
    wake_lock(&main_wake_lock);

    android_power_kobj = kobject_create_and_add("android_power", NULL);
    if (!android_power_kobj)
        return -ENOMEM;

    ret = sysfs_create_group(android_power_kobj, &android_power_attr_group);
    if (ret)
        kobject_put(android_power_kobj);

    return ret;
}

static void __exit android_power_exit(void)
{
    sysfs_remove_group(android_power_kobj, &android_power_attr_group);
    kobject_put(android_power_kobj);
    wake_lock_destroy(&main_wake_lock);
    wake_lock_destroy(&unknown_wakelock);
}

module_init(android_power_init);
module_exit(android_power_exit);
MODULE_LICENSE("GPL");
