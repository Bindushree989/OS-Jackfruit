#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/signal.h>
#include <linux/jiffies.h>
#include <linux/timex.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("student");
MODULE_DESCRIPTION("Container memory monitor");

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 2

struct centry {
    struct list_head list;
    char   id[64];
    int    pid;
    long   soft_kb;
    long   hard_kb;
    int    soft_warned;
};

static LIST_HEAD(clist);
static DEFINE_MUTEX(clist_lock);

static dev_t           devno;
static struct cdev     cdev;
static struct class   *cls;
static struct timer_list check_timer;

static long get_rss_kb(int pid) {
    struct task_struct *task;
    long rss = 0;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task && task->mm)
        rss = get_mm_rss(task->mm) * (PAGE_SIZE / 1024);
    rcu_read_unlock();
    return rss;
}

static void kill_pid_signal(int pid, int sig) {
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task) send_sig(sig, task, 1);
    rcu_read_unlock();
}

static void check_all(struct timer_list *t) {
    struct centry *e, *tmp;
    mutex_lock(&clist_lock);
    list_for_each_entry_safe(e, tmp, &clist, list) {
        long rss = get_rss_kb(e->pid);
        if (rss == 0) {
            pr_info("container_monitor: removing stale entry pid=%d (%s)\n",
                    e->pid, e->id);
            list_del(&e->list);
            kfree(e);
            continue;
        }
        if (rss > e->hard_kb) {
            pr_warn("container_monitor: HARD LIMIT pid=%d (%s) rss=%ldKB hard=%ldKB killing\n",
                    e->pid, e->id, rss, e->hard_kb);
            kill_pid_signal(e->pid, SIGKILL);
        } else if (rss > e->soft_kb && !e->soft_warned) {
            pr_warn("container_monitor: SOFT LIMIT pid=%d (%s) rss=%ldKB soft=%ldKB warning\n",
                    e->pid, e->id, rss, e->soft_kb);
            e->soft_warned = 1;
        }
    }
    mutex_unlock(&clist_lock);
    mod_timer(&check_timer,
              jiffies + msecs_to_jiffies(CHECK_INTERVAL_SEC * 1000));
}

static long mon_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
    case MONITOR_REGISTER: {
        struct container_reg reg;
        if (copy_from_user(&reg, (void __user *)arg, sizeof(reg)))
            return -EFAULT;
        struct centry *e = kzalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;
        strncpy(e->id, reg.id, 63);
        e->pid     = reg.pid;
        e->soft_kb = (long)reg.soft_mib * 1024;
        e->hard_kb = (long)reg.hard_mib * 1024;
        mutex_lock(&clist_lock);
        list_add_tail(&e->list, &clist);
        mutex_unlock(&clist_lock);
        pr_info("container_monitor: registered pid=%d (%s) soft=%dMiB hard=%dMiB\n",
                reg.pid, reg.id, reg.soft_mib, reg.hard_mib);
        return 0;
    }
    case MONITOR_UNREGISTER: {
        struct container_unreg unreg;
        if (copy_from_user(&unreg, (void __user *)arg, sizeof(unreg)))
            return -EFAULT;
        struct centry *e, *tmp;
        mutex_lock(&clist_lock);
        list_for_each_entry_safe(e, tmp, &clist, list) {
            if (e->pid == unreg.pid) {
                list_del(&e->list);
                kfree(e);
                pr_info("container_monitor: unregistered pid=%d\n", unreg.pid);
                break;
            }
        }
        mutex_unlock(&clist_lock);
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = mon_ioctl,
};

static int __init mon_init(void) {
    alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    cdev_init(&cdev, &mon_fops);
    cdev_add(&cdev, devno, 1);
    cls = class_create(DEVICE_NAME);
    device_create(cls, NULL, devno, NULL, DEVICE_NAME);
    timer_setup(&check_timer, check_all, 0);
    mod_timer(&check_timer,
              jiffies + msecs_to_jiffies(CHECK_INTERVAL_SEC * 1000));
    pr_info("container_monitor: loaded /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit mon_exit(void) {
    timer_shutdown_sync(&check_timer);
    struct centry *e, *tmp;
    mutex_lock(&clist_lock);
    list_for_each_entry_safe(e, tmp, &clist, list) {
        list_del(&e->list);
        kfree(e);
    }
    mutex_unlock(&clist_lock);
    device_destroy(cls, devno);
    class_destroy(cls);
    cdev_del(&cdev);
    unregister_chrdev_region(devno, 1);
    pr_info("container_monitor: unloaded\n");
}

module_init(mon_init);
module_exit(mon_exit);
