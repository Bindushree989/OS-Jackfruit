# Multi-Container Runtime

## 1. Team Information

| Name             | SRN              |
|------------------|------------------|
| Bindushree       | PES1UG24AM901    |
| Harsha Rajeswari |PES1UG24AM014     |

---

## 2. Build, Load, and Run Instructions

### Prerequisites
```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r) musl-tools
```

### Download Alpine rootfs
```bash
mkdir -p rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base
```

### Build
```bash
make
```

This builds:
- `engine` — user-space runtime and supervisor
- `cpu_workload`, `mem_workload`, `io_workload` — test workloads (statically linked)
- `monitor.ko` — kernel module

### Prepare rootfs copies
```bash
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta

cp cpu_workload mem_workload io_workload rootfs-alpha/
cp cpu_workload mem_workload io_workload rootfs-beta/
```

### Load kernel module
```bash
sudo insmod monitor.ko
ls /dev/container_monitor
```

### Start supervisor (Terminal 1)
```bash
sudo ./engine supervisor ./rootfs-base
```

### Launch containers (Terminal 2)
```bash
sudo ./engine start alpha ./rootfs-alpha /cpu_workload 30 --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /cpu_workload 30 --soft-mib 48 --hard-mib 80
```

### List containers
```bash
sudo ./engine ps
```

### View logs
```bash
sudo ./engine logs alpha
# or directly:
cat /tmp/container_alpha.log
```

### Stop a container
```bash
sudo ./engine stop alpha
```

### Run memory limit test
```bash
sudo ./engine start memtest ./rootfs-alpha /mem_workload 50 --soft-mib 10 --hard-mib 20
sleep 30
sudo dmesg | tail -20
```

### Run scheduling experiment
```bash
sudo ./engine start hi ./rootfs-alpha /cpu_workload 30
sudo nice -n 19 ./engine start lo ./rootfs-beta /cpu_workload 30
sleep 35
cat /tmp/container_hi.log
cat /tmp/container_lo.log
```

### Clean up
```bash
sudo ./engine stop alpha
sudo ./engine stop beta
# Press Ctrl+C in Terminal 1 to stop supervisor
sudo rmmod monitor
sudo rm -f /tmp/engine.sock /tmp/container_*.log
```

---

## 3. Demo Screenshots

### Screenshot 1 — Multi-container supervision
Two containers (alpha and beta) started under one supervisor process. The supervisor remains alive while both containers run concurrently.
![Supervisor](/s1.jpeg)

### Screenshot 2 — Metadata tracking
Output of `engine ps` showing container ID, PID, start time, state, and log path for all tracked containers.
![cpu_workload](/s2.jpeg)

### Screenshot 3 — Bounded-buffer logging
Contents of `/tmp/container_alpha.log` captured through the producer-consumer logging pipeline. Shows cpu_workload output being captured line by line.
![start logs](/s3.jpeg)

### Screenshot 4 — CLI and IPC
`engine stop alpha` command sent over UNIX domain socket to supervisor. Supervisor responds with confirmation and ps shows state changed to `stopped`.
![dmesg](/s4.jpeg)

### Screenshot 5 — Soft-limit warning
`dmesg` output showing `container_monitor: SOFT LIMIT pid=XXXX (memtest4) rss=15360KB soft=10240KB` — kernel module detected RSS exceeded soft limit.
![ps](/s5.jpeg)

### Screenshot 6 — Hard-limit enforcement
`dmesg` output showing `container_monitor: HARD LIMIT pid=XXXX (memtest4) rss=25600KB hard=20480KB -- killing` and `engine ps` showing state as `hard_limit_killed`.
![stop](/s6.jpeg)

### Screenshot 7 — Scheduling experiment
Log output from `hi` (nice=0) and `lo` (nice=19) containers running the same workload. `hi` accumulates more iterations than `lo` in the same time period, demonstrating CFS priority scheduling.
![screenshot](/s71.jpeg)
![screenshot](/s72.jpeg)

### Screenshot 8 — Clean teardown
`ps aux | grep engine` shows no lingering engine processes after supervisor shutdown. `dmesg` shows clean module unload. No zombie processes remain.
![final](/s8.jpeg)

---

## 4. Engineering Analysis

### Isolation Mechanisms

The runtime achieves process isolation using Linux namespaces via `clone()` with three flags. `CLONE_NEWPID` gives each container its own PID namespace — the first process inside sees itself as PID 1, completely isolated from host PIDs. `CLONE_NEWUTS` isolates the hostname so each container can have its own hostname without affecting the host. `CLONE_NEWNS` creates a separate mount namespace so filesystem mounts inside the container do not propagate to the host.

`chroot()` changes the container's root directory to its assigned rootfs copy, preventing it from accessing host files. The kernel still shares the same underlying hardware, scheduler, and network stack with all containers — true network and user namespace isolation would require `CLONE_NEWNET` and `CLONE_NEWUSER`, which this runtime does not implement.

Filesystem isolation requires a separate writable rootfs per container. Sharing a rootfs between two running containers would cause filesystem corruption since both would write to the same inodes concurrently.

### Supervisor and Process Lifecycle

A long-running supervisor is necessary because containers are child processes — only their parent can reap them with `waitpid()`. Without a persistent parent, exited containers become zombies that hold PID table entries until the parent collects their exit status.

The supervisor installs a `SIGCHLD` handler that calls `waitpid(-1, &status, WNOHANG)` in a loop to reap all exited children non-blockingly. `WNOHANG` is critical — it prevents the handler from blocking if multiple children exit simultaneously. The exit status is decoded using `WIFEXITED`/`WIFSIGNALED` macros and stored in the container metadata, allowing `engine ps` to report accurate final states.

Container creation uses `clone()` instead of `fork()` because `clone()` accepts namespace flags. The child stack must be allocated on the heap and the pointer passed to `clone()` must point to the top of the stack (stacks grow downward on x86-64).

### IPC, Threads, and Synchronization

The project uses two separate IPC mechanisms. Path A (logging) uses pipes — each container's stdout and stderr are redirected into a pipe via `dup2()`. The supervisor's producer thread reads from the pipe's read end and pushes data into the bounded buffer. Path B (control) uses a UNIX domain socket — CLI processes connect, send a command string, and receive a response. These are intentionally separate: mixing control commands and log data on the same channel would require complex framing and demultiplexing.

The bounded buffer uses a mutex (`pthread_mutex_t`) to protect the head/tail/count fields from concurrent access by multiple producer threads and the consumer thread. Two condition variables (`not_full`, `not_empty`) implement blocking: producers wait on `not_full` when the buffer is full, consumers wait on `not_empty` when it is empty. Without the mutex, two producers could simultaneously read `tail`, compute the same slot index, and overwrite each other's data. Without condition variables, threads would busy-wait (spin-poll) wasting CPU.

The container metadata array is protected by a separate `meta_lock` mutex. This is intentional — holding the log buffer lock while accessing metadata (or vice versa) would risk deadlock if two threads each hold one lock and wait for the other.

### Memory Management and Enforcement

RSS (Resident Set Size) measures the amount of physical RAM currently mapped and used by a process. It does not measure virtual memory, shared libraries counted multiple times, or memory that has been swapped out. A process can have large virtual memory but small RSS if pages have not been faulted in yet.

Soft and hard limits implement different policies intentionally. A soft limit is a warning threshold — the process is allowed to continue but the operator is alerted that memory usage is climbing. A hard limit is an enforcement threshold — the kernel module sends SIGKILL to terminate the process immediately. This two-level design allows operators to react to approaching limits before they become critical.

Enforcement belongs in kernel space because user-space monitoring can be bypassed — a runaway process could consume all memory faster than a user-space polling loop detects it. The kernel module runs a timer interrupt that checks RSS every 2 seconds using `get_mm_rss()` on the task's `mm_struct`, which is only accessible from kernel space. Sending SIGKILL from kernel space is also atomic and cannot be caught or ignored by the target process.

### Scheduling Behavior

The Linux Completely Fair Scheduler (CFS) assigns CPU time proportionally based on process weight, which is derived from the nice value. A process with nice=0 has weight 1024; nice=19 has weight 15. The scheduler maintains a virtual runtime (`vruntime`) per process and always runs the process with the smallest `vruntime` next.

In our experiment, `hi` (nice=0) and `lo` (nice=19) ran the same cpu_workload simultaneously. `hi` received approximately 68x more CPU time than `lo` based on CFS weight ratios. This is visible in the log files — `hi` completes more accumulator iterations in the same wall-clock time. This demonstrates CFS's weighted fairness: processes are not treated equally but proportionally to their priority weight, achieving throughput goals for high-priority work while still allowing low-priority work to make progress.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** Used `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` with `chroot()`.
**Tradeoff:** `chroot()` is simpler than `pivot_root()` but less secure — a privileged process inside the container could escape via `..` traversal. `pivot_root()` would be more thorough.
**Justification:** For this project's scope, `chroot()` provides sufficient filesystem isolation while keeping the implementation straightforward.

### Supervisor Architecture
**Choice:** Single supervisor process with a socket listener thread handling all CLI commands.
**Tradeoff:** All container metadata lives in one process's memory. If the supervisor crashes, all metadata is lost. A persistent metadata store (e.g., file or database) would survive crashes but adds complexity.
**Justification:** For a runtime managing a small number of containers in a controlled environment, in-memory metadata is sufficient and avoids I/O overhead.

### IPC and Logging
**Choice:** Pipes for logging (Path A) and UNIX domain socket for control (Path B).
**Tradeoff:** Two separate IPC mechanisms add code complexity. A single multiplexed channel would be simpler but would require framing to distinguish log data from control responses.
**Justification:** Separation of concerns — log data is high-volume and streaming; control commands are low-volume and request-response. Different characteristics justify different mechanisms.

### Kernel Monitor
**Choice:** Timer-based polling every 2 seconds using `get_mm_rss()`.
**Tradeoff:** 2-second polling means a process could exceed its hard limit by a significant amount before being killed. Event-driven enforcement (e.g., memory cgroups) would be more precise but much more complex to implement.
**Justification:** For demonstration purposes, 2-second polling is sufficient to show the enforcement mechanism working correctly.

### Scheduling Experiments
**Choice:** Used `nice` values to differentiate container priorities rather than CPU affinity or cgroups.
**Tradeoff:** Nice values affect CFS weight but do not provide hard CPU isolation — a low-priority process still gets some CPU time. CPU affinity would pin containers to specific cores, giving harder isolation.
**Justification:** Nice values directly demonstrate CFS scheduling behavior and are easy to observe and measure without requiring cgroup setup.

---

## 6. Scheduler Experiment Results

### Setup
Two containers ran the same `cpu_workload` for 30 seconds simultaneously:
- `hi`: nice=0 (normal priority, CFS weight=1024)
- `lo`: nice=19 (lowest priority, CFS weight=15)

### Results

| Container | Nice Value | CFS Weight | Iterations Completed |
|-----------|-----------|------------|---------------------|
| hi        | 0         | 1024       | Higher              |
| lo        | 19        | 15         | Lower               |

### Analysis

The CFS scheduler assigned CPU time proportionally to weight. With weight ratio 1024:15, `hi` received approximately 68x more CPU time than `lo`. This is visible in the log files — `hi` accumulated significantly more loop iterations than `lo` in the same 30-second window.

This demonstrates CFS's core design goal: weighted fairness. Unlike a strict priority scheduler that would starve `lo` completely, CFS still allows `lo` to make progress — just at a much slower rate. This achieves both throughput (high-priority work completes faster) and fairness (low-priority work is not completely blocked).

The result confirms that `nice` values are an effective mechanism for controlling relative CPU allocation between competing workloads in Linux.
