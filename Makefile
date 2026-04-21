CC      = gcc
CFLAGS  = -Wall -Wextra -g
KDIR   ?= /lib/modules/$(shell uname -r)/build

USER_BINS = engine cpu_workload mem_workload io_workload

all: $(USER_BINS) monitor.ko

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ engine.c -lpthread

cpu_workload: cpu_workload.c
	$(CC) $(CFLAGS) -o $@ cpu_workload.c

mem_workload: mem_workload.c
	$(CC) $(CFLAGS) -o $@ mem_workload.c

io_workload: io_workload.c
	$(CC) $(CFLAGS) -o $@ io_workload.c

obj-m := monitor.o
monitor.ko: monitor.c monitor_ioctl.h
	cp monitor_ioctl.h /tmp/monitor_ioctl.h 2>/dev/null || true
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ci: engine cpu_workload mem_workload io_workload

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true
	rm -f $(USER_BINS) *.o *.mod *.mod.c *.symvers *.order .*.cmd
	rm -rf .tmp_versions

.PHONY: all ci clean
