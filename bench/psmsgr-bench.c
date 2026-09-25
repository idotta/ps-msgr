/* SPDX-License-Identifier: Apache-2.0 */
/*
 * psmsgr-bench: latency of the state channel operations, for the on-target
 * validation (spec/build-and-test.md). How to run it on the board:
 * bench/README.md.
 *
 * Every figure is CLOCK_MONOTONIC around the operation, in nanoseconds per
 * operation. The "timer overhead" row is the same measurement around nothing;
 * it is included in every other row.
 */
#include <psmsgr/psmsgr.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/time_types.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CLOCK_BATCH 100 /* clock and syscall rows: calls per timed sample */

static const uint32_t sizes[] = { 16, 256, 4096, 65536 };
#define NSIZES (sizeof sizes / sizeof *sizes)

static const char usage_text[] =
    "usage: psmsgr-bench [options]\n"
    "\n"
    "Measures publish, read and peek latency for 16 B, 256 B, 4 KiB and 64 KiB\n"
    "payloads, uncontended and against a writer in another process, publish\n"
    "against the zero-copy begin/commit with the producer's fill, the wake-up\n"
    "latency of wait (and of polling on a NO_NOTIFY channel), and the cost of\n"
    "clock_gettime. All figures are ns per operation.\n"
    "\n"
    "  --iterations N  timed samples per uncontended row, and the samples kept\n"
    "                  for percentiles in the other rows (default 100000)\n"
    "  --warmup N      untimed iterations before each row (default 1000)\n"
    "  --batch N       operations per timed sample, uncontended rows (default 1)\n"
    "  --seconds S     duration of each contended and wake-up run (default 5)\n"
    "  --rate HZ       writer publish rate in those runs (default 500)\n"
    "  --poll-us US    NO_NOTIFY wake-up: reader poll interval (default 100)\n"
    "  --dir D         base directory, on tmpfs (default $PSMSGR_DIR, else /dev/shm)\n"
    "  --csv           CSV instead of a table\n"
    "  -h, --help\n";

typedef struct config {
    uint32_t iterations;
    uint32_t warmup;
    uint32_t batch;
    double seconds;
    uint32_t rate;
    uint32_t poll_us;
    const char *base;
    bool csv;
    char dir[512]; /* private channel directory under base */
} config;

static config cfg = {
    .iterations = 100000,
    .warmup = 1000,
    .batch = 1,
    .seconds = 5,
    .rate = 500,
    .poll_us = 100,
};

static pid_t main_pid;

static _Noreturn void die(const char *what, int rc)
{
    fprintf(stderr, "psmsgr-bench: %s: %s\n", what,
            rc == PSMSGR_E_SYS ? strerror(errno) : psmsgr_strerror(rc));
    if (getpid() != main_pid)
        _exit(1); /* a writer process: the channels are the parent's to clean up */
    exit(1);
}

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t)
{
    struct timespec ts = { .tv_sec = (time_t)(t / 1000000000u),
                           .tv_nsec = (long)(t % 1000000000u) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

/* ---- samples ------------------------------------------------------------------ */

/* Exact count, min and max; percentiles from a uniform reservoir of at most
 * `cap` samples (all of them, for the uncontended rows). */
typedef struct samples {
    uint64_t *v;
    size_t cap, n;
    uint64_t seen, min, max;
    uint64_t rng;
} samples;

static void samples_init(samples *s, size_t cap)
{
    *s = (samples){ .cap = cap, .min = UINT64_MAX, .rng = 0x9E3779B97F4A7C15u };
    s->v = calloc(cap, sizeof *s->v); /* checks cap * size, which wraps on 32-bit */
    if (s->v == NULL) {
        fputs("psmsgr-bench: out of memory\n", stderr);
        exit(1);
    }
}

static void samples_add(samples *s, uint64_t x)
{
    s->seen++;
    if (x < s->min)
        s->min = x;
    if (x > s->max)
        s->max = x;
    if (s->n < s->cap) {
        s->v[s->n++] = x;
        return;
    }
    s->rng ^= s->rng << 13; /* xorshift64 */
    s->rng ^= s->rng >> 7;
    s->rng ^= s->rng << 17;
    uint64_t j = s->rng % s->seen;
    if (j < s->cap)
        s->v[j] = x;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ---- output ------------------------------------------------------------------- */

static void print_table_header(void)
{
    if (cfg.csv)
        puts("test,size_bytes,n,min_ns,median_ns,p99_ns,max_ns,busy,missed");
    else
        printf("%-28s %8s %9s %9s %9s %9s %9s  %s\n", "test", "size", "n", "min", "median", "p99",
               "max", "notes");
}

/* One row; every value divided by `per` (operations per sample). `busy` and
 * `missed` < 0: not applicable. */
static void print_row(const char *test, uint32_t size, samples *s, uint32_t per, int64_t busy,
                      int64_t missed)
{
    double min = 0, med = 0, p99 = 0, max = 0;
    if (s->n > 0) {
        qsort(s->v, s->n, sizeof *s->v, cmp_u64);
        min = (double)s->min / per;
        med = (double)s->v[s->n / 2] / per;
        p99 = (double)s->v[s->n - 1 - s->n / 100] / per;
        max = (double)s->max / per;
    }
    char sz[16] = "-";
    if (size != 0)
        snprintf(sz, sizeof sz, "%" PRIu32, size);
    if (cfg.csv) {
        printf("%s,%s,%" PRIu64 ",%.0f,%.0f,%.0f,%.0f,", test, size != 0 ? sz : "", s->seen, min,
               med, p99, max);
        if (busy >= 0)
            printf("%" PRId64, busy);
        putchar(',');
        if (missed >= 0)
            printf("%" PRId64, missed);
        putchar('\n');
    } else {
        printf("%-28s %8s %9" PRIu64 " %9.0f %9.0f %9.0f %9.0f", test, sz, s->seen, min, med, p99,
               max);
        if (busy >= 0)
            printf("  busy %" PRId64, busy);
        if (missed >= 0)
            printf("  missed %" PRId64, missed);
        putchar('\n');
    }
    fflush(stdout);
}

static void read_line(const char *path, const char *prefix, char *out, size_t n)
{
    snprintf(out, n, "n/a");
    FILE *f = fopen(path, "re");
    if (f == NULL)
        return;
    char line[256];
    size_t plen = prefix != NULL ? strlen(prefix) : 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (plen != 0 && strncmp(line, prefix, plen) != 0)
            continue;
        const char *v = line + plen;
        if (plen != 0) {
            const char *colon = strchr(v, ':');
            if (colon == NULL)
                continue;
            v = colon + 1;
        }
        v += strspn(v, " \t");
        snprintf(out, n, "%.*s", (int)strcspn(v, "\n"), v);
        break;
    }
    fclose(f);
}

/* "driver/governor: state, state (disabled), ..." for cpu0. The wake-up rows
 * depend on the deep idle states' exit latency more than on anything else. */
static void cpuidle_info(char *out, size_t n)
{
    const char *base = "/sys/devices/system/cpu";
    char path[128], drv[64], gov[64], name[64], off[16];
    snprintf(path, sizeof path, "%s/cpuidle/current_driver", base);
    read_line(path, NULL, drv, sizeof drv);
    snprintf(path, sizeof path, "%s/cpuidle/current_governor", base);
    read_line(path, NULL, gov, sizeof gov);
    size_t len = (size_t)snprintf(out, n, "%s/%s", drv, gov);
    for (int i = 0; len < n; ++i) {
        snprintf(path, sizeof path, "%s/cpu0/cpuidle/state%d/name", base, i);
        read_line(path, NULL, name, sizeof name);
        if (strcmp(name, "n/a") == 0)
            break;
        snprintf(path, sizeof path, "%s/cpu0/cpuidle/state%d/disable", base, i);
        read_line(path, NULL, off, sizeof off);
        len += (size_t)snprintf(out + len, n - len, "%s %s%s", i == 0 ? ":" : ",", name,
                                strcmp(off, "1") == 0 ? " (disabled)" : "");
    }
}

static void print_system(void)
{
    struct utsname u;
    char kernel[256] = "n/a", cpu[128], hw[128], board[128], gov[64], freq[64], clk[64];
    if (uname(&u) == 0)
        snprintf(kernel, sizeof kernel, "%s %s %s", u.sysname, u.release, u.machine);
    read_line("/proc/cpuinfo", "model name", cpu, sizeof cpu);
    read_line("/proc/cpuinfo", "Hardware", hw, sizeof hw);
    read_line("/proc/device-tree/model", NULL, board, sizeof board);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", NULL, gov, sizeof gov);
    read_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", NULL, freq, sizeof freq);
    read_line("/sys/devices/system/clocksource/clocksource0/current_clocksource", NULL, clk,
              sizeof clk);
    if (strcmp(freq, "n/a") != 0) {
        long khz = strtol(freq, NULL, 10);
        snprintf(freq, sizeof freq, "%ld MHz", khz / 1000);
    }

    const char *fmt = cfg.csv ? "# %s: %s\n" : "%-12s %s\n";
    uint32_t v = psmsgr_version();
    char buf[160];
    snprintf(buf, sizeof buf, "%u.%u.%u", (unsigned)(v >> 16), (unsigned)((v >> 8) & 0xFF),
             (unsigned)(v & 0xFF));
    printf(fmt, "psmsgr", buf);
    printf(fmt, "kernel", kernel);
    printf(fmt, "cpu", cpu);
    printf(fmt, "hardware", hw);
    printf(fmt, "board", board);
    snprintf(buf, sizeof buf, "%ld online", sysconf(_SC_NPROCESSORS_ONLN));
    printf(fmt, "cpus", buf);
    snprintf(buf, sizeof buf, "%s, %s", gov, freq);
    printf(fmt, "governor", buf);
    printf(fmt, "clocksource", clk);
    cpuidle_info(buf, sizeof buf);
    printf(fmt, "cpuidle", buf);
    printf(fmt, "dir", cfg.dir);
    snprintf(buf, sizeof buf,
             "iterations %" PRIu32 ", warm-up %" PRIu32 ", batch %" PRIu32 ", %.1f s at %" PRIu32
             " Hz per run",
             cfg.iterations, cfg.warmup, cfg.batch, cfg.seconds, cfg.rate);
    printf(fmt, "settings", buf);
    if (!cfg.csv)
        putchar('\n');
}

/* ---- uncontended ------------------------------------------------------------- */

typedef struct op_ctx {
    psmsgr_state_writer *w;
    psmsgr_state_reader *r;
    unsigned char *buf;
    uint32_t size;
    unsigned char fill; /* byte for the fill rows, changed on every fill */
} op_ctx;

typedef void (*op_fn)(op_ctx *c);

static void op_nothing(op_ctx *c)
{
    (void)c;
    __asm__ volatile("" ::: "memory");
}

static void op_clock_gettime(op_ctx *c)
{
    (void)c;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    __asm__ volatile("" : : "r"(&ts) : "memory");
}

/* 32-bit arches with a 64-bit time_t have clock_gettime64. */
static void op_clock_syscall(op_ctx *c)
{
    (void)c;
    struct __kernel_timespec ts;
#ifdef SYS_clock_gettime64
    syscall(SYS_clock_gettime64, CLOCK_MONOTONIC, &ts);
#else
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
#endif
    __asm__ volatile("" : : "r"(&ts) : "memory");
}

static void op_getppid(op_ctx *c)
{
    (void)c;
    syscall(SYS_getppid);
}

static void op_now(op_ctx *c)
{
    (void)c;
    uint64_t t = psmsgr_now_ns();
    __asm__ volatile("" : : "r"(&t) : "memory");
}

static void op_publish(op_ctx *c)
{
    int rc = psmsgr_state_publish(c->w, c->buf, c->size, NULL);
    if (rc != PSMSGR_OK)
        die("publish", rc);
}

/* The producer writes the whole payload: into its own buffer and then
 * publish copies it, or in place between begin and commit. */
static void op_fill_publish(op_ctx *c)
{
    memset(c->buf, c->fill++, c->size);
    op_publish(c);
}

static void op_begin_fill_commit(op_ctx *c)
{
    void *p;
    int rc = psmsgr_state_begin(c->w, &p);
    if (rc != PSMSGR_OK)
        die("begin", rc);
    memset(p, c->fill++, c->size);
    rc = psmsgr_state_commit(c->w, c->size, NULL);
    if (rc != PSMSGR_OK)
        die("commit", rc);
}

static void op_begin_commit(op_ctx *c)
{
    void *p;
    int rc = psmsgr_state_begin(c->w, &p);
    if (rc != PSMSGR_OK)
        die("begin", rc);
    rc = psmsgr_state_commit(c->w, c->size, NULL);
    if (rc != PSMSGR_OK)
        die("commit", rc);
}

static void op_read(op_ctx *c)
{
    psmsgr_state_info info;
    int rc = psmsgr_state_read(c->r, c->buf, c->size, &info);
    if (rc != PSMSGR_OK)
        die("read", rc);
}

static void op_peek(op_ctx *c)
{
    psmsgr_state_info info;
    int rc = psmsgr_state_peek(c->r, &info);
    if (rc != PSMSGR_OK)
        die("peek", rc);
}

static void measure(const char *test, op_fn op, op_ctx *c, uint32_t batch, uint32_t n)
{
    for (uint32_t i = 0; i < cfg.warmup; ++i)
        op(c);
    samples s;
    samples_init(&s, n);
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t t0 = now_ns();
        for (uint32_t j = 0; j < batch; ++j)
            op(c);
        samples_add(&s, now_ns() - t0);
    }
    print_row(test, c->size, &s, batch, -1, -1);
    free(s.v);
}

static const char *chan_name(char *buf, size_t n, const char *what, uint32_t size)
{
    snprintf(buf, n, "%s-%" PRIu32, what, size);
    return buf;
}

static psmsgr_state_writer *open_writer(const char *name, uint32_t capacity, uint32_t flags)
{
    psmsgr_state_options o;
    psmsgr_state_options_init(&o);
    o.capacity = capacity;
    o.flags = flags;
    o.dir = cfg.dir;
    psmsgr_state_writer *w = NULL;
    int rc = psmsgr_state_writer_open(name, &o, &w);
    if (rc != PSMSGR_OK)
        die("writer_open", rc);
    return w;
}

static psmsgr_state_reader *open_reader(const char *name)
{
    psmsgr_state_reader *r = NULL;
    int rc = psmsgr_state_reader_open(name, cfg.dir, &r);
    if (rc != PSMSGR_OK)
        die("reader_open", rc);
    return r;
}

static void bench_clock(void)
{
    op_ctx c = { 0 };
    uint32_t n = cfg.iterations / CLOCK_BATCH > 100 ? cfg.iterations / CLOCK_BATCH : 100;
    measure("timer overhead", op_nothing, &c, cfg.batch, cfg.iterations);
    measure("clock_gettime", op_clock_gettime, &c, CLOCK_BATCH, n);
    measure("clock_gettime syscall", op_clock_syscall, &c, CLOCK_BATCH, n);
    measure("getppid syscall", op_getppid, &c, CLOCK_BATCH, n);
    measure("psmsgr_now_ns", op_now, &c, CLOCK_BATCH, n);
}

static void bench_uncontended(void)
{
    for (size_t i = 0; i < NSIZES; ++i) {
        uint32_t size = sizes[i];
        char name[64];
        op_ctx c = { .size = size, .buf = calloc(1, size) };
        if (c.buf == NULL)
            die("calloc", PSMSGR_E_SYS);

        c.w = open_writer(chan_name(name, sizeof name, "nonotify", size), size,
                          PSMSGR_STATE_NO_NOTIFY);
        measure("publish NO_NOTIFY", op_publish, &c, cfg.batch, cfg.iterations);
        measure("fill/publish NO_NOTIFY", op_fill_publish, &c, cfg.batch, cfg.iterations);
        measure("begin/fill/commit NO_NOTIFY", op_begin_fill_commit, &c, cfg.batch, cfg.iterations);
        measure("begin/commit NO_NOTIFY", op_begin_commit, &c, cfg.batch, cfg.iterations);
        psmsgr_state_writer_close(c.w);

        c.w = open_writer(chan_name(name, sizeof name, "plain", size), size, 0);
        c.r = open_reader(name);
        measure("publish", op_publish, &c, cfg.batch, cfg.iterations);
        measure("read", op_read, &c, cfg.batch, cfg.iterations);
        measure("peek", op_peek, &c, cfg.batch, cfg.iterations);
        psmsgr_state_reader_close(c.r);
        psmsgr_state_writer_close(c.w);
        free(c.buf);
    }
}

/* ---- writer process ----------------------------------------------------------- */

typedef struct writer_proc {
    pid_t pid;
    int fd; /* reads "ready", then the publish count */
} writer_proc;

/* Publishes at cfg.rate for cfg.seconds in a child process. With `stamp`,
 * the payload starts with psmsgr_now_ns() taken just before the publish. */
static writer_proc writer_start(const char *name, uint32_t size, uint32_t flags, bool stamp)
{
    int p[2];
    if (pipe(p) != 0)
        die("pipe", PSMSGR_E_SYS);
    fflush(NULL);
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0)
        die("fork", PSMSGR_E_SYS);
    if (pid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(1);
        close(p[0]);
        psmsgr_state_writer *w = open_writer(name, size, flags);
        unsigned char *buf = calloc(1, size);
        if (buf == NULL)
            _exit(1);
        uint64_t count = 0;
        if (psmsgr_state_publish(w, buf, size, NULL) != PSMSGR_OK ||
            write(p[1], &count, sizeof count) != (ssize_t)sizeof count)
            _exit(1);
        uint64_t period = 1000000000u / cfg.rate;
        uint64_t start = now_ns();
        uint64_t end = start + (uint64_t)(cfg.seconds * 1e9);
        for (uint64_t next = start + period; next < end; next += period) {
            sleep_until(next);
            if (stamp) {
                uint64_t t = psmsgr_now_ns();
                memcpy(buf, &t, sizeof t);
            }
            if (psmsgr_state_publish(w, buf, size, NULL) != PSMSGR_OK)
                _exit(1);
            ++count;
        }
        psmsgr_state_writer_close(w);
        _exit(write(p[1], &count, sizeof count) == (ssize_t)sizeof count ? 0 : 1);
    }
    close(p[1]);
    writer_proc wp = { pid, p[0] };
    uint64_t ready;
    if (read(wp.fd, &ready, sizeof ready) != (ssize_t)sizeof ready) {
        fputs("psmsgr-bench: writer process failed to start\n", stderr);
        exit(1);
    }
    return wp;
}

/* Waits for the writer to finish; returns its publish count. */
static uint64_t writer_finish(writer_proc *wp)
{
    uint64_t count = 0;
    ssize_t got;
    while ((got = read(wp->fd, &count, sizeof count)) < 0 && errno == EINTR) {
    }
    close(wp->fd);
    int status = 0;
    while (waitpid(wp->pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (got != (ssize_t)sizeof count || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fputs("psmsgr-bench: writer process failed\n", stderr);
        exit(1);
    }
    return count;
}

/* ---- contended --------------------------------------------------------------- */

/* A reader polling as fast as it can against a writer at a fixed rate. */
static void bench_contended(void)
{
    for (size_t i = 0; i < NSIZES; ++i) {
        uint32_t size = sizes[i];
        char name[64];
        chan_name(name, sizeof name, "contended", size);
        unsigned char *buf = malloc(size);
        if (buf == NULL)
            die("malloc", PSMSGR_E_SYS);
        writer_proc wp = writer_start(name, size, 0, false);
        psmsgr_state_reader *r = open_reader(name);

        samples s;
        samples_init(&s, cfg.iterations);
        int64_t busy = 0;
        uint64_t end = now_ns() + (uint64_t)(cfg.seconds * 1e9);
        for (uint64_t t0 = now_ns(); t0 < end;) {
            psmsgr_state_info info;
            int rc = psmsgr_state_read(r, buf, size, &info);
            uint64_t t1 = now_ns();
            if (rc == PSMSGR_OK)
                samples_add(&s, t1 - t0);
            else if (rc == PSMSGR_E_BUSY)
                ++busy;
            else
                die("read", rc);
            t0 = t1;
        }
        (void)writer_finish(&wp);
        print_row("read (writer active)", size, &s, 1, busy, -1);
        free(s.v);
        psmsgr_state_reader_close(r);
        free(buf);
    }
}

/* ---- wake-up ----------------------------------------------------------------- */

/* Publish-to-reader latency: the writer stamps psmsgr_now_ns() into the
 * payload; the reader subtracts it once it has the value. With notify the
 * reader blocks in wait; with NO_NOTIFY it peeks every cfg.poll_us. */
static void bench_wake(bool notify)
{
    const uint32_t size = 16;
    char name[64];
    chan_name(name, sizeof name, notify ? "wake" : "poll", size);
    writer_proc wp = writer_start(name, size, notify ? 0 : PSMSGR_STATE_NO_NOTIFY, true);
    psmsgr_state_reader *r = open_reader(name);

    samples s;
    samples_init(&s, cfg.iterations);
    int64_t missed = 0;
    uint32_t seen = 0;
    unsigned char buf[16];
    psmsgr_state_info info;
    int rc = psmsgr_state_read(r, buf, size, &info); /* the writer's initial value */
    if (rc != PSMSGR_OK)
        die("read", rc);
    seen = info.generation;
    uint64_t end = now_ns() + (uint64_t)(cfg.seconds * 1e9);
    struct timespec poll = { .tv_sec = cfg.poll_us / 1000000u,
                             .tv_nsec = (long)(cfg.poll_us % 1000000u) * 1000 };
    while (now_ns() < end) {
        if (notify) {
            rc = psmsgr_state_wait(r, seen, 100);
            if (rc == PSMSGR_E_TIMEOUT)
                continue;
            if (rc != PSMSGR_OK)
                die("wait", rc);
        } else {
            rc = psmsgr_state_peek(r, &info);
            if (rc != PSMSGR_OK && rc != PSMSGR_E_BUSY)
                die("peek", rc);
            if (rc != PSMSGR_OK || info.generation == seen) {
                (void)clock_nanosleep(CLOCK_MONOTONIC, 0, &poll, NULL);
                continue;
            }
        }
        rc = psmsgr_state_read(r, buf, size, &info);
        uint64_t t = psmsgr_now_ns();
        if (rc == PSMSGR_E_BUSY)
            continue;
        if (rc != PSMSGR_OK)
            die("read", rc);
        uint64_t stamp;
        memcpy(&stamp, buf, sizeof stamp);
        samples_add(&s, t - stamp);
        missed += (int64_t)(uint32_t)(info.generation - seen - 1);
        seen = info.generation;
    }
    (void)writer_finish(&wp);
    char test[64];
    if (notify)
        snprintf(test, sizeof test, "wait wake-up");
    else
        snprintf(test, sizeof test, "poll wake-up (%" PRIu32 " us)", cfg.poll_us);
    print_row(test, size, &s, 1, -1, missed);
    free(s.v);
    psmsgr_state_reader_close(r);
}

/* ---- main --------------------------------------------------------------------- */

static void cleanup(void)
{
    static const char *const kinds[] = { "nonotify", "plain", "contended" };
    char name[64];
    for (size_t k = 0; k < sizeof kinds / sizeof *kinds; ++k)
        for (size_t i = 0; i < NSIZES; ++i)
            (void)psmsgr_state_unlink(chan_name(name, sizeof name, kinds[k], sizes[i]), cfg.dir);
    (void)psmsgr_state_unlink(chan_name(name, sizeof name, "wake", 16), cfg.dir);
    (void)psmsgr_state_unlink(chan_name(name, sizeof name, "poll", 16), cfg.dir);
    (void)rmdir(cfg.dir);
}

static bool parse_u32(const char *s, uint32_t min, uint32_t *out)
{
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < min || v > UINT32_MAX)
        return false;
    *out = (uint32_t)v;
    return true;
}

static int usage_error(const char *msg)
{
    fprintf(stderr, "psmsgr-bench: %s\n%s", msg, usage_text);
    return 2;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "iterations", required_argument, NULL, 'i' },
        { "warmup", required_argument, NULL, 'w' },
        { "batch", required_argument, NULL, 'b' },
        { "seconds", required_argument, NULL, 's' },
        { "rate", required_argument, NULL, 'r' },
        { "poll-us", required_argument, NULL, 'p' },
        { "dir", required_argument, NULL, 'd' },
        { "csv", no_argument, NULL, 'c' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        bool ok = true;
        switch (c) {
        case 'i': ok = parse_u32(optarg, 1, &cfg.iterations); break;
        case 'w': ok = parse_u32(optarg, 0, &cfg.warmup); break;
        case 'b': ok = parse_u32(optarg, 1, &cfg.batch); break;
        case 'r': ok = parse_u32(optarg, 1, &cfg.rate) && cfg.rate <= 1000000; break;
        case 'p': ok = parse_u32(optarg, 0, &cfg.poll_us); break;
        case 'd': cfg.base = optarg; break;
        case 'c': cfg.csv = true; break;
        case 's': {
            char *end;
            cfg.seconds = strtod(optarg, &end);
            ok = end != optarg && *end == '\0' && cfg.seconds > 0 && cfg.seconds <= 3600;
            break;
        }
        case 'h': fputs(usage_text, stdout); return 0;
        default:  return 2;
        }
        if (!ok)
            return usage_error("invalid option value");
    }
    if (optind != argc)
        return usage_error("unexpected argument");

    if (cfg.base == NULL) {
        cfg.base = getenv("PSMSGR_DIR");
        if (cfg.base == NULL || cfg.base[0] == '\0')
            cfg.base = "/dev/shm";
    }
    snprintf(cfg.dir, sizeof cfg.dir, "%s/psmsgr-bench-XXXXXX", cfg.base);
    if (mkdtemp(cfg.dir) == NULL) {
        fprintf(stderr, "psmsgr-bench: mkdtemp %s: %s\n", cfg.dir, strerror(errno));
        return 1;
    }
    main_pid = getpid();
    atexit(cleanup);

    print_system();
    print_table_header();
    bench_clock();
    bench_uncontended();
    bench_contended();
    bench_wake(true);
    bench_wake(false);
    return 0;
}
