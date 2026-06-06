#ifndef _LINUX_ANDROID_POWER_H
#define _LINUX_ANDROID_POWER_H

#include <linux/wakelock.h>

typedef struct wake_lock android_suspend_lock_t;

static inline void android_init_suspend_lock(android_suspend_lock_t *lock)
{
    wake_lock_init(lock, WAKE_LOCK_SUSPEND, "evdev");
}

static inline void android_uninit_suspend_lock(android_suspend_lock_t *lock)
{
    wake_lock_destroy(lock);
}

static inline void android_lock_suspend(android_suspend_lock_t *lock)
{
    wake_lock(lock);
}

static inline void android_unlock_suspend(android_suspend_lock_t *lock)
{
    wake_unlock(lock);
}

static inline void android_lock_suspend_auto_expire(
        android_suspend_lock_t *lock, int timeout_ms)
{
    wake_lock_timeout(lock, msecs_to_jiffies(timeout_ms));
}

#endif /* _LINUX_ANDROID_POWER_H */
