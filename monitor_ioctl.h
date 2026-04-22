#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

struct container_reg {
    int  pid;
    int  soft_mib;
    int  hard_mib;
    char id[64];
};

struct container_unreg {
    int pid;
};

#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct container_reg)
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct container_unreg)

#endif
