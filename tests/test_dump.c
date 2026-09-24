/* SPDX-License-Identifier: Apache-2.0 */
/* psmsgr-dump, run as a child process against channels in the test's
 * directory. The command to run it (argv[1..], possibly behind an emulator)
 * comes from CTest. */
#include "state_util.h"

#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>

static char **tool_argv;
static int tool_argc;

typedef struct proc {
    pid_t pid;
    int fd; /* read end of the child's stdout and stderr */
    size_t len;
    char out[65536];
} proc;

static proc run; /* static: too large for the stack under the sanitizers */

/* Starts the tool with the test's --dir and the given NULL-terminated args. */
static void proc_start(proc *p, ...)
{
    const char *argv[32];
    int n = 0;
    for (int i = 0; i < tool_argc; ++i)
        argv[n++] = tool_argv[i];
    argv[n++] = "--dir";
    argv[n++] = test_dir;
    va_list ap;
    va_start(ap, p);
    for (const char *a; (a = va_arg(ap, const char *)) != NULL && n < 31;)
        argv[n++] = a;
    va_end(ap);
    argv[n] = NULL;

    int pipefd[2];
    assert_int_equal(pipe(pipefd), 0);
    fflush(NULL);
    p->pid = fork();
    assert_int_not_equal(p->pid, -1);
    if (p->pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)(uintptr_t)argv);
        _exit(127);
    }
    close(pipefd[1]);
    p->fd = pipefd[0];
    p->len = 0;
    p->out[0] = '\0';
}

/* Reads output until `needle` appears (true), EOF or the timeout (false).
 * NULL reads to EOF. */
static bool proc_read_until(proc *p, const char *needle, int timeout_ms)
{
    uint64_t start = psmsgr_now_ns();
    while (needle == NULL || strstr(p->out, needle) == NULL) {
        int left = timeout_ms - (int)elapsed_ms(start);
        if (left <= 0)
            return false;
        struct pollfd pfd = { .fd = p->fd, .events = POLLIN };
        if (poll(&pfd, 1, left) <= 0)
            continue;
        if (p->len >= sizeof p->out - 1)
            return false;
        ssize_t got = read(p->fd, p->out + p->len, sizeof p->out - 1 - p->len);
        if (got <= 0)
            return needle == NULL;
        p->len += (size_t)got;
        p->out[p->len] = '\0';
    }
    return true;
}

/* Reads the rest of the output and reaps the child: its exit status, or
 * 128 + signal. */
static int proc_wait(proc *p)
{
    assert_true(proc_read_until(p, NULL, 60000));
    close(p->fd);
    int status;
    while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

#define dump(...) (proc_start(&run, __VA_ARGS__, (const char *)NULL), proc_wait(&run))

#define assert_output(needle)                                                    \
    do {                                                                         \
        if (strstr(run.out, (needle)) == NULL) {                                 \
            cmocka_print_error("output lacks \"%s\":\n%s\n", (needle), run.out); \
            fail();                                                              \
        }                                                                        \
    } while (0)

/* ---- tests ------------------------------------------------------------------ */

static void usage_errors_exit_2(void **state)
{
    assert_int_equal(dump("--help"), 0);
    assert_output("usage: psmsgr-dump");
    assert_output("may be torn");
    assert_int_equal(dump("--bogus", CHAN), 2);
    assert_int_equal(dump(CHAN, "other"), 2);
    assert_int_equal(dump("a/b"), 2);
    assert_output("invalid channel name");
    assert_int_equal(dump(".hidden"), 2);
    /* No name at all: only the --dir the helper adds. */
    assert_int_equal(dump("--watch"), 2);
    assert_output("missing channel name");
}

static void missing_channel_exits_1(void **state)
{
    assert_int_equal(dump(CHAN), 1);
    assert_output("channel not found");
    assert_output("writer        not running");
}

static void never_published(void **state)
{
    psmsgr_state_writer *w = NULL;
    psmsgr_state_options o = make_opts(64, 3, 0);
    o.payload_type = 0x1234;
    assert_rc(psmsgr_state_writer_open(CHAN, &o, &w), PSMSGR_OK);

    assert_int_equal(dump(CHAN), 0);
    assert_output("magic         0x534d5350 \"PSMS\"");
    assert_output("version       1.0");
    assert_output("slots         3 x 128 B stride");
    assert_output("capacity      64");
    assert_output("payload_type  0x00001234 (4660)");
    assert_output("config_flags  0x00000000\n");
    assert_output("state         0x00000000\n");
    char pid[64];
    snprintf(pid, sizeof pid, "writer_pid    %d\n", (int)getpid());
    assert_output(pid);
    assert_output("latest        none (nothing published)");
    assert_output("writer        alive");
    assert_output("value         no data");
    assert_null(strstr(run.out, "<- latest"));
    psmsgr_state_writer_close(w);
}

static void published(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 3, PSMSGR_STATE_NO_NOTIFY, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "first", NULL), PSMSGR_OK);
    assert_rc(publish_str(w, "hello", &gen), PSMSGR_OK);
    psmsgr_state_writer_close(w);
    char value[64];
    snprintf(value, sizeof value, "value         generation %" PRIu32 ", length 5, age ", gen);

    assert_int_equal(dump(CHAN), 0);
    assert_output("config_flags  0x00000001 NO_NOTIFY");
    assert_output("latest        0x");
    assert_output("<- latest");
    assert_output(value);
    assert_output("writer        not running");
    assert_null(strstr(run.out, "STALE"));
    assert_null(strstr(run.out, "payload       "));
}

static void retired_channel(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 2, 0, &w), PSMSGR_OK);
    assert_rc(publish_str(w, "hello", NULL), PSMSGR_OK);
    psmsgr_state_writer_close(w);
    uint32_t st = PSMI_STATE_RETIRED;
    assert_int_equal(
        raw_write(data_path(CHAN), &st, sizeof st, (off_t)offsetof(psmi_header, state)), 0);

    assert_int_equal(dump(CHAN), 0);
    assert_output("state         0x00000001 RETIRED");
    assert_output("<- latest");
    assert_output("value         no data"); /* readers treat a retired file as absent */
}

static void corrupt_header_exits_1(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 2, 0, &w), PSMSGR_OK);
    assert_rc(publish_str(w, "hello", NULL), PSMSGR_OK);
    psmsgr_state_writer_close(w);
    psmi_header good;
    assert_int_equal(raw_header(CHAN, &good), 0);

    static const struct {
        size_t off;
        uint32_t value;
        const char *message;
    } cases[] = {
        { offsetof(psmi_header, magic), 0x12345678, "bad magic" },
        { offsetof(psmi_header, version_major), 2, "unsupported major version" },
        { offsetof(psmi_header, header_size), 64, "bad header_size" },
        { offsetof(psmi_header, slot_count), 99, "slot_count out of range" },
        { offsetof(psmi_header, slot_count), 1, "slot_count out of range" },
        { offsetof(psmi_header, capacity), UINT32_MAX, "capacity out of range" },
        { offsetof(psmi_header, slot_stride), 65, "slot_stride does not match capacity" },
        { offsetof(psmi_header, slot_count), 16, "file shorter than its slots" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        assert_int_equal(raw_write(data_path(CHAN), &good, sizeof good, 0), 0);
        size_t n = cases[i].off == offsetof(psmi_header, version_major) ? 2 : 4;
        assert_int_equal(raw_write(data_path(CHAN), &cases[i].value, n, (off_t)cases[i].off), 0);
        assert_int_equal(dump(CHAN), 1);
        assert_output("invalid channel file");
        assert_output(cases[i].message);
    }

    /* Too short for a header; and not a regular file. */
    assert_int_equal(truncate(data_path(CHAN), 100), 0);
    assert_int_equal(dump(CHAN), 1);
    assert_output("shorter than a header");
    assert_int_equal(unlink(data_path(CHAN)), 0);
    assert_int_equal(mkdir(data_path(CHAN), 0700), 0);
    assert_int_equal(dump(CHAN), 1);
    assert_output("not a regular file");
    assert_int_equal(rmdir(data_path(CHAN)), 0);

    /* Random bytes. */
    unsigned char junk[4096];
    srand(1);
    for (int round = 0; round < 8; ++round) {
        for (size_t i = 0; i < sizeof junk; ++i)
            junk[i] = (unsigned char)rand();
        int fd = open(data_path(CHAN), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        assert_true(fd >= 0);
        assert_int_equal(write(fd, junk, sizeof junk), (ssize_t)sizeof junk);
        close(fd);
        assert_int_equal(dump(CHAN), 1);
        assert_output("invalid channel file");
    }
}

static void stale_latest_and_odd_seq_are_flagged(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 3, 0, &w), PSMSGR_OK);
    assert_rc(publish_str(w, "hello", NULL), PSMSGR_OK);
    psmsgr_state_writer_close(w);

    psmi_header h;
    assert_int_equal(raw_header(CHAN, &h), 0);
    uint32_t slot = psmi_latest_slot(h.latest);
    uint32_t stale = h.latest + (1u << 4); /* next tag, same slot */
    uint32_t odd = 3;
    uint32_t other = (slot + 1) % h.slot_count;
    assert_int_equal(
        raw_write(data_path(CHAN), &stale, sizeof stale, (off_t)offsetof(psmi_header, latest)), 0);
    assert_int_equal(raw_write(data_path(CHAN), &odd, sizeof odd,
                               (off_t)(PSMI_HEADER_SIZE + (uint64_t)other * h.slot_stride)),
                     0);

    assert_int_equal(dump(CHAN), 0);
    assert_output("STALE: slot seq 2 (tag 1); readers get BUSY");
    assert_output("odd: being written, or aborted/crashed");
    assert_output("value         channel busy");

    uint32_t bad = 5; /* slot 5 of 3 */
    assert_int_equal(
        raw_write(data_path(CHAN), &bad, sizeof bad, (off_t)offsetof(psmi_header, latest)), 0);
    assert_int_equal(dump(CHAN), 1);
    assert_output("INVALID: readers get FORMAT");
    assert_output("value         invalid channel format");
}

static void oversized_latest_length_is_invalid(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 3, 0, &w), PSMSGR_OK);
    assert_rc(publish_str(w, "hello", NULL), PSMSGR_OK);
    psmsgr_state_writer_close(w);

    psmi_header h;
    assert_int_equal(raw_header(CHAN, &h), 0);
    uint32_t length = h.capacity + 1;
    off_t slot = (off_t)(PSMI_HEADER_SIZE + (uint64_t)psmi_latest_slot(h.latest) * h.slot_stride);
    assert_int_equal(raw_write(data_path(CHAN), &length, sizeof length,
                               slot + (off_t)offsetof(psmi_slot, length)),
                     0);

    assert_int_equal(dump(CHAN), 1);
    assert_output("INVALID: length > capacity; readers get FORMAT");
    assert_output("length > capacity  <- latest");
    assert_output("value         invalid channel format");
}

static void hex_dumps_the_latest_payload(void **state)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 2000, 2, 0, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "hello, psmsgr!", &gen), PSMSGR_OK);

    char payload[64];
    snprintf(payload, sizeof payload, "payload       generation %" PRIu32 ", 14 bytes", gen);
    assert_int_equal(dump(CHAN, "--hex"), 0);
    assert_output(payload);
    assert_output("  00000000  68 65 6c 6c 6f 2c 20 70  73 6d 73 67 72 21        |hello, psmsgr!|");
    assert_null(strstr(run.out, "more bytes"));

    /* Capped at 1 KiB. */
    static unsigned char big[2000];
    memset(big, 'x', sizeof big);
    assert_rc(psmsgr_state_publish(w, big, sizeof big, &gen), PSMSGR_OK);
    snprintf(payload, sizeof payload, "payload       generation %" PRIu32 ", 2000 bytes", gen);
    assert_int_equal(dump(CHAN, "--hex"), 0);
    assert_output(payload);
    assert_output("  000003f0  78 78");
    assert_null(strstr(run.out, "  00000400  "));
    assert_output("  ... 976 more bytes");

    /* Nothing published on a zero-capacity channel. */
    psmsgr_state_writer_close(w);
    assert_rc(open_writer("empty", 0, 2, 0, &w), PSMSGR_OK);
    assert_int_equal(dump("empty", "--hex"), 0);
    assert_output("payload       no data");
    assert_rc(psmsgr_state_publish(w, NULL, 0, &gen), PSMSGR_OK);
    snprintf(payload, sizeof payload, "payload       generation %" PRIu32 ", 0 bytes", gen);
    assert_int_equal(dump("empty", "--hex"), 0);
    assert_output(payload);
    psmsgr_state_writer_close(w);
}

static void watch_redraws_on_change(uint32_t flags)
{
    psmsgr_state_writer *w = NULL;
    assert_rc(open_writer(CHAN, 64, 3, flags, &w), PSMSGR_OK);
    uint32_t gen;
    assert_rc(publish_str(w, "one", &gen), PSMSGR_OK);

    char want[2][64];
    snprintf(want[0], sizeof want[0], "value         generation %" PRIu32 ",", gen);
    snprintf(want[1], sizeof want[1], "value         generation %" PRIu32 ",", gen_after(gen, 1));
    proc_start(&run, CHAN, "--watch", (const char *)NULL);
    bool first = proc_read_until(&run, want[0], 30000);
    if (first)
        assert_rc(publish_str(w, "two", NULL), PSMSGR_OK);
    bool second = first && proc_read_until(&run, want[1], 30000);
    kill(run.pid, SIGINT);
    int status = proc_wait(&run);
    psmsgr_state_writer_close(w);
    if (!second)
        cmocka_print_error("output:\n%s\n", run.out);
    assert_true(second);
    assert_int_equal(status, 0);
}

static void watch_with_notify(void **state)
{
    watch_redraws_on_change(0);
}

static void watch_polls_without_notify(void **state)
{
    watch_redraws_on_change(PSMSGR_STATE_NO_NOTIFY);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command to run psmsgr-dump>...\n", argv[0]);
        return 2;
    }
    tool_argv = argv + 1;
    tool_argc = argc - 1;

    const struct CMUnitTest tests[] = {
        TEST(usage_errors_exit_2),
        TEST(missing_channel_exits_1),
        TEST(never_published),
        TEST(published),
        TEST(retired_channel),
        TEST(corrupt_header_exits_1),
        TEST(stale_latest_and_odd_seq_are_flagged),
        TEST(oversized_latest_length_is_invalid),
        TEST(hex_dumps_the_latest_payload),
        TEST(watch_with_notify),
        TEST(watch_polls_without_notify),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
