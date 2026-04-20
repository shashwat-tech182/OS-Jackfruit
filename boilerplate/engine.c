#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- optional kernel module header ---- */
#ifdef HAVE_MONITOR
#include "monitor_ioctl.h"
#endif

/* =========================================================
 * Constants
 * ========================================================= */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MSG_LEN     512
#define CHILD_CMD_LEN       512
#define LOG_CHUNK           4096
#define BUF_CAP             16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */
#define MAX_CONTAINERS      64

/* =========================================================
 * Types
 * ========================================================= */
typedef enum {
    CMD_START, CMD_RUN, CMD_PS, CMD_LOGS, CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

static const char *state_str(container_state_t s) {
    switch (s) {
        case CONTAINER_STARTING:          return "starting";
        case CONTAINER_RUNNING:           return "running";
        case CONTAINER_STOPPED:           return "stopped";
        case CONTAINER_KILLED:            return "killed";
        case CONTAINER_EXITED:            return "exited";
        case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
        default:                          return "unknown";
    }
}

typedef struct container_record {
    char               id[CONTAINER_ID_LEN];
    pid_t              host_pid;
    container_state_t  state;
    char               log_path[PATH_MAX];
    int                stop_requested;       /* set before sending SIGTERM from 'stop' */
    int                exit_code;
    int                exit_signal;
    time_t             start_time;
    unsigned long      soft_limit_bytes;
    unsigned long      hard_limit_bytes;
    int                nice_value;
    pthread_t          producer_thread;
    int                pipe_fd;             /* read-end; producer owns/closes it */
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK];
    int    is_sentinel;  /* 1 = "consumer should flush and may check shutdown" */
} log_item_t;

typedef struct {
    log_item_t      items[BUF_CAP];
    size_t          head, tail, count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_CMD_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
    int            run_notify_fd;  /* for CMD_RUN: supervisor writes exit code here */
} control_request_t;

typedef struct {
    int  status;                      /* 0 = ok */
    char message[CONTROL_MSG_LEN];
} control_response_t;

/* config passed to child via pipe so it survives the clone stack frame */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_CMD_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int              server_fd;
    int              monitor_fd;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
    volatile int     shutdown;
} supervisor_ctx_t;

/* =========================================================
 * Globals (supervisor-side only)
 * ========================================================= */
static supervisor_ctx_t *g_ctx = NULL;  /* for signal handlers */

/* =========================================================
 * Bounded buffer
 * ========================================================= */
static void bb_init(bounded_buffer_t *b) {
    memset(b, 0, sizeof(*b));
    pthread_mutex_init(&b->mutex, NULL);
    pthread_cond_init(&b->not_empty, NULL);
    pthread_cond_init(&b->not_full, NULL);
}

static void bb_push(bounded_buffer_t *b, const log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == BUF_CAP && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);

    if (!b->shutting_down || item->is_sentinel) {
        b->items[b->tail] = *item;
        b->tail = (b->tail + 1) % BUF_CAP;
        b->count++;
        pthread_cond_signal(&b->not_empty);
    }
    pthread_mutex_unlock(&b->mutex);
}

/* Returns 0 if item was popped, -1 if should exit */
static int bb_pop(bounded_buffer_t *b, log_item_t *item) {
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0) {
        if (b->shutting_down) {
            pthread_mutex_unlock(&b->mutex);
            return -1;
        }
        pthread_cond_wait(&b->not_empty, &b->mutex);
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % BUF_CAP;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

static void bb_signal_shutdown(bounded_buffer_t *b) {
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/* =========================================================
 * Logger (consumer) thread
 * ========================================================= */
static void *logging_thread(void *arg) {
    supervisor_ctx_t *ctx = arg;
    log_item_t item;

    while (bb_pop(&ctx->log_buffer, &item) == 0) {
        if (item.length == 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *f = fopen(path, "a");
        if (f) {
            fwrite(item.data, 1, item.length, f);
            fclose(f);
        }
    }

    fprintf(stderr, "[logger] consumer thread exiting, flushing done\n");
    return NULL;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }

    return 0;
}

static void send_response(int fd, const control_response_t *resp) {
    if (write_all(fd, resp, sizeof(*resp)) != 0)
        perror("write response");
}

/* =========================================================
 * Container child function
 * ========================================================= */
static int child_fn(void *arg) {
    child_config_t *cfg = (child_config_t *)arg;

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount private");
        return 1;
    }

    /* Filesystem isolation */
    if (chdir(cfg->rootfs) != 0) {
        perror("chdir");
        return 1;
    }
    if (chroot(".") != 0) {
        perror("chroot");
        return 1;
    }

    /* Mount /proc so ps, top etc. work inside */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount proc");

    /* Apply nice value */
    if (cfg->nice_value != 0 && nice(cfg->nice_value) == -1)
        perror("nice");

    /* Redirect stdout/stderr into logging pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Exec the requested command */
    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("execl");
    return 1;
}

/* =========================================================
 * Producer thread: reads container pipe, pushes to buffer
 * ========================================================= */
typedef struct {
    container_record_t *rec;
    supervisor_ctx_t   *ctx;
} producer_arg_t;

static void *producer_thread(void *arg) {
    producer_arg_t *p = (producer_arg_t *)arg;
    container_record_t *rec = p->rec;
    supervisor_ctx_t   *ctx = p->ctx;
    free(p);

    char buf[LOG_CHUNK];
    ssize_t n;

    while ((n = read(rec->pipe_fd, buf, sizeof(buf))) > 0) {
        log_item_t item;
        memset(&item, 0, sizeof(item));
        snprintf(item.container_id, sizeof(item.container_id), "%s", rec->id);
        item.length = (size_t)n;
        memcpy(item.data, buf, (size_t)n);
        item.is_sentinel = 0;
        bb_push(&ctx->log_buffer, &item);
    }

    close(rec->pipe_fd);
    rec->pipe_fd = -1;

    fprintf(stderr, "[producer] container %s pipe closed\n", rec->id);
    return NULL;
}

/* =========================================================
 * SIGCHLD handler — reap children, update metadata
 * ========================================================= */
static void sigchld_handler(int sig) {
    (void)sig;
    if (!g_ctx) return;

    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        for (container_record_t *r = g_ctx->containers; r; r = r->next) {
            if (r->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                r->exit_code   = WEXITSTATUS(status);
                r->exit_signal = 0;
                r->state       = r->stop_requested
                                    ? CONTAINER_STOPPED
                                    : CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                r->exit_signal = WTERMSIG(status);
                r->exit_code   = 128 + r->exit_signal;
                /* hard-limit kill: SIGKILL without stop_requested */
                if (r->exit_signal == SIGKILL && !r->stop_requested)
                    r->state = CONTAINER_HARD_LIMIT_KILLED;
                else
                    r->state = CONTAINER_KILLED;
            }
            break;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

/* =========================================================
 * SIGTERM/SIGINT handler — orderly supervisor shutdown
 * ========================================================= */
static void sigterm_handler(int sig) {
    (void)sig;
    if (!g_ctx) return;
    g_ctx->shutdown = 1;

    /* Kill all running containers */
    pthread_mutex_lock(&g_ctx->metadata_lock);
    for (container_record_t *r = g_ctx->containers; r; r = r->next) {
        if (r->state == CONTAINER_RUNNING || r->state == CONTAINER_STARTING) {
            r->stop_requested = 1;
            kill(r->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_ctx->metadata_lock);

    /* Wake up accept() loop */
    close(g_ctx->server_fd);
}

/* =========================================================
 * Launch a new container
 * ========================================================= */
static pid_t launch_container(supervisor_ctx_t *ctx, const control_request_t *req) {
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return -1; }

    /* child_config must outlive the clone call — heap-allocate it */
    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) return -1;
    snprintf(cfg->id,      sizeof(cfg->id),      "%s", req->container_id);
    snprintf(cfg->rootfs,  sizeof(cfg->rootfs),  "%s", req->rootfs);
    snprintf(cfg->command, sizeof(cfg->command), "%s", req->command);
    cfg->nice_value    = req->nice_value;
    cfg->log_write_fd  = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); return -1; }

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);

    close(pipefd[1]);   /* supervisor doesn't write to this end */

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        free(cfg);
        free(stack);
        return -1;
    }
    free(stack);   /* child copied its stack pages; parent can free */
    free(cfg);     /* child already exec'd; cfg was read before exec */

    /* Build metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    snprintf(rec->id, sizeof(rec->id), "%s", req->container_id);
    rec->host_pid         = pid;
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->nice_value       = req->nice_value;
    rec->pipe_fd          = pipefd[0];
    rec->start_time       = time(NULL);
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next        = ctx->containers;
    ctx->containers  = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor if available */
#ifdef HAVE_MONITOR
    if (ctx->monitor_fd >= 0) {
        struct monitor_request mr;
        memset(&mr, 0, sizeof(mr));
        mr.pid               = pid;
        mr.soft_limit_bytes  = req->soft_limit_bytes;
        mr.hard_limit_bytes  = req->hard_limit_bytes;
        snprintf(mr.container_id, sizeof(mr.container_id), "%s", req->container_id);
        ioctl(ctx->monitor_fd, MONITOR_REGISTER, &mr);
    }
#endif

    /* Spawn producer thread */
    producer_arg_t *parg = malloc(sizeof(*parg));
    parg->rec = rec;
    parg->ctx = ctx;
    pthread_create(&rec->producer_thread, NULL, producer_thread, parg);

    fprintf(stderr, "[supervisor] started container %s pid=%d\n", rec->id, pid);
    return pid;
}

/* =========================================================
 * Handle one CLI connection
 * ========================================================= */
static void handle_client(supervisor_ctx_t *ctx, int fd) {
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    ssize_t nr = read(fd, &req, sizeof(req));
    if (nr != sizeof(req)) {
        resp.status = 1;
        snprintf(resp.message, CONTROL_MSG_LEN, "bad request size");
        send_response(fd, &resp);
        return;
    }

    /* ---- start ---- */
    if (req.kind == CMD_START) {
        if (req.soft_limit_bytes == 0) req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
        if (req.hard_limit_bytes == 0) req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

        /* Check for duplicate ID */
        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            if (strcmp(r->id, req.container_id) == 0 &&
                r->state == CONTAINER_RUNNING) {
                pthread_mutex_unlock(&ctx->metadata_lock);
                resp.status = 1;
                snprintf(resp.message, CONTROL_MSG_LEN,
                         "container '%s' already running", req.container_id);
                send_response(fd, &resp);
                return;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = 1;
            snprintf(resp.message, CONTROL_MSG_LEN, "launch failed");
        } else {
            resp.status = 0;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "started container '%s' pid=%d", req.container_id, pid);
        }

    /* ---- run ---- */
    } else if (req.kind == CMD_RUN) {
        if (req.soft_limit_bytes == 0) req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
        if (req.hard_limit_bytes == 0) req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

        pid_t pid = launch_container(ctx, &req);
        if (pid < 0) {
            resp.status = 1;
            snprintf(resp.message, CONTROL_MSG_LEN, "launch failed");
            send_response(fd, &resp);
            return;
        }

        /* Send ack then block until container exits */
        resp.status = 0;
        snprintf(resp.message, CONTROL_MSG_LEN, "running '%s' pid=%d",
                 req.container_id, pid);
        send_response(fd, &resp);

        /* Poll for container exit */
        while (1) {
            usleep(200000);
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *r = NULL;
            for (r = ctx->containers; r; r = r->next)
                if (strcmp(r->id, req.container_id) == 0) break;
            int done = r && r->state != CONTAINER_RUNNING
                         && r->state != CONTAINER_STARTING;
            int code = r ? r->exit_code : 0;
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (done) {
                memset(&resp, 0, sizeof(resp));
                resp.status = code;
                snprintf(resp.message, CONTROL_MSG_LEN,
                         "container '%s' exited, code=%d", req.container_id, code);
                send_response(fd, &resp);
                return;
            }
        }

    /* ---- ps ---- */
    } else if (req.kind == CMD_PS) {
        char buf[CONTROL_MSG_LEN];
        int  off = 0;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%-16s %-8s %-20s %-12s %-10s\n",
                        "ID", "PID", "STARTED", "STATE", "EXIT");

        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *r = ctx->containers; r; r = r->next) {
            char ts[20];
            struct tm *tm = localtime(&r->start_time);
            strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", tm);
            off += snprintf(buf + off, sizeof(buf) - off,
                            "%-16s %-8d %-20s %-12s %-10d\n",
                            r->id, r->host_pid, ts,
                            state_str(r->state), r->exit_code);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (off == 0) snprintf(buf, sizeof(buf), "(no containers)\n");
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "%s", buf);

    /* ---- logs ---- */
    } else if (req.kind == CMD_LOGS) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);

        /* Send the first CONTROL_MSG_LEN bytes of the log file as the response */
        FILE *f = fopen(path, "r");
        if (!f) {
            resp.status = 1;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "no log file for '%s'", req.container_id);
        } else {
            resp.status = 0;
            size_t n = fread(resp.message, 1, CONTROL_MSG_LEN - 1, f);
            resp.message[n] = '\0';
            fclose(f);
        }

    /* ---- stop ---- */
    } else if (req.kind == CMD_STOP) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = NULL;
        for (r = ctx->containers; r; r = r->next)
            if (strcmp(r->id, req.container_id) == 0) break;

        if (!r || r->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = 1;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "container '%s' not running", req.container_id);
        } else {
            r->stop_requested = 1;
            pid_t pid = r->host_pid;
            pthread_mutex_unlock(&ctx->metadata_lock);

            kill(pid, SIGTERM);
            /* Give it 2 s then SIGKILL */
            usleep(2000000);
            kill(pid, SIGKILL);

            resp.status = 0;
            snprintf(resp.message, CONTROL_MSG_LEN,
                     "stop signal sent to '%s'", req.container_id);
        }

    } else {
        resp.status = 1;
        snprintf(resp.message, CONTROL_MSG_LEN, "unknown command %d", req.kind);
    }

    send_response(fd, &resp);
}

/* =========================================================
 * Supervisor main loop
 * ========================================================= */
static int run_supervisor(const char *base_rootfs) {
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    g_ctx = &ctx;

    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bb_init(&ctx.log_buffer);

    mkdir(LOG_DIR, 0755);

    /* Open kernel monitor device if present */
    ctx.monitor_fd = -1;
#ifdef HAVE_MONITOR
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] warning: no /dev/container_monitor\n");
#endif

    /* Start consumer (logger) thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create logger");
        return 1;
    }

    /* Install signal handlers */
    struct sigaction sa_chld = {0};
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    struct sigaction sa_term = {0};
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT,  &sa_term, NULL);

    /* UNIX domain socket for CLI */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen"); return 1;
    }

    fprintf(stderr, "[supervisor] listening on %s, rootfs=%s\n",
            CONTROL_PATH, base_rootfs);

    while (!ctx.shutdown) {
        int client = accept(ctx.server_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == EBADF) break;
            continue;
        }
        handle_client(&ctx, client);
        close(client);
    }

    /* Shutdown sequence */
    fprintf(stderr, "[supervisor] shutting down\n");

    /* Wait for all producer threads */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *r = ctx.containers; r; r = r->next) {
        if (r->producer_thread)
            pthread_join(r->producer_thread, NULL);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Signal and join logger thread */
    bb_signal_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);

    /* Free metadata */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        free(r);
        r = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    unlink(CONTROL_PATH);
    fprintf(stderr, "[supervisor] exited cleanly\n");
    return 0;
}

/* =========================================================
 * CLI client: parse args and send to supervisor
 * ========================================================= */
static int send_request(control_request_t *req) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "error: cannot connect to supervisor at %s"
                        " — is it running?\n", CONTROL_PATH);
        close(s);
        return 1;
    }

    if (write(s, req, sizeof(*req)) != sizeof(*req)) {
        perror("write"); close(s); return 1;
    }

    control_response_t resp;
    if (read(s, &resp, sizeof(resp)) != sizeof(resp)) {
        perror("read"); close(s); return 1;
    }
    printf("%s\n", resp.message);

    /* For run: keep reading until container exits */
    if (req->kind == CMD_RUN) {
        while (1) {
            ssize_t n = read(s, &resp, sizeof(resp));
            if (n <= 0) break;
            printf("%s\n", resp.message);
            /* Supervisor sends a second response when done */
            break;
        }
    }

    close(s);
    return resp.status;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start  <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run    <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs   <id>\n"
        "  %s stop   <id>\n",
        prog, prog, prog, prog, prog, prog);
}

/* =========================================================
 * main
 * ========================================================= */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* ---- supervisor ---- */
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        return run_supervisor(argv[2]);
    }

    /* ---- CLI commands ---- */
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    /* Helper: parse optional flags from position i onwards */
    /* Returns index of first non-flag arg (or argc) */
    #define PARSE_FLAGS(start) do {                                        \
        for (int _i = (start); _i < argc; _i++) {                          \
            if (strcmp(argv[_i], "--soft-mib") == 0 && _i+1 < argc)        \
                req.soft_limit_bytes = (unsigned long)atol(argv[++_i]) << 20;\
            else if (strcmp(argv[_i], "--hard-mib") == 0 && _i+1 < argc)   \
                req.hard_limit_bytes = (unsigned long)atol(argv[++_i]) << 20;\
            else if (strcmp(argv[_i], "--nice") == 0 && _i+1 < argc)       \
                req.nice_value = atoi(argv[++_i]);                          \
        }                                                                    \
    } while(0)

    if (strcmp(argv[1], "start") == 0) {
        if (argc < 5) { print_usage(argv[0]); return 1; }
        req.kind = CMD_START;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
        strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
        strncpy(req.command,      argv[4], CHILD_CMD_LEN - 1);
        PARSE_FLAGS(5);

    } else if (strcmp(argv[1], "run") == 0) {
        if (argc < 5) { print_usage(argv[0]); return 1; }
        req.kind = CMD_RUN;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
        strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
        strncpy(req.command,      argv[4], CHILD_CMD_LEN - 1);
        PARSE_FLAGS(5);

    } else if (strcmp(argv[1], "ps") == 0) {
        req.kind = CMD_PS;

    } else if (strcmp(argv[1], "logs") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        req.kind = CMD_LOGS;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);

    } else if (strcmp(argv[1], "stop") == 0) {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        req.kind = CMD_STOP;
        strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);

    } else {
        fprintf(stderr, "unknown command: %s\n", argv[1]);
        print_usage(argv[0]);
        return 1;
    }

    return send_request(&req);
}
