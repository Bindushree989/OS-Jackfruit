#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include "monitor_ioctl.h"

#define STACK_SIZE      (1024 * 1024)
#define MAX_CONTAINERS  16
#define MAX_ARGS        32
#define SOCKET_PATH     "/tmp/engine.sock"
#define LOG_DIR         "/tmp"
#define BUF_SLOTS       256
#define BUF_LINE_MAX    4096

typedef enum {
    STATE_STARTING, STATE_RUNNING, STATE_STOPPED,
    STATE_KILLED, STATE_HARD_LIMIT_KILLED, STATE_EXITED
} ContainerState;

typedef struct {
    char           id[64];
    pid_t          pid;
    time_t         start_time;
    ContainerState state;
    int            soft_mib;
    int            hard_mib;
    char           log_path[256];
    int            exit_code;
    int            exit_signal;
    int            stop_requested;
    int            used;
} ContainerMeta;

typedef struct {
    char id[64];
    char line[BUF_LINE_MAX];
    int  len;
} LogEntry;

typedef struct {
    LogEntry        slots[BUF_SLOTS];
    int             head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    int             shutdown;
} BoundedBuffer;

typedef struct {
    char  rootfs[256];
    char  argbuf[256 * MAX_ARGS];
    char *argv[MAX_ARGS];
    int   argc;
    int   pipe_out[2];
} CloneArgs;

typedef struct {
    int  read_fd;
    char id[64];
} ProducerArgs;

typedef struct {
    char id[64];
    int  client_fd;
} RunWaitArgs;

static ContainerMeta   containers[MAX_CONTAINERS];
static pthread_mutex_t meta_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    running   = 1;
static BoundedBuffer   log_buf;
static int             monitor_fd = -1;

static void bb_init(BoundedBuffer *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->not_full,  NULL);
    pthread_cond_init(&b->not_empty, NULL);
}

static void bb_push(BoundedBuffer *b, const char *id, const char *data, int len) {
    pthread_mutex_lock(&b->lock);
    while (b->count == BUF_SLOTS && !b->shutdown)
        pthread_cond_wait(&b->not_full, &b->lock);
    if (b->shutdown) { pthread_mutex_unlock(&b->lock); return; }
    LogEntry *e = &b->slots[b->tail];
    strncpy(e->id, id, 63);
    int copy = len < BUF_LINE_MAX ? len : BUF_LINE_MAX - 1;
    memcpy(e->line, data, copy);
    e->len  = copy;
    b->tail = (b->tail + 1) % BUF_SLOTS;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

static int bb_pop(BoundedBuffer *b, LogEntry *out) {
    pthread_mutex_lock(&b->lock);
    while (b->count == 0 && !b->shutdown)
        pthread_cond_wait(&b->not_empty, &b->lock);
    if (b->count == 0) { pthread_mutex_unlock(&b->lock); return 0; }
    *out    = b->slots[b->head];
    b->head = (b->head + 1) % BUF_SLOTS;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->lock);
    return 1;
}

static void bb_shutdown(BoundedBuffer *b) {
    pthread_mutex_lock(&b->lock);
    b->shutdown = 1;
    pthread_cond_broadcast(&b->not_full);
    pthread_cond_broadcast(&b->not_empty);
    pthread_mutex_unlock(&b->lock);
}

static ContainerMeta *find_by_id(const char *id) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].used && strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static ContainerMeta *find_by_pid(pid_t pid) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].used && containers[i].pid == pid)
            return &containers[i];
    return NULL;
}

static ContainerMeta *alloc_slot(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].used) return &containers[i];
    return NULL;
}

static const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING:          return "starting";
        case STATE_RUNNING:           return "running";
        case STATE_STOPPED:           return "stopped";
        case STATE_KILLED:            return "killed";
        case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
        case STATE_EXITED:            return "exited";
        default:                      return "unknown";
    }
}

static void sigchld_handler(int sig) {
    (void)sig;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&meta_lock);
        ContainerMeta *m = find_by_pid(pid);
        if (m) {
            if (WIFEXITED(status)) {
                m->exit_code = WEXITSTATUS(status);
                m->exit_signal = 0;
                m->state = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                m->exit_signal = WTERMSIG(status);
                m->exit_code   = 128 + m->exit_signal;
                if (m->stop_requested)
                    m->state = STATE_STOPPED;
                else if (m->exit_signal == SIGKILL)
                    m->state = STATE_HARD_LIMIT_KILLED;
                else
                    m->state = STATE_KILLED;
            }
        }
        pthread_mutex_unlock(&meta_lock);
    }
}

static void shutdown_handler(int sig) { (void)sig; running = 0; }

static void km_register(const char *id, pid_t pid, int soft, int hard) {
    if (monitor_fd < 0) return;
    struct container_reg r;
    memset(&r, 0, sizeof(r));
    r.pid = pid; r.soft_mib = soft; r.hard_mib = hard;
    strncpy(r.id, id, 63);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &r) < 0)
        perror("[engine] ioctl REGISTER");
}

static void km_unregister(pid_t pid) {
    if (monitor_fd < 0) return;
    struct container_unreg u; u.pid = pid;
    ioctl(monitor_fd, MONITOR_UNREGISTER, &u);
}

static int container_main(void *arg) {
    CloneArgs *a = (CloneArgs *)arg;
    close(a->pipe_out[0]);
    dup2(a->pipe_out[1], STDOUT_FILENO);
    dup2(a->pipe_out[1], STDERR_FILENO);
    close(a->pipe_out[1]);
    sethostname("container", 9);
    if (chroot(a->rootfs) != 0) { perror("chroot"); return 1; }
    chdir("/");
    mount("proc", "/proc", "proc", 0, NULL);
    execv(a->argv[0], a->argv);
    perror("execv");
    return 1;
}

static void *producer_thread(void *arg) {
    ProducerArgs *pa = (ProducerArgs *)arg;
    char buf[BUF_LINE_MAX];
    ssize_t n;
    while ((n = read(pa->read_fd, buf, sizeof(buf)-1)) > 0)
        bb_push(&log_buf, pa->id, buf, (int)n);
    close(pa->read_fd);
    free(pa);
    return NULL;
}

static void *consumer_thread(void *arg) {
    (void)arg;
    LogEntry e;
    while (bb_pop(&log_buf, &e)) {
        pthread_mutex_lock(&meta_lock);
        ContainerMeta *m = find_by_id(e.id);
        char path[256] = "";
        if (m) strncpy(path, m->log_path, 255);
        pthread_mutex_unlock(&meta_lock);
        if (path[0]) {
            int fd = open(path, O_WRONLY|O_CREAT|O_APPEND, 0644);
            if (fd >= 0) { write(fd, e.line, e.len); close(fd); }
        }
    }
    return NULL;
}

static int spawn_container(const char *id, const char *rootfs,
                            char **argv, int argc,
                            int soft_mib, int hard_mib,
                            char *out, int outsz) {
    pthread_mutex_lock(&meta_lock);
    if (find_by_id(id)) {
        snprintf(out, outsz, "ERROR: container '%s' already exists\n", id);
        pthread_mutex_unlock(&meta_lock);
        return -1;
    }
    ContainerMeta *m = alloc_slot();
    if (!m) {
        snprintf(out, outsz, "ERROR: max containers reached\n");
        pthread_mutex_unlock(&meta_lock);
        return -1;
    }
    memset(m, 0, sizeof(*m));
    strncpy(m->id, id, sizeof(m->id)-1);
    m->soft_mib   = soft_mib;
    m->hard_mib   = hard_mib;
    m->start_time = time(NULL);
    m->state      = STATE_STARTING;
    m->used       = 1;
    snprintf(m->log_path, sizeof(m->log_path), "%s/container_%s.log", LOG_DIR, id);
    pthread_mutex_unlock(&meta_lock);

    CloneArgs *ca = calloc(1, sizeof(CloneArgs));
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs)-1);
    ca->argc = argc;
    char *ptr = ca->argbuf;
    for (int i = 0; i < argc && i < MAX_ARGS-1; i++) {
        strncpy(ptr, argv[i], 255);
        ca->argv[i] = ptr;
        ptr += 256;
    }
    ca->argv[argc] = NULL;

    if (pipe(ca->pipe_out) != 0) {
        perror("pipe"); free(ca);
        snprintf(out, outsz, "ERROR: pipe failed\n");
        return -1;
    }

    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(container_main, stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, ca);
    if (pid < 0) {
        perror("clone"); free(stack); free(ca);
        snprintf(out, outsz, "ERROR: clone failed\n");
        return -1;
    }

    close(ca->pipe_out[1]);

    pthread_mutex_lock(&meta_lock);
    m->pid   = pid;
    m->state = STATE_RUNNING;
    pthread_mutex_unlock(&meta_lock);

    km_register(id, pid, soft_mib, hard_mib);

    ProducerArgs *pa = malloc(sizeof(ProducerArgs));
    pa->read_fd = ca->pipe_out[0];
    strncpy(pa->id, id, 63);
    pthread_t ptid;
    pthread_create(&ptid, NULL, producer_thread, pa);
    pthread_detach(ptid);

    snprintf(out, outsz, "OK: started '%s' pid=%d\n", id, pid);
    return 0;
}

static void *run_wait_thread(void *arg) {
    RunWaitArgs *rwa = (RunWaitArgs *)arg;
    char id[64]; strncpy(id, rwa->id, 63);
    int cfd = rwa->client_fd;
    free(rwa);
    while (1) {
        sleep(1);
        pthread_mutex_lock(&meta_lock);
        ContainerMeta *m = find_by_id(id);
        if (!m || m->state != STATE_RUNNING) {
            char resp[128] = "OK: exited\n";
            if (m) snprintf(resp, sizeof(resp), "OK: exit_code=%d\n", m->exit_code);
            pthread_mutex_unlock(&meta_lock);
            write(cfd, resp, strlen(resp));
            close(cfd);
            return NULL;
        }
        pthread_mutex_unlock(&meta_lock);
    }
}

static void handle_command(const char *cmd, int client_fd) {
    char resp[4096] = "";
    char buf[4096];
    strncpy(buf, cmd, sizeof(buf)-1);
    char *tokens[64]; int tc = 0;
    char *p = strtok(buf, " \t\n");
    while (p && tc < 63) { tokens[tc++] = p; p = strtok(NULL, " \t\n"); }
    if (tc == 0) { write(client_fd, "ERROR: empty\n", 13); close(client_fd); return; }

    if (strcmp(tokens[0], "start") == 0 || strcmp(tokens[0], "run") == 0) {
        int is_run = (strcmp(tokens[0], "run") == 0);
        if (tc < 4) {
            snprintf(resp, sizeof(resp), "ERROR: usage: %s <id> <rootfs> <cmd>\n", tokens[0]);
            write(client_fd, resp, strlen(resp)); close(client_fd); return;
        }
        char *id = tokens[1], *rootfs = tokens[2];
        int soft = 40, hard = 64;
        char *argv[MAX_ARGS]; int argc = 0;
        for (int i = 3; i < tc; i++) {
            if (strcmp(tokens[i], "--soft-mib") == 0 && i+1 < tc)      soft = atoi(tokens[++i]);
            else if (strcmp(tokens[i], "--hard-mib") == 0 && i+1 < tc) hard = atoi(tokens[++i]);
            else if (strcmp(tokens[i], "--nice") == 0 && i+1 < tc)     i++;
            else argv[argc++] = tokens[i];
        }
        argv[argc] = NULL;
        int rc = spawn_container(id, rootfs, argv, argc, soft, hard, resp, sizeof(resp));
        write(client_fd, resp, strlen(resp));
        if (rc == 0 && is_run) {
            RunWaitArgs *rwa = malloc(sizeof(RunWaitArgs));
            strncpy(rwa->id, id, 63); rwa->client_fd = client_fd;
            pthread_t t; pthread_create(&t, NULL, run_wait_thread, rwa); pthread_detach(t);
            return;
        }
        close(client_fd); return;
    }

    if (strcmp(tokens[0], "ps") == 0) {
        pthread_mutex_lock(&meta_lock);
        int n = 0;
        n += snprintf(resp+n, sizeof(resp)-n, "%-12s %-8s %-20s %-20s %s\n",
                      "ID","PID","STARTED","STATE","LOG");
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].used) continue;
            ContainerMeta *m = &containers[i];
            char ts[32]; struct tm *t = localtime(&m->start_time);
            strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
            n += snprintf(resp+n, sizeof(resp)-n, "%-12s %-8d %-20s %-20s %s\n",
                          m->id, m->pid, ts, state_str(m->state), m->log_path);
        }
        pthread_mutex_unlock(&meta_lock);
        write(client_fd, resp, strlen(resp)); close(client_fd); return;
    }

    if (strcmp(tokens[0], "logs") == 0) {
        if (tc < 2) { write(client_fd, "ERROR: logs <id>\n", 17); close(client_fd); return; }
        pthread_mutex_lock(&meta_lock);
        ContainerMeta *m = find_by_id(tokens[1]);
        char path[256] = "";
        if (m) strncpy(path, m->log_path, 255);
        pthread_mutex_unlock(&meta_lock);
        if (!path[0]) {
            snprintf(resp, sizeof(resp), "ERROR: no container '%s'\n", tokens[1]);
            write(client_fd, resp, strlen(resp)); close(client_fd); return;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            snprintf(resp, sizeof(resp), "ERROR: log file missing\n");
            write(client_fd, resp, strlen(resp)); close(client_fd); return;
        }
        char lbuf[4096]; ssize_t nr;
        while ((nr = read(fd, lbuf, sizeof(lbuf))) > 0) write(client_fd, lbuf, nr);
        close(fd); close(client_fd); return;
    }

    if (strcmp(tokens[0], "stop") == 0) {
        if (tc < 2) { write(client_fd, "ERROR: stop <id>\n", 17); close(client_fd); return; }
        pthread_mutex_lock(&meta_lock);
        ContainerMeta *m = find_by_id(tokens[1]);
        if (!m)
            snprintf(resp, sizeof(resp), "ERROR: no container '%s'\n", tokens[1]);
        else if (m->state != STATE_RUNNING)
            snprintf(resp, sizeof(resp), "ERROR: '%s' not running (state=%s)\n",
                     tokens[1], state_str(m->state));
        else {
            m->stop_requested = 1;
            kill(m->pid, SIGTERM);
            km_unregister(m->pid);
            snprintf(resp, sizeof(resp), "OK: sent SIGTERM to '%s'\n", tokens[1]);
        }
        pthread_mutex_unlock(&meta_lock);
        write(client_fd, resp, strlen(resp)); close(client_fd); return;
    }

    snprintf(resp, sizeof(resp), "ERROR: unknown command '%s'\n", tokens[0]);
    write(client_fd, resp, strlen(resp)); close(client_fd);
}

static void *socket_listener(void *arg) {
    (void)arg;
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(SOCKET_PATH);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    chmod(SOCKET_PATH, 0777);
    listen(srv, 16);
    printf("[supervisor] listening on %s\n", SOCKET_PATH);
    while (running) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) continue;
        char buf[4096] = {0};
        ssize_t n = read(cfd, buf, sizeof(buf)-1);
        if (n > 0) handle_command(buf, cfd);
        else close(cfd);
    }
    close(srv); unlink(SOCKET_PATH); return NULL;
}

static int cli_client(int argc, char *argv[]) {
    char cmd[4096] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) strncat(cmd, " ", sizeof(cmd)-strlen(cmd)-1);
        strncat(cmd, argv[i], sizeof(cmd)-strlen(cmd)-1);
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect — is supervisor running?"); return 1;
    }
    write(fd, cmd, strlen(cmd));
    char buf[4096]; ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) fwrite(buf, 1, n, stdout);
    close(fd); return 0;
}

static int supervisor_main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler; sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = shutdown_handler; sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    bb_init(&log_buf);
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, consumer_thread, NULL);

    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[supervisor] kernel monitor not loaded — memory limits disabled\n");
    else
        printf("[supervisor] kernel monitor connected\n");

    pthread_t listener_tid;
    pthread_create(&listener_tid, NULL, socket_listener, NULL);

    printf("[supervisor] ready\n");
    while (running) sleep(1);

    printf("[supervisor] shutting down...\n");
    pthread_mutex_lock(&meta_lock);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        ContainerMeta *m = &containers[i];
        if (m->used && m->state == STATE_RUNNING) {
            m->stop_requested = 1;
            kill(m->pid, SIGTERM);
            km_unregister(m->pid);
        }
    }
    pthread_mutex_unlock(&meta_lock);
    sleep(2);
    bb_shutdown(&log_buf);
    pthread_join(consumer_tid, NULL);
    if (monitor_fd >= 0) close(monitor_fd);
    unlink(SOCKET_PATH);
    printf("[supervisor] done\n");
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  engine ps\n"
            "  engine logs <id>\n"
            "  engine stop <id>\n");
        return 1;
    }
    if (strcmp(argv[1], "supervisor") == 0) return supervisor_main();
    return cli_client(argc, argv);
}
