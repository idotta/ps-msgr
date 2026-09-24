/* SPDX-License-Identifier: Apache-2.0 */
/* Seqlock torture test (spec/build-and-test.md, "Torture (C)").
 *
 * One writer publishes as fast as it can while reader threads and reader
 * processes, each with its own handle and therefore its own mapping, read for
 * a fixed time and validate every value. TSan cannot see the seqlock race
 * (state-channel.md §5.4), so this is the test that checks it.
 *
 * PSMSGR_TORTURE_SECONDS sets the time per variant (default 1; fractions are
 * fine). Links the static library for the test hooks: the writer starts just
 * below the generation wrap, and the last test breaks the seqlock on purpose
 * to prove that torn reads are detected. */
#include "internal.h"
#include "state_util.h"

#include <inttypes.h>
#include <pthread.h>

#define READER_THREADS 2
#define READER_PROCS   2
#define MAX_READERS    (READER_THREADS + READER_PROCS)

/* Payload: {generation, length, bytes derived from generation, CRC32}. A value
 * shorter than the frame is only the derived bytes. */
#define FRAME_HEAD 8u
#define FRAME_SIZE (FRAME_HEAD + 4u)

static uint64_t duration_ns = 1000000000u;

/* ---- CRC32 (IEEE, reflected) --------------------------------------------- */

static uint32_t crc_table[256];

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

static uint32_t crc32(const unsigned char *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return ~c;
}

/* ---- payloads ---------------------------------------------------------------- */

/* A byte stream seeded by the generation: every generation's bytes differ. */
static void fill_derived(unsigned char *p, uint32_t gen, uint32_t len)
{
    uint32_t v = gen * 2654435761u;
    for (uint32_t k = 0; k < len; ++k) {
        v = v * 1103515245u + 12345u;
        p[k] = (unsigned char)(v >> 24);
    }
}

static void make_payload(unsigned char *p, uint32_t gen, uint32_t len)
{
    fill_derived(p, gen, len);
    if (len >= FRAME_SIZE) {
        memcpy(p, &gen, 4);
        memcpy(p + 4, &len, 4);
        uint32_t crc = crc32(p, len - 4);
        memcpy(p + len - 4, &crc, 4);
    }
}

static uint32_t next_generation(uint32_t g)
{
    return g == UINT32_MAX ? 1 : g + 1;
}

static uint64_t rand_next(uint64_t *s) /* xorshift64* */
{
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * UINT64_C(2685821657736338717);
}

/* ---- readers ------------------------------------------------------------------ */

typedef struct reader_stats {
    uint64_t reads;     /* successful reads, each validated */
    uint64_t peeks;     /* successful peeks, checked for order only */
    uint64_t busy;      /* PSMSGR_E_BUSY */
    uint64_t bad_data;  /* CRC (or derived bytes) wrong */
    uint64_t bad_gen;   /* embedded generation != info.generation, or 0 */
    uint64_t bad_len;   /* embedded length != info.length */
    uint64_t bad_order; /* generation or timestamp went backwards */
    uint64_t bad_rc;    /* any other result, or NODATA after a value */
    int last_bad_rc;
} reader_stats;

typedef struct run_cfg {
    uint32_t slots;
    uint32_t capacity;
    unsigned threads;
    unsigned procs;
    uint64_t deadline_ns;
} run_cfg;

static void check_value(reader_stats *st, const unsigned char *p, const psmsgr_state_info *info)
{
    uint32_t len = info->length;
    if (len < FRAME_SIZE) {
        unsigned char expect[FRAME_SIZE];
        fill_derived(expect, info->generation, len);
        if (memcmp(p, expect, len) != 0)
            ++st->bad_data;
        return;
    }
    uint32_t gen, embedded_len, crc;
    memcpy(&gen, p, 4);
    memcpy(&embedded_len, p + 4, 4);
    memcpy(&crc, p + len - 4, 4);
    if (crc32(p, len - 4) != crc)
        ++st->bad_data;
    else if (gen != info->generation)
        ++st->bad_gen;
    else if (embedded_len != len)
        ++st->bad_len;
}

/* Reads (and every eighth time peeks) until the deadline. Before the first
 * value NODATA is expected: the reader may start before the writer. */
static void reader_loop(const run_cfg *cfg, reader_stats *st)
{
    memset(st, 0, sizeof *st);
    unsigned char *buf = malloc(cfg->capacity);
    psmsgr_state_reader *r = open_reader(CHAN);
    if (buf == NULL || r == NULL) {
        st->bad_rc = 1;
        st->last_bad_rc = PSMSGR_E_SYS;
        free(buf);
        return;
    }
    bool seen = false;
    uint32_t prev_gen = 0;
    uint64_t prev_ts = 0, ops = 0;
    while (psmsgr_now_ns() < cfg->deadline_ns) {
        psmsgr_state_info info;
        bool peek = (++ops & 7u) == 0;
        int rc =
            peek ? psmsgr_state_peek(r, &info) : psmsgr_state_read(r, buf, cfg->capacity, &info);
        if (rc == PSMSGR_E_BUSY) {
            ++st->busy;
            continue;
        }
        if (rc != PSMSGR_OK) {
            if (rc != PSMSGR_E_NODATA || seen) {
                ++st->bad_rc;
                st->last_bad_rc = rc;
            }
            continue;
        }
        if (info.generation == 0)
            ++st->bad_gen;
        if (seen && ((int32_t)(info.generation - prev_gen) < 0 || info.timestamp_ns < prev_ts))
            ++st->bad_order;
        seen = true;
        prev_gen = info.generation;
        prev_ts = info.timestamp_ns;
        if (peek) {
            ++st->peeks;
        } else {
            ++st->reads;
            check_value(st, buf, &info);
        }
    }
    psmsgr_state_reader_close(r);
    free(buf);
}

typedef struct reader_thread {
    pthread_t thread;
    const run_cfg *cfg;
    reader_stats stats;
} reader_thread;

static void *reader_thread_main(void *arg)
{
    reader_thread *t = arg;
    reader_loop(t->cfg, &t->stats);
    return NULL;
}

typedef struct reader_proc {
    pid_t pid;
    int fd; /* read end: the child's reader_stats */
} reader_proc;

/* Forked while the process is single-threaded. The child reports its stats
 * through a pipe; it dies with the test program. */
static reader_proc reader_proc_start(const run_cfg *cfg)
{
    reader_proc c = { -1, -1 };
    int p[2];
    if (pipe(p) != 0)
        return c;
    fflush(NULL);
    pid_t parent = getpid();
    c.pid = fork();
    if (c.pid == 0) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent)
            _exit(3);
        close(p[0]);
        reader_stats st;
        reader_loop(cfg, &st);
        _exit(write(p[1], &st, sizeof st) == (ssize_t)sizeof st ? 0 : 3);
    }
    close(p[1]);
    c.fd = p[0];
    return c;
}

/* Collects the child's stats; false if it did not report them and exit 0. */
static bool reader_proc_finish(reader_proc *c, reader_stats *st)
{
    ssize_t n = -1;
    if (c->fd >= 0) {
        while ((n = read(c->fd, st, sizeof *st)) < 0 && errno == EINTR) {
        }
        close(c->fd);
    }
    int status = 0;
    if (c->pid > 0) {
        while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    return c->pid > 0 && n == (ssize_t)sizeof *st && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- writer ------------------------------------------------------------------- */

typedef struct writer_stats {
    uint64_t publishes; /* publish or begin/commit */
    uint64_t aborts;
    uint64_t errors; /* failed call, or not the expected generation */
    int last_error;
} writer_stats;

static void writer_error(writer_stats *st, int rc)
{
    ++st->errors;
    st->last_error = rc;
}

/* Publishes until the deadline, mixing publish, begin/commit and (rarely)
 * begin/abort with random lengths. Starts just below the generation wrap. */
static void writer_loop(psmsgr_state_writer *w, const run_cfg *cfg, writer_stats *st)
{
    memset(st, 0, sizeof *st);
    unsigned char *buf = malloc(cfg->capacity);
    if (buf == NULL) {
        writer_error(st, PSMSGR_E_SYS);
        return;
    }
    uint64_t rng = UINT64_C(0x9E3779B97F4A7C15) ^ ((uint64_t)cfg->capacity << 8) ^ cfg->slots;
    uint32_t gen = UINT32_MAX - 100;
    psmi_writer_set_generation(w, gen);
    while (psmsgr_now_ns() < cfg->deadline_ns) {
        uint64_t x = rand_next(&rng);
        uint32_t len = (uint32_t)(x % ((uint64_t)cfg->capacity + 1));
        uint32_t got = 0;
        int rc;
        if ((x >> 32) % 16 == 0) {
            /* An abort leaves the slot odd, with a partly overwritten payload
             * under the old generation (§5.4): it must never be readable. */
            void *p;
            rc = psmsgr_state_begin(w, &p);
            if (rc == PSMSGR_OK) {
                fill_derived(p, ~gen, len);
                rc = psmsgr_state_abort(w);
            }
            if (rc != PSMSGR_OK)
                writer_error(st, rc);
            else
                ++st->aborts;
            continue;
        }
        if ((x >> 32) & 1u) {
            make_payload(buf, gen, len);
            rc = psmsgr_state_publish(w, buf, len, &got);
        } else {
            void *p;
            rc = psmsgr_state_begin(w, &p);
            if (rc == PSMSGR_OK) {
                make_payload(p, gen, len);
                rc = psmsgr_state_commit(w, len, &got);
            }
        }
        if (rc != PSMSGR_OK || got != gen)
            writer_error(st, rc);
        else
            ++st->publishes;
        gen = next_generation(gen);
    }
    free(buf);
}

/* ---- a run -------------------------------------------------------------------- */

typedef struct run_result {
    writer_stats w;
    reader_stats r[MAX_READERS]; /* threads first, then processes */
    bool proc_ok[MAX_READERS];
    int writer_open_rc;
} run_result;

static uint64_t bad_reads(const reader_stats *s)
{
    return s->bad_data + s->bad_gen + s->bad_len + s->bad_order + s->bad_rc;
}

/* Readers first, then the writer: processes are forked before any thread
 * exists, and they attach lazily once the writer creates the channel. The
 * main thread is the writer, and it asserts nothing until all readers are
 * done. */
static void run(run_cfg *cfg, uint64_t ns, run_result *res)
{
    memset(res, 0, sizeof *res);
    cfg->deadline_ns = psmsgr_now_ns() + ns;

    reader_proc procs[READER_PROCS];
    for (unsigned i = 0; i < cfg->procs; ++i)
        procs[i] = reader_proc_start(cfg);
    reader_thread threads[READER_THREADS];
    bool started[READER_THREADS] = { false };
    for (unsigned i = 0; i < cfg->threads; ++i) {
        threads[i].cfg = cfg;
        started[i] = pthread_create(&threads[i].thread, NULL, reader_thread_main, &threads[i]) == 0;
    }

    psmsgr_state_writer *w = NULL;
    res->writer_open_rc = open_writer(CHAN, cfg->capacity, cfg->slots, PSMSGR_STATE_NO_NOTIFY, &w);
    if (res->writer_open_rc == PSMSGR_OK)
        writer_loop(w, cfg, &res->w);

    for (unsigned i = 0; i < cfg->threads; ++i) {
        if (started[i]) {
            pthread_join(threads[i].thread, NULL);
            res->r[i] = threads[i].stats;
        } else {
            res->r[i].bad_rc = 1;
            res->r[i].last_bad_rc = PSMSGR_E_SYS;
        }
        res->proc_ok[i] = true;
    }
    for (unsigned i = 0; i < cfg->procs; ++i)
        res->proc_ok[cfg->threads + i] = reader_proc_finish(&procs[i], &res->r[cfg->threads + i]);
    psmsgr_state_writer_close(w);
}

static void report(const run_cfg *cfg, const run_result *res, double seconds)
{
    print_message("slots %" PRIu32 ", capacity %" PRIu32 ": %.1f s, %" PRIu64 " publishes, %" PRIu64
                  " aborts\n",
                  cfg->slots, cfg->capacity, seconds, res->w.publishes, res->w.aborts);
    for (unsigned i = 0; i < cfg->threads + cfg->procs; ++i) {
        const reader_stats *s = &res->r[i];
        uint64_t attempts = s->reads + s->peeks + s->busy;
        print_message("  %s %u: %" PRIu64 " reads, %" PRIu64 " peeks, %" PRIu64 " busy (%.2g), "
                      "%" PRIu64 " bad [data %" PRIu64 ", gen %" PRIu64 ", len %" PRIu64
                      ", order %" PRIu64 ", rc %" PRIu64 " (%s)]\n",
                      i < cfg->threads ? "thread " : "process", i, s->reads, s->peeks, s->busy,
                      attempts ? (double)s->busy / (double)attempts : 0.0, bad_reads(s),
                      s->bad_data, s->bad_gen, s->bad_len, s->bad_order, s->bad_rc,
                      s->bad_rc ? psmsgr_strerror(s->last_bad_rc) : "-");
    }
}

static void torture(uint32_t slots, uint32_t capacity)
{
    run_cfg cfg = {
        .slots = slots, .capacity = capacity, .threads = READER_THREADS, .procs = READER_PROCS
    };
    run_result res;
    run(&cfg, duration_ns, &res);
    report(&cfg, &res, (double)duration_ns / 1e9);

    assert_rc(res.writer_open_rc, PSMSGR_OK);
    if (res.w.errors != 0)
        print_message("writer: %s\n", res.w.last_error != PSMSGR_OK
                                          ? psmsgr_strerror(res.w.last_error)
                                          : "unexpected generation");
    assert_uint_equal(res.w.errors, 0);
    assert_uint_not_equal(res.w.publishes, 0);
    for (unsigned i = 0; i < cfg.threads + cfg.procs; ++i) {
        assert_true(res.proc_ok[i]);
        assert_uint_equal(bad_reads(&res.r[i]), 0);
        assert_uint_not_equal(res.r[i].reads, 0);
    }
}

#define VARIANT(slots, cap, suffix)                            \
    static void torture_##slots##_slots_##suffix(void **state) \
    {                                                          \
        torture(slots, cap);                                   \
    }

VARIANT(2, 16, 16b)
VARIANT(2, 4096, 4k)
VARIANT(2, 1u << 20, 1m)
VARIANT(3, 16, 16b)
VARIANT(3, 4096, 4k)
VARIANT(3, 1u << 20, 1m)

/* ---- fault injection ------------------------------------------------------------ */

/* With the reader's second seq check disabled, the checks above must see torn
 * values; otherwise a passing torture test would prove nothing. Runs short
 * rounds until one does, for at most 30 s (tears are usually immediate). */
static void torn_reads_are_detected(void **state)
{
    run_cfg cfg = { .slots = 2, .capacity = 4096, .threads = READER_THREADS, .procs = 0 };
    run_result res;
    uint64_t data = 0, gen = 0, len = 0, reads = 0, rounds = 0;
    uint64_t start = psmsgr_now_ns();
    psmi_test_skip_seq_recheck = true;
    do {
        run(&cfg, 250000000u, &res);
        ++rounds;
        for (unsigned i = 0; i < cfg.threads; ++i) {
            data += res.r[i].bad_data;
            gen += res.r[i].bad_gen;
            len += res.r[i].bad_len;
            reads += res.r[i].reads;
        }
    } while (data + gen + len == 0 && elapsed_ms(start) < 30000 && res.writer_open_rc == PSMSGR_OK);
    psmi_test_skip_seq_recheck = false;

    print_message("seq recheck disabled: %" PRIu64 " reads in %" PRIu64
                  " rounds, torn: data %" PRIu64 ", gen %" PRIu64 ", len %" PRIu64 "\n",
                  reads, rounds, data, gen, len);
    assert_rc(res.writer_open_rc, PSMSGR_OK);
    assert_uint_not_equal(data + gen + len, 0);
}

int main(void)
{
    const char *env = getenv("PSMSGR_TORTURE_SECONDS");
    if (env != NULL && env[0] != '\0') {
        char *end;
        double s = strtod(env, &end);
        if (*end != '\0' || !(s > 0 && s < 1e6)) {
            fprintf(stderr, "PSMSGR_TORTURE_SECONDS: not a number of seconds: %s\n", env);
            return 1;
        }
        duration_ns = (uint64_t)(s * 1e9 + 0.5);
    }
    crc32_init();

    const struct CMUnitTest tests[] = {
        TEST(torture_2_slots_16b),     TEST(torture_2_slots_4k), TEST(torture_2_slots_1m),
        TEST(torture_3_slots_16b),     TEST(torture_3_slots_4k), TEST(torture_3_slots_1m),
        TEST(torn_reads_are_detected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
