/* SPDX-License-Identifier: Apache-2.0 */
/* State channel unit tests (spec/build-and-test.md, "Unit"), through the
 * public API of the shared library. layout.h is used only to inspect and
 * corrupt channel files from the outside. */
#include "state_util.h"

#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/mount.h>
#include <sys/syscall.h>

/* ---- names and options ------------------------------------------------------ */

static void names_are_validated(void)
{
    char longest[PSMSGR_NAME_MAX + 2];
    memset(longest, 'n', sizeof longest);
    longest[PSMSGR_NAME_MAX] = '\0';

    const char *good[] = { "a", "A-b_c.d", "_x", "-x", "0", "x.", "a..b", longest };
    for (size_t i = 0; i < sizeof good / sizeof *good; ++i) {
        psmsgr_state_reader *r = NULL;
        CHECK(psmsgr_state_reader_open(good[i], test_dir, &r) == PSMSGR_OK);
        psmsgr_state_reader_close(r);
        CHECK(psmsgr_state_unlink(good[i], test_dir) == PSMSGR_E_NODATA);
    }

    longest[PSMSGR_NAME_MAX]     = 'n';
    longest[PSMSGR_NAME_MAX + 1] = '\0';
    const char *bad[] = { "", ".x", "..", "a/b", "a b", "a\n", "\xc3\xa4", "a*", longest, NULL };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        psmsgr_state_reader *r = (psmsgr_state_reader *)1;
        psmsgr_state_writer *w = (psmsgr_state_writer *)1;
        CHECK(psmsgr_state_reader_open(bad[i], test_dir, &r) == PSMSGR_E_INVAL);
        CHECK(r == NULL);
        CHECK(open_writer(bad[i], 8, 2, 0, &w) == PSMSGR_E_INVAL);
        CHECK(w == NULL);
        CHECK(psmsgr_state_unlink(bad[i], test_dir) == PSMSGR_E_INVAL);
    }
}

static void options_defaults_and_limits(void)
{
    psmsgr_state_options o;
    memset(&o, 0xA5, sizeof o);
    psmsgr_state_options_init(&o);
    CHECK(o.struct_size == sizeof o);
    CHECK(o.capacity == 0);
    CHECK(o.slot_count == PSMSGR_STATE_DEFAULT_SLOTS);
    CHECK(o.payload_type == 0);
    CHECK(o.mode == 0644);
    CHECK(o.flags == 0);
    CHECK(o.dir == NULL);

    psmsgr_state_writer *w = NULL;
    CHECK(open_writer(CHAN, 8, 1, 0, &w) == PSMSGR_E_INVAL);
    CHECK(open_writer(CHAN, 8, 17, 0, &w) == PSMSGR_E_INVAL);
    CHECK(open_writer(CHAN, PSMSGR_STATE_MAX_CAPACITY + 1, 2, 0, &w) == PSMSGR_E_INVAL);
    CHECK(open_writer(CHAN, 8, 2, 1u << 7, &w) == PSMSGR_E_INVAL);
    o = make_opts(8, 2, 0);
    o.mode = 010644;
    CHECK(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_E_INVAL);
    o = make_opts(8, 2, 0);
    o.struct_size = 0;
    CHECK(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_E_INVAL);
    o = make_opts(8, 2, 0);
    o.dir = "";
    CHECK(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_E_INVAL);
    CHECK(psmsgr_state_writer_open(CHAN, &o, NULL) == PSMSGR_E_INVAL);
    CHECK(access(lock_path(CHAN), F_OK) != 0); /* nothing created */

    /* Limits are inclusive. */
    CHECK(open_writer(CHAN, PSMSGR_STATE_MAX_CAPACITY, 2, 0, &w) == PSMSGR_OK);
    CHECK(psmsgr_state_writer_capacity(w) == PSMSGR_STATE_MAX_CAPACITY);
    psmsgr_state_writer_close(w);
    CHECK(open_writer("sixteen", 0, 16, 0, &w) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    CHECK(psmsgr_state_writer_capacity(NULL) == 0);

    /* Only the fields covered by struct_size are read. */
    o = make_opts(8, 2, 0);
    o.payload_type = 77;
    o.struct_size  = (uint32_t)offsetof(psmsgr_state_options, payload_type);
    o.dir          = NULL; /* not covered either: the default applies */
    setenv("PSMSGR_DIR", test_dir, 1);
    CHECK(psmsgr_state_writer_open("short", &o, &w) == PSMSGR_OK);
    unsetenv("PSMSGR_DIR");
    psmsgr_state_writer_close(w);
    psmi_header h;
    REQUIRE(raw_header("short", &h) == 0);
    CHECK(h.capacity == 8 && h.slot_count == 2 && h.payload_type == 0);
}

static void dir_comes_from_env_once(void)
{
    setenv("PSMSGR_DIR", test_dir, 1);
    psmsgr_state_writer *w = NULL;
    psmsgr_state_reader *r = NULL;
    CHECK(psmsgr_state_writer_open(CHAN, NULL, &w) == PSMSGR_OK); /* NULL: all defaults */
    CHECK(psmsgr_state_reader_open(CHAN, NULL, &r) == PSMSGR_OK);
    setenv("PSMSGR_DIR", "/nonexistent", 1); /* too late for both handles */
    REQUIRE(w != NULL && r != NULL);
    CHECK(access(data_path(CHAN), F_OK) == 0);
    CHECK(psmsgr_state_publish(w, NULL, 0, NULL) == PSMSGR_OK);
    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK);
    unsetenv("PSMSGR_DIR");
    psmsgr_state_reader_close(r);
    psmsgr_state_writer_close(w);
}

/* ---- create and reuse ---------------------------------------------------------- */

static void create_then_reuse(void)
{
    psmsgr_state_writer *w = NULL;
    psmsgr_state_options o = make_opts(16, 3, 0);
    o.payload_type = 0x1234;
    o.mode         = 0640;
    REQUIRE(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_OK);

    struct stat st;
    REQUIRE(stat(data_path(CHAN), &st) == 0);
    CHECK((st.st_mode & 07777) == 0640); /* fchmod: independent of the umask */
    CHECK(st.st_size == 128 + 3 * 64);
    REQUIRE(stat(lock_path(CHAN), &st) == 0);
    CHECK((st.st_mode & 07777) == 0640);

    psmi_header h;
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.magic == 0x534D5350u && memcmp(&h.magic, "PSMS", 4) == 0);
    CHECK(h.version_major == 1 && h.version_minor == 0);
    CHECK(h.header_size == 128 && h.slot_header_size == 32);
    CHECK(h.slot_count == 3 && h.slot_stride == 64 && h.capacity == 16);
    CHECK(h.payload_type == 0x1234 && h.config_flags == 0);
    CHECK(h.state == 0 && h.latest == PSMI_LATEST_NONE && h.notify == 0);
    CHECK(h.writer_pid == (uint32_t)getpid());
    CHECK(h.created_realtime_ns > 0);

    uint32_t gen = 0;
    CHECK(publish_str(w, "one", &gen) == PSMSGR_OK && gen == 1);
    CHECK(publish_str(w, "two", &gen) == PSMSGR_OK && gen == 2);
    CHECK(publish_str(w, "three", NULL) == PSMSGR_OK);
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.latest == 2 && h.notify == 3);
    ino_t ino = inode_of(data_path(CHAN));
    psmsgr_state_writer_close(w);

    /* Reuse: same file, generation carried across the restart. */
    REQUIRE(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_OK);
    CHECK(inode_of(data_path(CHAN)) == ino);
    CHECK(publish_str(w, "four", &gen) == PSMSGR_OK && gen == 4);
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.latest == 0); /* rotation continues after the last slot */

    psmsgr_state_reader *r = open_reader(CHAN);
    char buf[32];
    psmsgr_state_info info;
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK);
    CHECK(strcmp(buf, "four") == 0 && info.generation == 4 && info.length == 4);
    CHECK(info.reserved == 0);
    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK);
    CHECK(d.capacity == 16 && d.slot_count == 3 && d.payload_type == 0x1234 && d.flags == 0);
    psmsgr_state_reader_close(r);
    psmsgr_state_writer_close(w);
}

static void mismatch_and_recreate(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 16, 3, 0, &w) == PSMSGR_OK);
    uint32_t gen;
    CHECK(publish_str(w, "old", &gen) == PSMSGR_OK && gen == 1);
    psmsgr_state_writer_close(w);

    psmsgr_state_reader *r = open_reader(CHAN);
    char buf[64];
    psmsgr_state_info info;
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "old") == 0);
    CHECK(info.flags & PSMSGR_INFO_ATTACHED);

    CHECK(open_writer(CHAN, 32, 3, 0, &w) == PSMSGR_E_MISMATCH);
    CHECK(open_writer(CHAN, 16, 4, 0, &w) == PSMSGR_E_MISMATCH);
    CHECK(open_writer(CHAN, 16, 3, PSMSGR_STATE_NO_NOTIFY, &w) == PSMSGR_E_MISMATCH);
    psmsgr_state_options o = make_opts(16, 3, 0);
    o.payload_type = 9;
    CHECK(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_E_MISMATCH);
    CHECK(w == NULL);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 1);

    ino_t ino = inode_of(data_path(CHAN));
    REQUIRE(open_writer(CHAN, 32, 3, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    CHECK(inode_of(data_path(CHAN)) != ino);
    CHECK(psmsgr_state_writer_capacity(w) == 32);

    /* The reader follows the retire to the new, still empty file. */
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA);
    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK && d.capacity == 32);
    CHECK(publish_str(w, "new", &gen) == PSMSGR_OK && gen == 2); /* carried */
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "new") == 0);
    CHECK(info.generation == 2 && (info.flags & PSMSGR_INFO_ATTACHED));

    /* RECREATE on a compatible file is a plain reuse. */
    psmsgr_state_writer_close(w);
    ino = inode_of(data_path(CHAN));
    REQUIRE(open_writer(CHAN, 32, 3, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    CHECK(inode_of(data_path(CHAN)) == ino);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- writer exclusivity ------------------------------------------------------------ */

static void second_writer_same_process(void)
{
    psmsgr_state_writer *a = NULL, *b = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &a) == PSMSGR_OK);
    CHECK(open_writer(CHAN, 8, 2, 0, &b) == PSMSGR_E_WRITER_EXISTS);
    CHECK(open_writer(CHAN, 8, 2, PSMSGR_STATE_RECREATE, &b) == PSMSGR_E_WRITER_EXISTS);
    CHECK(psmsgr_state_unlink(CHAN, test_dir) == PSMSGR_E_WRITER_EXISTS);
    CHECK(b == NULL);
    psmsgr_state_writer_close(a);
    CHECK(open_writer(CHAN, 8, 2, 0, &b) == PSMSGR_OK);
    psmsgr_state_writer_close(b);
}

static int child_open_writer(void)
{
    psmsgr_state_writer *w = NULL;
    int rc = open_writer(CHAN, 8, 2, 0, &w);
    if (rc == PSMSGR_OK)
        rc = psmsgr_state_publish(w, "child", 5, NULL);
    return rc; /* the handle stays open until the child is killed */
}

static void second_writer_other_process(void)
{
    psmsgr_state_reader *r = open_reader(CHAN);
    CHECK(psmsgr_state_writer_alive(r) == 0); /* no lock file yet */

    child c = child_start(child_open_writer);
    REQUIRE(child_result(&c) == PSMSGR_OK);
    psmsgr_state_writer *w = NULL;
    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_E_WRITER_EXISTS);
    CHECK(psmsgr_state_writer_alive(r) == 1);

    child_kill(&c);
    CHECK(psmsgr_state_writer_alive(r) == 0);
    char buf[16];
    psmsgr_state_info info;
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "child") == 0);

    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(psmsgr_state_writer_alive(r) == 1);
    psmsgr_state_writer_close(w);
    CHECK(psmsgr_state_writer_alive(r) == 0);
    psmsgr_state_reader_close(r);
}

/* ---- crash recovery ------------------------------------------------------------ */

static int child_crash_mid_publish(void)
{
    psmsgr_state_writer *w = NULL;
    int rc = open_writer(CHAN, 16, 3, 0, &w);
    if (rc != PSMSGR_OK)
        return rc;
    if ((rc = psmsgr_state_publish(w, "one", 3, NULL)) != PSMSGR_OK)
        return rc;
    void *buf;
    if ((rc = psmsgr_state_begin(w, &buf)) != PSMSGR_OK)
        return rc;
    memcpy(buf, "PARTIAL", 7); /* then "crash": killed with the begin open */
    return PSMSGR_OK;
}

static void crash_mid_publish(void)
{
    child c = child_start(child_crash_mid_publish);
    REQUIRE(child_result(&c) == PSMSGR_OK);
    child_kill(&c);

    psmi_header h;
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.latest == 0);
    uint32_t seq = raw_slot_seq(CHAN, 1);
    CHECK(seq & 1u); /* the interrupted slot */

    psmsgr_state_reader *r = open_reader(CHAN);
    char buf[32];
    psmsgr_state_info info;
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "one") == 0);

    /* Leftovers of an interrupted create. Only exact tmp names are ours:
     * the longer ones belong to a channel named "chan.state.tmp.AbC123". */
    const char *tmp   = chan_path(CHAN, ".state.tmp.AbC123");
    const char *other = chan_path(CHAN, ".state.tmp.AbC123.state");
    const char *short_ = chan_path(CHAN, ".state.tmp.abc");
    const char *files[] = { tmp, other, short_ };
    for (int i = 0; i < 3; ++i) {
        int fd = open(files[i], O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(fd >= 0);
        close(fd);
    }

    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 16, 3, 0, &w) == PSMSGR_OK);
    CHECK(access(tmp, F_OK) != 0);
    CHECK(access(other, F_OK) == 0);
    CHECK(access(short_, F_OK) == 0);
    CHECK(raw_slot_seq(CHAN, 1) == seq); /* still unreadable until rewritten */

    uint32_t gen;
    CHECK(publish_str(w, "two", &gen) == PSMSGR_OK && gen == 2);
    CHECK(raw_slot_seq(CHAN, 1) == seq + 1); /* rewritten and committed */
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "two") == 0);
    CHECK(info.generation == 2);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- lazy reader ------------------------------------------------------------------- */

static void lazy_reader_attaches_later(void)
{
    psmsgr_state_reader *r = open_reader(CHAN);
    REQUIRE(r != NULL);
    psmsgr_state_info info;
    psmsgr_state_desc d;
    char buf[16];
    CHECK(psmsgr_state_read(r, buf, sizeof buf, &info) == PSMSGR_E_NODATA);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA);
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_E_NODATA);
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_E_TIMEOUT);
    CHECK(psmsgr_state_writer_alive(r) == 0);

    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA); /* attached, nothing published */
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK && d.capacity == 8);
    CHECK(psmsgr_state_writer_alive(r) == 1);

    uint32_t gen;
    CHECK(publish_str(w, "hi", &gen) == PSMSGR_OK);
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "hi") == 0);
    CHECK(info.generation == gen && (info.flags & PSMSGR_INFO_ATTACHED));
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
    psmsgr_state_reader_close(NULL);
    psmsgr_state_writer_close(NULL);
}

/* ---- read results ---------------------------------------------------------------- */

static void read_results_and_sizes(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    unsigned char buf[16];
    psmsgr_state_info info;
    CHECK(psmsgr_state_read(r, buf, sizeof buf, &info) == PSMSGR_E_NODATA);

    CHECK(psmsgr_state_publish(w, "123456789", 9, NULL) == PSMSGR_E_TOOBIG);
    CHECK(psmsgr_state_publish(w, NULL, 1, NULL) == PSMSGR_E_INVAL);
    CHECK(psmsgr_state_publish(NULL, "x", 1, NULL) == PSMSGR_E_INVAL);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA); /* failures publish nothing */
    CHECK(psmsgr_state_publish(w, "12345678", 8, NULL) == PSMSGR_OK);

    memset(buf, 0xAA, sizeof buf);
    CHECK(psmsgr_state_read(r, buf, 7, &info) == PSMSGR_E_TOOSMALL);
    CHECK(info.length == 8 && info.generation == 1);
    CHECK(info.flags & PSMSGR_INFO_ATTACHED); /* TOOSMALL is a result too */
    CHECK(buf[0] == 0xAA); /* nothing copied */
    CHECK(psmsgr_state_read(r, buf, 8, &info) == PSMSGR_OK);
    CHECK(info.length == 8 && memcmp(buf, "12345678", 8) == 0 && info.flags == 0);

    CHECK(psmsgr_state_publish(w, NULL, 0, NULL) == PSMSGR_OK);
    CHECK(psmsgr_state_read(r, NULL, 0, &info) == PSMSGR_OK && info.length == 0);
    CHECK(psmsgr_state_read(r, NULL, 1, &info) == PSMSGR_E_INVAL);
    CHECK(psmsgr_state_read(r, buf, sizeof buf, NULL) == PSMSGR_E_INVAL);
    CHECK(psmsgr_state_peek(r, NULL) == PSMSGR_E_INVAL);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void heartbeat_channel(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 0, 2, 0, &w) == PSMSGR_OK);
    CHECK(psmsgr_state_writer_capacity(w) == 0);
    struct stat st;
    CHECK(stat(data_path(CHAN), &st) == 0 && st.st_size == 128 + 2 * 64);
    CHECK(psmsgr_state_publish(w, "x", 1, NULL) == PSMSGR_E_TOOBIG);
    uint32_t gen;
    CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK && gen == 1);
    void *buf;
    CHECK(psmsgr_state_begin(w, &buf) == PSMSGR_OK);
    CHECK(psmsgr_state_commit(w, 1, NULL) == PSMSGR_E_TOOBIG);
    CHECK(psmsgr_state_commit(w, 0, &gen) == PSMSGR_OK && gen == 2);

    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_info info;
    CHECK(psmsgr_state_read(r, NULL, 0, &info) == PSMSGR_OK);
    CHECK(info.generation == 2 && info.length == 0);
    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK && d.capacity == 0);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- zero-copy publish ---------------------------------------------------------- */

static void begin_commit_abort(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 16, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    char str[32];
    psmsgr_state_info info;
    uint32_t gen;

    CHECK(psmsgr_state_commit(w, 0, NULL) == PSMSGR_E_STATE);
    CHECK(psmsgr_state_abort(w) == PSMSGR_E_STATE);
    CHECK(psmsgr_state_begin(w, NULL) == PSMSGR_E_INVAL);
    CHECK(publish_str(w, "A", &gen) == PSMSGR_OK && gen == 1); /* slot 0 */

    void *buf = NULL, *again = NULL;
    REQUIRE(psmsgr_state_begin(w, &buf) == PSMSGR_OK);
    CHECK(((uintptr_t)buf % 32) == 0);
    memcpy(buf, "XXXXXXXX", 8);
    CHECK(psmsgr_state_begin(w, &again) == PSMSGR_E_STATE);
    CHECK(publish_str(w, "B", NULL) == PSMSGR_E_STATE);
    CHECK(read_str(r, str, sizeof str, &info) == PSMSGR_OK && strcmp(str, "A") == 0);

    /* Abort leaves the partly written slot unreadable (odd). */
    CHECK(psmsgr_state_abort(w) == PSMSGR_OK);
    CHECK(psmsgr_state_abort(w) == PSMSGR_E_STATE);
    CHECK(raw_slot_seq(CHAN, 1) & 1u);
    CHECK(read_str(r, str, sizeof str, &info) == PSMSGR_OK && strcmp(str, "A") == 0);
    CHECK(info.generation == 1);

    /* The next begin reuses that slot and commits it. */
    REQUIRE(psmsgr_state_begin(w, &again) == PSMSGR_OK);
    CHECK(again == buf);
    memcpy(again, "B", 1);
    CHECK(psmsgr_state_commit(w, 17, NULL) == PSMSGR_E_TOOBIG); /* begin stays open */
    CHECK(psmsgr_state_commit(w, 1, &gen) == PSMSGR_OK && gen == 2);
    CHECK((raw_slot_seq(CHAN, 1) & 1u) == 0);
    CHECK(read_str(r, str, sizeof str, &info) == PSMSGR_OK && strcmp(str, "B") == 0);
    CHECK(info.generation == 2);

    /* Close with an open begin aborts it. */
    REQUIRE(psmsgr_state_begin(w, &buf) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    REQUIRE(open_writer(CHAN, 16, 2, 0, &w) == PSMSGR_OK);
    CHECK(read_str(r, str, sizeof str, &info) == PSMSGR_OK && strcmp(str, "B") == 0);
    CHECK(publish_str(w, "C", &gen) == PSMSGR_OK && gen == 3);
    CHECK(read_str(r, str, sizeof str, &info) == PSMSGR_OK && strcmp(str, "C") == 0);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- peek ------------------------------------------------------------------------ */

static void peek_timestamps(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 3, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    uint64_t prev = 0;
    for (uint32_t i = 1; i <= 20; ++i) {
        uint64_t before = psmsgr_now_ns();
        uint32_t gen;
        CHECK(psmsgr_state_publish(w, &i, sizeof i, &gen) == PSMSGR_OK && gen == i);
        uint64_t after = psmsgr_now_ns();
        psmsgr_state_info info;
        CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK);
        CHECK(info.generation == i && info.length == sizeof i);
        CHECK(info.timestamp_ns >= before && info.timestamp_ns <= after);
        CHECK(info.timestamp_ns >= prev);
        CHECK(((info.flags & PSMSGR_INFO_ATTACHED) != 0) == (i == 1));
        prev = info.timestamp_ns;
    }
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- wait -------------------------------------------------------------------------- */

typedef struct waiter {
    pthread_t            thread;
    psmsgr_state_reader *r;
    uint32_t             last;
    int32_t              timeout_ms;
    int                  rc;
    int                  done; /* atomic */
} waiter;

static void *waiter_main(void *arg)
{
    waiter *wt = arg;
    wt->rc = psmsgr_state_wait(wt->r, wt->last, wt->timeout_ms);
    __atomic_store_n(&wt->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int waiter_start(waiter *wt, psmsgr_state_reader *r, uint32_t last, int32_t timeout_ms)
{
    *wt = (waiter){ .r = r, .last = last, .timeout_ms = timeout_ms };
    return pthread_create(&wt->thread, NULL, waiter_main, wt);
}

static int waiter_join(waiter *wt)
{
    pthread_join(wt->thread, NULL);
    return wt->rc;
}

static void wait_wakes_on_publish(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    uint32_t gen;
    CHECK(publish_str(w, "a", &gen) == PSMSGR_OK);

    /* A value that differs from last_generation returns at once. */
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_OK);
    CHECK(psmsgr_state_wait(r, gen + 1, -1) == PSMSGR_OK);

    psmi_header h;
    REQUIRE(raw_header(CHAN, &h) == 0);
    uint32_t notify = h.notify;

    waiter wt;
    REQUIRE(waiter_start(&wt, r, gen, 10000) == 0);
    sleep_ms(50); /* likely blocked by now; the test holds either way */
    CHECK(publish_str(w, "b", NULL) == PSMSGR_OK);
    CHECK(waiter_join(&wt) == PSMSGR_OK);
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.notify == notify + 1); /* the futex word the waiter sleeps on */

    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == gen + 1);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void wait_timeouts(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);

    /* Nothing published: NODATA counts as unchanged, also for 0. */
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_E_TIMEOUT);
    uint32_t gen;
    CHECK(publish_str(w, "a", &gen) == PSMSGR_OK);

    CHECK(psmsgr_state_wait(r, gen, 0) == PSMSGR_E_TIMEOUT);

    /* Lower bounds only: the deadline is on CLOCK_MONOTONIC, so these are
     * exact. How promptly wait returns is for bench/ on the target. */
    uint64_t t0 = psmsgr_now_ns();
    CHECK(psmsgr_state_wait(r, gen, 120) == PSMSGR_E_TIMEOUT);
    CHECK(elapsed_ms(t0) >= 120);

    /* Unattached readers time out as well. */
    psmsgr_state_reader *none = open_reader("absent");
    t0 = psmsgr_now_ns();
    CHECK(psmsgr_state_wait(none, 0, 50) == PSMSGR_E_TIMEOUT);
    CHECK(elapsed_ms(t0) >= 50);
    CHECK(psmsgr_state_wait(NULL, 0, 0) == PSMSGR_E_INVAL);
    psmsgr_state_reader_close(none);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* Waiters on a file must be woken when it is retired, not left to notice at
 * their next identity check. Checked through the protocol state instead of
 * timing: the old file is RETIRED and its futex word was bumped once. */
static bool retired_and_notified(int old_fd, uint32_t notify_before)
{
    psmi_header h;
    return pread(old_fd, &h, sizeof h, 0) == (ssize_t)sizeof h &&
           (h.state & PSMI_STATE_RETIRED) != 0 && h.notify == notify_before + 1;
}

static void wait_follows_retire(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    uint32_t gen;
    CHECK(publish_str(w, "a", &gen) == PSMSGR_OK);
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_OK); /* attach */
    psmsgr_state_writer_close(w);
    int old_fd = open(data_path(CHAN), O_RDONLY | O_CLOEXEC);
    REQUIRE(old_fd >= 0);

    waiter wt;
    REQUIRE(waiter_start(&wt, r, gen, 10000) == 0);
    sleep_ms(50); /* likely blocked by now; the test holds either way */
    REQUIRE(open_writer(CHAN, 16, 2, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    CHECK(retired_and_notified(old_fd, 1));
    close(old_fd);
    sleep_ms(50);
    CHECK(__atomic_load_n(&wt.done, __ATOMIC_ACQUIRE) == 0); /* the retire alone: no return */
    CHECK(publish_str(w, "b", NULL) == PSMSGR_OK);
    CHECK(waiter_join(&wt) == PSMSGR_OK);

    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK && d.capacity == 16);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void wait_follows_unlink(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    uint32_t gen;
    CHECK(publish_str(w, "a", NULL) == PSMSGR_OK);
    CHECK(publish_str(w, "b", &gen) == PSMSGR_OK && gen == 2);
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    int old_fd = open(data_path(CHAN), O_RDONLY | O_CLOEXEC);
    REQUIRE(old_fd >= 0);

    waiter wt;
    REQUIRE(waiter_start(&wt, r, gen, 10000) == 0);
    sleep_ms(50); /* likely blocked by now; the test holds either way */
    CHECK(psmsgr_state_unlink(CHAN, test_dir) == PSMSGR_OK);
    CHECK(access(data_path(CHAN), F_OK) != 0 && access(lock_path(CHAN), F_OK) != 0);
    CHECK(retired_and_notified(old_fd, 2));
    close(old_fd);
    sleep_ms(50);
    CHECK(__atomic_load_n(&wt.done, __ATOMIC_ACQUIRE) == 0);
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(publish_str(w, "c", &gen) == PSMSGR_OK && gen == 1); /* fresh channel */
    CHECK(waiter_join(&wt) == PSMSGR_OK);

    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 1);
    psmsgr_state_writer_close(w);
    CHECK(psmsgr_state_unlink(CHAN, test_dir) == PSMSGR_OK);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA); /* retire check */
    CHECK(psmsgr_state_unlink(CHAN, test_dir) == PSMSGR_E_NODATA);
    CHECK(access(lock_path(CHAN), F_OK) != 0);
    psmsgr_state_reader_close(r);
}

static void wait_not_supported_without_notify(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, PSMSGR_STATE_NO_NOTIFY, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    CHECK(publish_str(w, "a", NULL) == PSMSGR_OK);
    CHECK(psmsgr_state_wait(r, 0, 100) == PSMSGR_E_NOTSUP);
    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK && d.flags == PSMSGR_STATE_NO_NOTIFY);
    psmi_header h;
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.config_flags == 1 && h.notify == 0); /* no wakes */
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void on_signal(int sig)
{
    (void)sig;
}

static void wait_interrupted_by_signal(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    uint32_t gen;
    CHECK(publish_str(w, "a", &gen) == PSMSGR_OK);

    struct sigaction sa = { .sa_handler = on_signal }, old; /* no SA_RESTART */
    sigemptyset(&sa.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &sa, &old) == 0);

    waiter wt;
    REQUIRE(waiter_start(&wt, r, gen, -1) == 0);
    /* Repeat: a signal that lands before the thread blocks is not an error. */
    for (int i = 0; i < 500 && !__atomic_load_n(&wt.done, __ATOMIC_ACQUIRE); ++i) {
        sleep_ms(10);
        pthread_kill(wt.thread, SIGUSR1);
    }
    if (!__atomic_load_n(&wt.done, __ATOMIC_ACQUIRE))
        CHECK(publish_str(w, "unblock", NULL) == PSMSGR_OK);
    CHECK(waiter_join(&wt) == PSMSGR_E_INTR);
    sigaction(SIGUSR1, &old, NULL);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- orphaned files ---------------------------------------------------------------- */

static void orphaned_file(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    char buf[16];
    psmsgr_state_info info;
    CHECK(publish_str(w, "old", NULL) == PSMSGR_OK);
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "old") == 0);
    psmsgr_state_writer_close(w);

    /* Deleted behind the library's back (rm, systemd RemoveIPC). */
    REQUIRE(unlink(data_path(CHAN)) == 0);
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(publish_str(w, "new", NULL) == PSMSGR_OK);
    CHECK(publish_str(w, "newer", NULL) == PSMSGR_OK);

    /* read and peek make no syscalls: they keep the old value. */
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "old") == 0);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 1);

    CHECK(psmsgr_state_writer_alive(r) == 1); /* identity check: reattaches */
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "newer") == 0);
    CHECK(info.generation == 2 && (info.flags & PSMSGR_INFO_ATTACHED));
    psmsgr_state_writer_close(w);

    /* wait notices within its one-second identity check. */
    REQUIRE(unlink(data_path(CHAN)) == 0);
    CHECK(psmsgr_state_writer_alive(r) == 0); /* path gone: unattached */
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA);
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(publish_str(w, "one", NULL) == PSMSGR_OK);
    CHECK(publish_str(w, "two", NULL) == PSMSGR_OK);
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "two") == 0);
    psmsgr_state_writer_close(w);
    REQUIRE(unlink(data_path(CHAN)) == 0);
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(publish_str(w, "three", NULL) == PSMSGR_OK); /* generation 1 */
    CHECK(psmsgr_state_wait(r, 2, 5000) == PSMSGR_OK); /* at its 1 s identity check */
    CHECK(read_str(r, buf, sizeof buf, &info) == PSMSGR_OK && strcmp(buf, "three") == 0);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- format handling --------------------------------------------------------------- */

static void minor_version_recreates(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    uint32_t gen;
    for (int i = 0; i < 3; ++i)
        CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK);
    psmsgr_state_writer_close(w);

    uint16_t minor = 1;
    REQUIRE(raw_write(data_path(CHAN), &minor, sizeof minor, 6) == 0);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK); /* readers accept any minor */
    CHECK(info.generation == 3 && (info.flags & PSMSGR_INFO_ATTACHED));

    ino_t ino = inode_of(data_path(CHAN));
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK); /* no RECREATE needed */
    CHECK(inode_of(data_path(CHAN)) != ino);
    psmi_header h;
    REQUIRE(raw_header(CHAN, &h) == 0);
    CHECK(h.version_minor == 0);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA); /* followed the retire */
    CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK && gen == 4);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 4);
    CHECK(info.flags & PSMSGR_INFO_ATTACHED);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* Replaces `path` with a new file (never truncates in place: that raises
 * SIGBUS in readers still mapping the old one). */
static void write_file(const char *path, const void *data, size_t n)
{
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    if (fd < 0 || (n != 0 && write(fd, data, n) != (ssize_t)n) || rename(tmp, path) != 0)
        fprintf(stderr, "write_file %s: %s\n", path, strerror(errno));
    if (fd >= 0)
        close(fd);
}

static void invalid_file_needs_recreate(void)
{
    unsigned char junk[300];
    memset(junk, 0x5A, sizeof junk);
    write_file(data_path(CHAN), junk, sizeof junk);

    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_FORMAT);
    psmsgr_state_writer *w = NULL;
    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_E_FORMAT);
    CHECK(access(lock_path(CHAN), F_OK) == 0); /* lock file stays; it is harmless */
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_E_FORMAT);

    REQUIRE(open_writer(CHAN, 8, 2, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    uint32_t gen;
    CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK && gen == 1);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 1);
    psmsgr_state_writer_close(w);

    /* An empty file and a corrupt `latest` are invalid too. */
    write_file(data_path(CHAN), NULL, 0);
    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_E_FORMAT);
    REQUIRE(open_writer(CHAN, 8, 2, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    uint32_t latest = 2;
    REQUIRE(raw_write(data_path(CHAN), &latest, sizeof latest, 40) == 0);
    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_E_FORMAT);
    psmsgr_state_reader_close(r); /* still mapping a file write_file() orphaned */
    r = open_reader(CHAN);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_FORMAT);
    psmsgr_state_reader_close(r);
}

/* A file left RETIRED by an interrupted unlink is replaced, not reused. */
static void retired_file_is_replaced(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    uint32_t gen;
    CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    uint32_t state = PSMI_STATE_RETIRED;
    REQUIRE(raw_write(data_path(CHAN), &state, sizeof state, 36) == 0);

    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA); /* counts as absent */
    ino_t ino = inode_of(data_path(CHAN));
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    CHECK(inode_of(data_path(CHAN)) != ino);
    CHECK(psmsgr_state_publish(w, NULL, 0, &gen) == PSMSGR_OK && gen == 2);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.generation == 2);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

static void attached_flag_once_per_attach(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_OK);
    psmsgr_state_reader *r = open_reader(CHAN);
    CHECK(psmsgr_state_publish(w, NULL, 0, NULL) == PSMSGR_OK);

    psmsgr_state_info info;
    psmsgr_state_desc d;
    CHECK(psmsgr_state_describe(r, &d) == PSMSGR_OK); /* attaches, does not consume */
    CHECK(psmsgr_state_wait(r, 0, 0) == PSMSGR_OK);   /* neither does wait */
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.flags == PSMSGR_INFO_ATTACHED);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.flags == 0);
    CHECK(psmsgr_state_read(r, NULL, 0, &info) == PSMSGR_OK && info.flags == 0);
    psmsgr_state_writer_close(w);

    REQUIRE(open_writer(CHAN, 8, 3, PSMSGR_STATE_RECREATE, &w) == PSMSGR_OK);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA);
    CHECK(psmsgr_state_publish(w, NULL, 0, NULL) == PSMSGR_OK);
    CHECK(psmsgr_state_read(r, NULL, 0, &info) == PSMSGR_OK && info.flags == PSMSGR_INFO_ATTACHED);
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_OK && info.flags == 0);
    psmsgr_state_writer_close(w);
    psmsgr_state_reader_close(r);
}

/* ---- file system edge cases -------------------------------------------------------- */

static void symlinks_are_rejected(void)
{
    char target[600];
    snprintf(target, sizeof target, "%s/target", test_dir);
    write_file(target, "x", 1);

    REQUIRE(symlink(target, data_path(CHAN)) == 0);
    psmsgr_state_writer *w = NULL;
    errno = 0;
    CHECK(open_writer(CHAN, 8, 2, PSMSGR_STATE_RECREATE, &w) == PSMSGR_E_SYS && errno == ELOOP);
    psmsgr_state_reader *r = open_reader(CHAN);
    psmsgr_state_info info;
    errno = 0;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_SYS && errno == ELOOP);
    psmsgr_state_reader_close(r);
    REQUIRE(unlink(data_path(CHAN)) == 0);

    REQUIRE(unlink(lock_path(CHAN)) == 0);
    REQUIRE(symlink(target, lock_path(CHAN)) == 0);
    errno = 0;
    CHECK(open_writer(CHAN, 8, 2, 0, &w) == PSMSGR_E_SYS && errno == ELOOP);
    errno = 0;
    CHECK(psmsgr_state_unlink(CHAN, test_dir) == PSMSGR_E_SYS && errno == ELOOP);
    CHECK(access(data_path(CHAN), F_OK) != 0);
}

static void missing_dir(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/missing", test_dir);
    psmsgr_state_options o = make_opts(8, 2, 0);
    o.dir = dir;
    psmsgr_state_writer *w = NULL;
    errno = 0;
    CHECK(psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_E_SYS && errno == ENOENT);
    CHECK(psmsgr_state_unlink(CHAN, dir) == PSMSGR_E_SYS && errno == ENOENT);

    psmsgr_state_reader *r = NULL;
    REQUIRE(psmsgr_state_reader_open(CHAN, dir, &r) == PSMSGR_OK); /* stays unattached */
    psmsgr_state_info info;
    CHECK(psmsgr_state_peek(r, &info) == PSMSGR_E_NODATA);
    CHECK(psmsgr_state_writer_alive(r) == 0);
    psmsgr_state_reader_close(r);
}

/* ---- reader validation fuzzing ------------------------------------------------------ */

static uint32_t rng_state = 0x9E3779B9u;

static uint32_t rnd(void)
{
    uint32_t x = rng_state; /* xorshift32: deterministic, reproducible */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

/* What a reader must report for this file: §6.1 attach checks, then the
 * §6.3 checks on `latest` and the payload length. */
static int expected_result(const unsigned char *f, size_t len)
{
    psmi_header h;
    if (len < sizeof h)
        return PSMSGR_E_FORMAT;
    memcpy(&h, f, sizeof h);
    if (h.magic != PSMI_MAGIC || h.version_major != 1 || h.header_size != 128 ||
        h.slot_header_size != 32 || h.slot_count < 2 || h.slot_count > 16 ||
        h.capacity > PSMSGR_STATE_MAX_CAPACITY || h.slot_stride != psmi_slot_stride(h.capacity) ||
        psmi_file_size(h.slot_count, h.slot_stride) > len)
        return PSMSGR_E_FORMAT;
    if (h.latest == PSMI_LATEST_NONE)
        return PSMSGR_E_NODATA;
    if (h.latest >= h.slot_count)
        return PSMSGR_E_FORMAT;
    psmi_slot s;
    memcpy(&s, f + 128 + (size_t)h.latest * h.slot_stride, sizeof s);
    if (s.seq & 1u)
        return PSMSGR_E_BUSY;
    return s.length > h.capacity ? PSMSGR_E_FORMAT : PSMSGR_OK;
}

static void fuzz_header(void)
{
    psmsgr_state_writer *w = NULL;
    REQUIRE(open_writer(CHAN, 40, 3, 0, &w) == PSMSGR_OK);
    for (uint32_t i = 0; i < 5; ++i)
        CHECK(psmsgr_state_publish(w, &i, sizeof i, NULL) == PSMSGR_OK);
    psmsgr_state_writer_close(w);
    unsigned char valid[128 + 3 * 128];
    REQUIRE(raw_read(data_path(CHAN), valid, sizeof valid, 0) == 0);
    REQUIRE(expected_result(valid, sizeof valid) == PSMSGR_OK);

    static const uint32_t extremes[] = {
        0, 1, 2, 3, 15, 16, 17, 31, 32, 33, 63, 64, 127, 128, 129,
        PSMSGR_STATE_MAX_CAPACITY, PSMSGR_STATE_MAX_CAPACITY + 1,
        0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
    };
    static const size_t fields[] = { 0, 8, 12, 16, 20, 24, 40 };
    unsigned char file[sizeof valid];
    int formats = 0;
    for (int iter = 0; iter < 600; ++iter) {
        memcpy(file, valid, sizeof file);
        size_t len = sizeof file;
        switch (rnd() % 5) {
        case 0: /* random header */
            for (size_t b = 0; b < 128; ++b)
                file[b] = (unsigned char)rnd();
            break;
        case 1: { /* one field set to an extreme value */
            uint32_t v = extremes[rnd() % (sizeof extremes / sizeof *extremes)];
            memcpy(file + fields[rnd() % (sizeof fields / sizeof *fields)], &v, sizeof v);
            break;
        }
        case 2: { /* versions */
            uint16_t v = (uint16_t)rnd();
            memcpy(file + 4 + 2 * (rnd() % 2), &v, sizeof v);
            break;
        }
        case 3: /* truncated */
            len = rnd() % sizeof file;
            break;
        default: { /* payload length of the latest slot beyond capacity */
            uint32_t latest, v = 41 + rnd() % 1000;
            memcpy(&latest, file + 40, sizeof latest);
            memcpy(file + 128 + latest * 128 + 16, &v, sizeof v);
            break;
        }
        }
        write_file(data_path(CHAN), file, len);
        int expect = expected_result(file, len);
        formats += expect == PSMSGR_E_FORMAT;

        psmsgr_state_reader *r = open_reader(CHAN);
        unsigned char buf[64];
        psmsgr_state_info info;
        psmsgr_state_desc d;
        int rc_read = psmsgr_state_read(r, buf, sizeof buf, &info);
        int rc_peek = psmsgr_state_peek(r, &info);
        int rc_desc = psmsgr_state_describe(r, &d);
        int rc_wait = psmsgr_state_wait(r, 0, 0);
        (void)psmsgr_state_writer_alive(r);
        psmsgr_state_reader_close(r);
        CHECK(rc_peek == expect);
        CHECK(rc_read == expect || (expect == PSMSGR_OK && rc_read == PSMSGR_E_TOOSMALL));
        CHECK(rc_desc == PSMSGR_OK || rc_desc == PSMSGR_E_FORMAT);
        CHECK(rc_wait == (expect == PSMSGR_OK       ? PSMSGR_OK
                          : expect == PSMSGR_E_FORMAT ? PSMSGR_E_FORMAT
                                                      : PSMSGR_E_TIMEOUT));

        /* The writer must cope as well (the result depends on the damage). */
        int rc = open_writer(CHAN, 40, 3, 0, &w);
        CHECK(rc == PSMSGR_OK || rc == PSMSGR_E_FORMAT || rc == PSMSGR_E_MISMATCH);
        if (rc == PSMSGR_OK) {
            CHECK(psmsgr_state_publish(w, "x", 1, NULL) == PSMSGR_OK);
            psmsgr_state_writer_close(w);
        }
    }
    CHECK(formats > 300); /* the fuzzing mostly produced invalid files */
}

/* ---- ENOSPC ------------------------------------------------------------------------- */

static int write_proc(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/* Needs a small tmpfs: mounted in a private user and mount namespace, where
 * the environment allows one. Exit code 77: skipped. */
static int enospc_child(void)
{
    char dir[600], map[64];
    snprintf(dir, sizeof dir, "%s/small", test_dir);
    if (mkdir(dir, 0700) != 0)
        return 2;
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0)
        return 77;
    snprintf(map, sizeof map, "0 %u 1", (unsigned)uid);
    if (write_proc("/proc/self/setgroups", "deny") != 0 || write_proc("/proc/self/uid_map", map) != 0)
        return 77;
    snprintf(map, sizeof map, "0 %u 1", (unsigned)gid);
    if (write_proc("/proc/self/gid_map", map) != 0)
        return 77;
    if (mount("tmpfs", dir, "tmpfs", 0, "size=256k") != 0)
        return 77;

    psmsgr_state_options o = make_opts(1u << 20, 2, 0);
    o.dir = dir;
    psmsgr_state_writer *w = NULL;
    errno = 0;
    int rc = psmsgr_state_writer_open(CHAN, &o, &w);
    int ok = rc == PSMSGR_E_SYS && errno == ENOSPC;
    /* Nothing is left behind but the lock file. */
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s/psmsgr.%s.state", dir, CHAN);
    ok = ok && access(tmp, F_OK) != 0;
    o.capacity = 1024; /* fits */
    ok = ok && psmsgr_state_writer_open(CHAN, &o, &w) == PSMSGR_OK;
    return ok ? 0 : 1;
}

static void enospc_on_full_tmpfs(void)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0)
        _exit(enospc_child());
    int status = 0;
    REQUIRE(waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    if (WEXITSTATUS(status) == 77) {
        printf("skip enospc_on_full_tmpfs: cannot mount a tmpfs here\n");
        return;
    }
    CHECK(WEXITSTATUS(status) == 0);
}

int main(void)
{
    /* Processes first: fork()ing is simplest while single-threaded. */
    TEST(second_writer_other_process);
    TEST(crash_mid_publish);
    TEST(enospc_on_full_tmpfs);

    TEST(names_are_validated);
    TEST(options_defaults_and_limits);
    TEST(dir_comes_from_env_once);
    TEST(create_then_reuse);
    TEST(mismatch_and_recreate);
    TEST(second_writer_same_process);
    TEST(lazy_reader_attaches_later);
    TEST(read_results_and_sizes);
    TEST(heartbeat_channel);
    TEST(begin_commit_abort);
    TEST(peek_timestamps);
    TEST(minor_version_recreates);
    TEST(invalid_file_needs_recreate);
    TEST(retired_file_is_replaced);
    TEST(attached_flag_once_per_attach);
    TEST(symlinks_are_rejected);
    TEST(missing_dir);
    TEST(fuzz_header);
    TEST(orphaned_file);
    TEST(wait_wakes_on_publish);
    TEST(wait_timeouts);
    TEST(wait_follows_retire);
    TEST(wait_follows_unlink);
    TEST(wait_not_supported_without_notify);
    TEST(wait_interrupted_by_signal);
    return TEST_EXIT();
}
