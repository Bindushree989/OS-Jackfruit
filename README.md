# OS-Jackfruit — Multi-Container Runtime

## 1. Team Information

| Name             | SRN           |
| ---------------- | ------------- |
| Harsha Rajeswari | PES1UG24AM014 |
| BinduShree L R   | PES1UG25AM901 |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04/24.04 or Debian 13 VM with Secure Boot OFF.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build

```bash
cd boilerplate
make
```

This produces: `engine`, `memory_hog`, `cpu_hog`, `io_pulse`, and `monitor.ko`.

### Load Kernel Module

```bash
sudo insmod monitor.ko
ls -l /dev/container_monitor
dmesg | tail
```

### Prepare Root Filesystems

```bash
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

cp memory_hog ./rootfs-alpha/
cp cpu_hog    ./rootfs-alpha/
cp io_pulse   ./rootfs-alpha/
```

### Start Supervisor

```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch Containers

```bash
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96
```

### CLI Commands

```bash
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine run test ./rootfs-alpha /bin/ps
```

### Clean Up

```bash
sudo rmmod monitor
make clean
rm -rf rootfs-alpha rootfs-beta logs
```

---

## 3. Demo Screenshots

### Screenshot 1 — Supervisor Running

![Supervisor](images/s1.jpeg)

Supervisor successfully started and listening on socket. Kernel monitor connected and system ready.

---

### Screenshot 2 — Container Execution Output

![cpu hog](images/s2.jpeg)

CPU workload (`cpu_hog`) running continuously, showing increasing accumulator values and active execution inside container.

---

### Screenshot 3 — Container Start and Logs

![start logs](images/s3.jpeg)

Containers `hi` and `lo` started successfully with different priorities. Output logs confirm execution of both workloads.

---

### Screenshot 4 — Kernel Monitoring (Soft + Hard Limit)

![dmesg](images/s4.jpeg)

Kernel logs (`dmesg`) show:

* Container registration
* SOFT LIMIT warning
* HARD LIMIT enforcement
* Container removal

---

### Screenshot 5 — Hard Limit Kill Verification

![ps](images/s5.jpeg)

`engine ps` shows container state as `hard_limit_killed`, confirming correct kernel enforcement.

---

### Screenshot 6 — Stop Command Behavior

![stop](images/s6.jpeg)

Attempting to stop an already exited container results in appropriate error message.

---

### Screenshot 7 — Scheduling Experiment

![screenshot](images/s71.jpeg)
![screenshot](images/s72.jpeg)

Two `cpu_hog` processes executed with different nice values. Output shows both running continuously with slight priority differences.

---

### Screenshot 8 — Final State

![final](images/s8.jpeg)

All containers are properly terminated. No active processes remain, indicating clean system state.

---

## 4. Engineering Analysis

### 4.1 Isolation

Isolation is achieved using Linux namespaces and `chroot`.

* PID namespace ensures containers see only their own processes
* UTS namespace allows separate hostname
* Mount namespace isolates filesystem

Each container runs in its own root filesystem using `chroot`, preventing access to host files.

---

### 4.2 Supervisor Lifecycle

The supervisor process:

* Starts containers using `clone()`
* Tracks all containers
* Handles termination using `waitpid()`
* Prevents zombie processes

It also maintains communication with CLI through UNIX socket.

---

### 4.3 IPC and Logging

Two IPC mechanisms are used:

* Pipes → capture container output
* UNIX socket → control communication

Logs are stored per container, ensuring proper output tracking.

---

### 4.4 Memory Monitoring

Kernel module monitors RSS memory:

* Soft limit → warning message
* Hard limit → container killed

From screenshots, both soft and hard limits are correctly triggered and logged.

---

### 4.5 Scheduling

Two CPU-heavy processes were run with different priorities using `nice`.

Higher priority process received slightly better CPU share, showing Linux CFS scheduling behavior.

---

## 5. Design Decisions

### Namespace Usage

Used namespaces for lightweight isolation instead of full VMs.

### Supervisor Model

Single supervisor manages all containers → simple and efficient.

### IPC Choice

Pipes for logs and sockets for control → clean separation.

### Kernel Monitoring

Kernel module ensures reliable enforcement compared to user-space monitoring.

---

## 6. Scheduler Experiment Results

### Setup

```bash
nice -n -10 ./cpu_hog &
nice -n 10 ./cpu_hog &
```

---

### Observation

* High priority process → higher CPU share
* Low priority process → lower CPU share
* Both continue running (no starvation)

---

### Conclusion

Linux CFS distributes CPU based on priority (nice values).
Higher priority gets more CPU, but fairness is maintained.
