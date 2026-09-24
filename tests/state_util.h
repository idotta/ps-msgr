/* SPDX-License-Identifier: Apache-2.0 */
/* Helpers for the state channel tests: a private channel directory per test,
 * raw access to channel files, and child processes. */
#ifndef PSMSGR_STATE_UTIL_H
#define PSMSGR_STATE_UTIL_H

#include <psmsgr/state.h>

#include "layout.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHAN "chan"

static char test_dir[512];

static inline int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb, (void)flag, (void)ftw;
    return remove(path);
}

/* Fixture: every test gets a fresh channel directory. Teardown also runs
 * after a failed assertion. */
static inline int make_test_dir(void **state)
{
    (void)state;
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    snprintf(test_dir, sizeof test_dir, "%s/psmsgr-test-XXXXXX", base);
    if (mkdtemp(test_dir) == NULL) {
        perror("mkdtemp");
        return -1;
    }
    return 0;
}

static inline int remove_test_dir(void **state)
{
    (void)state;
    (void)nftw(test_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    unsetenv("PSMSGR_DIR"); /* set by some tests; a failed one skips its own unsetenv */
    return 0;
}

#define TEST(test_fn) cmocka_unit_test_setup_teardown(test_fn, make_test_dir, remove_test_dir)

/* <test_dir>/psmsgr.<name><suffix>, in one of a few rotating buffers. */
static inline const char *chan_path(const char *name, const char *suffix)
{
    static char bufs[4][640];
    static unsigned next;
    char *b = bufs[next++ % 4];
    snprintf(b, sizeof bufs[0], "%s/psmsgr.%s%s", test_dir, name, suffix);
    return b;
}

static inline const char *data_path(const char *name)
{
    return chan_path(name, ".state");
}
static inline const char *lock_path(const char *name)
{
    return chan_path(name, ".lock");
}

static inline psmsgr_state_options make_opts(uint32_t capacity, uint32_t slots, uint32_t flags)
{
    psmsgr_state_options o;
    psmsgr_state_options_init(&o);
    o.capacity = capacity;
    o.slot_count = slots;
    o.flags = flags;
    o.dir = test_dir;
    return o;
}

static inline int open_writer(const char *name, uint32_t capacity, uint32_t slots, uint32_t flags,
                              psmsgr_state_writer **w)
{
    psmsgr_state_options o = make_opts(capacity, slots, flags);
    return psmsgr_state_writer_open(name, &o, w);
}

static inline psmsgr_state_reader *open_reader(const char *name)
{
    psmsgr_state_reader *r = NULL;
    if (psmsgr_state_reader_open(name, test_dir, &r) != PSMSGR_OK)
        return NULL;
    return r;
}

static inline int publish_str(psmsgr_state_writer *w, const char *s, uint32_t *gen)
{
    return psmsgr_state_publish(w, s, (uint32_t)strlen(s), gen);
}

/* The generation n publishes after `gen`. A new file starts at a random one,
 * so tests compare relative to the first; this wraps like the writer. */
static inline uint32_t gen_after(uint32_t gen, uint32_t n)
{
    while (n-- > 0)
        gen = gen == UINT32_MAX ? 1 : gen + 1;
    return gen;
}

/* Reads the latest value as a string; returns the read's result code. */
static inline int read_str(psmsgr_state_reader *r, char *buf, uint32_t size,
                           psmsgr_state_info *info)
{
    int rc = psmsgr_state_read(r, buf, size - 1, info);
    buf[rc == PSMSGR_OK ? info->length : 0] = '\0';
    return rc;
}

/* ---- raw file access ------------------------------------------------------ */

static inline int raw_read(const char *path, void *buf, size_t n, off_t off)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t got = pread(fd, buf, n, off);
    close(fd);
    return got == (ssize_t)n ? 0 : -1;
}

static inline int raw_write(const char *path, const void *buf, size_t n, off_t off)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t put = pwrite(fd, buf, n, off);
    close(fd);
    return put == (ssize_t)n ? 0 : -1;
}

static inline int raw_header(const char *name, psmi_header *h)
{
    return raw_read(data_path(name), h, sizeof *h, 0);
}

static inline uint32_t raw_slot_seq(const char *name, uint32_t slot)
{
    psmi_header h;
    uint32_t seq = 0xDEADBEEF;
    if (raw_header(name, &h) == 0)
        (void)raw_read(data_path(name), &seq, sizeof seq,
                       (off_t)(PSMI_HEADER_SIZE + (uint64_t)slot * h.slot_stride));
    return seq;
}

static inline ino_t inode_of(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 ? st.st_ino : 0;
}

/* ---- time ------------------------------------------------------------------- */

static inline void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static inline uint64_t elapsed_ms(uint64_t since_ns)
{
    return (psmsgr_now_ns() - since_ns) / 1000000u;
}

/* ---- child processes ---------------------------------------------------------- */

typedef struct child {
    pid_t pid;
    int fd; /* read end: the child's result */
} child;

/* Runs fn in a child, which reports its result and then waits to be killed.
 * fn must not use cmocka assertions. A child that outlives a failed test is
 * killed when the test program exits, so it cannot keep ctest waiting. */
static inline child child_start(int (*fn)(void))
{
    int p[2];
    child c = { -1, -1 };
    if (pipe(p) != 0)
        return c;
    fflush(NULL);
    pid_t parent = getpid();
    c.pid = fork();
    if (c.pid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(3);
        close(p[0]);
        int rc = fn();
        if (write(p[1], &rc, sizeof rc) != (ssize_t)sizeof rc)
            _exit(3);
        for (;;)
            pause();
    }
    close(p[1]);
    c.fd = p[0];
    return c;
}

static inline int child_result(const child *c)
{
    int rc;
    ssize_t n;
    while ((n = read(c->fd, &rc, sizeof rc)) < 0 && errno == EINTR) {
    }
    return n == (ssize_t)sizeof rc ? rc : INT32_MIN;
}

/* SIGKILL: the kernel releases the child's locks, as after a crash. */
static inline void child_kill(child *c)
{
    if (c->pid > 0) {
        kill(c->pid, SIGKILL);
        while (waitpid(c->pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
    if (c->fd >= 0)
        close(c->fd);
    c->pid = -1;
    c->fd = -1;
}

#endif /* PSMSGR_STATE_UTIL_H */
