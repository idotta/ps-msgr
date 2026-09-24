/* SPDX-License-Identifier: Apache-2.0 */
/*
 * psmsgr-dump: prints a state channel's header, slots, writer liveness and,
 * optionally, a hexdump of the latest value (spec/c-api.md, "Tools").
 *
 * The header and slot fields come from a raw, read-only view of the data
 * file, validated like a reader's attach (state-channel.md §6.1) before any
 * field is trusted. Everything consistent (the latest value, liveness) goes
 * through the public API.
 */
#include <psmsgr/psmsgr.h>

#include "layout.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXIT_INVALID 1 /* channel missing or invalid */
#define EXIT_USAGE   2

#define HEX_MAX          1024u /* payload bytes shown by --hex */
#define BUSY_RETRIES     16
#define WATCH_TIMEOUT_MS 1000  /* also bounds how late an unlink or a dead writer shows */
#define WATCH_POLL_MS    100   /* NO_NOTIFY channels */

static const char usage_text[] =
    "usage: psmsgr-dump <name> [--dir D] [--watch] [--hex]\n"
    "\n"
    "Prints a state channel's header, its slots, writer liveness and the latest\n"
    "value.\n"
    "\n"
    "  --dir D   channel directory (default: $PSMSGR_DIR, else /dev/shm)\n"
    "  --watch   redraw whenever the channel changes; Ctrl-C to stop. A missing\n"
    "            or invalid channel is shown and watched, not an error.\n"
    "  --hex     hexdump the latest payload (at most 1024 bytes)\n"
    "  -h, --help\n"
    "\n"
    "The header and slot fields are a raw snapshot of the file and may be torn\n"
    "while a writer is active. Only the \"value\" line and the hexdump are\n"
    "consistent: they are read through the library, like any reader.\n"
    "\n"
    "Exit status: 0 ok, 1 channel missing or invalid, 2 usage error.\n";

typedef struct options {
    const char *name;
    const char *dir; /* as given; NULL: the library's default */
    bool        watch;
    bool        hex;
} options;

static volatile sig_atomic_t stop;

__attribute__((format(printf, 1, 2))) static void error(const char *fmt, ...)
{
    fflush(stdout);
    fputs("psmsgr-dump: ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/* ---- formatting ------------------------------------------------------------ */

static void fmt_duration(char *buf, size_t n, uint64_t ns)
{
    if (ns < 1000u)
        snprintf(buf, n, "%" PRIu64 " ns", ns);
    else if (ns < 1000000u)
        snprintf(buf, n, "%.1f us", (double)ns / 1e3);
    else if (ns < 1000000000u)
        snprintf(buf, n, "%.1f ms", (double)ns / 1e6);
    else
        snprintf(buf, n, "%.3f s", (double)ns / 1e9);
}

/* Age of a CLOCK_MONOTONIC timestamp, or "?" for one in the future (torn). */
static void fmt_age(char *buf, size_t n, uint64_t timestamp_ns, uint64_t now)
{
    if (timestamp_ns > now)
        snprintf(buf, n, "?");
    else
        fmt_duration(buf, n, now - timestamp_ns);
}

static void fmt_realtime(char *buf, size_t n, uint64_t ns)
{
    time_t secs = (time_t)(ns / 1000000000u);
    struct tm tm;
    char date[64];
    if (localtime_r(&secs, &tm) == NULL
        || strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", &tm) == 0) {
        snprintf(buf, n, "%" PRIu64 " ns", ns);
        return;
    }
    char zone[16] = "";
    (void)strftime(zone, sizeof zone, "%z", &tm);
    snprintf(buf, n, "%s.%03u %s", date, (unsigned)(ns % 1000000000u / 1000000u), zone);
}

static void hexdump(const unsigned char *p, uint32_t len)
{
    for (uint32_t off = 0; off < len; off += 16) {
        uint32_t row = len - off < 16 ? len - off : 16;
        printf("  %08" PRIx32 " ", off);
        for (uint32_t i = 0; i < 16; ++i) {
            if (i == 8)
                putchar(' ');
            if (i < row)
                printf(" %02x", p[off + i]);
            else
                fputs("   ", stdout);
        }
        fputs("  |", stdout);
        for (uint32_t i = 0; i < row; ++i) {
            unsigned char c = p[off + i];
            putchar(c >= 0x20 && c < 0x7f ? c : '.');
        }
        puts("|");
    }
}

/* ---- raw view ------------------------------------------------------------------ */

/* The checks of a reader's attach (§6.1). NULL if the header is valid. */
static const char *header_problem(const psmi_header *h, uint64_t file_bytes)
{
    if (h->magic != PSMI_MAGIC)
        return "bad magic";
    if (h->version_major != PSMI_VERSION_MAJOR)
        return "unsupported major version";
    if (h->header_size != PSMI_HEADER_SIZE)
        return "bad header_size";
    if (h->slot_header_size != PSMI_SLOT_HEADER_SIZE)
        return "bad slot_header_size";
    if (h->slot_count < PSMI_MIN_SLOTS || h->slot_count > PSMI_MAX_SLOTS)
        return "slot_count out of range";
    if (h->capacity > PSMSGR_STATE_MAX_CAPACITY)
        return "capacity out of range";
    if (h->slot_stride != psmi_slot_stride(h->capacity))
        return "slot_stride does not match capacity";
    if (psmi_file_size(h->slot_count, h->slot_stride) > file_bytes)
        return "file shorter than its slots";
    return NULL;
}

static void print_header(const psmi_header *h)
{
    char magic[5];
    memcpy(magic, &h->magic, 4);
    for (int i = 0; i < 4; ++i)
        if (magic[i] < 0x20 || magic[i] >= 0x7f)
            magic[i] = '.';
    magic[4] = '\0';
    char created[96];
    fmt_realtime(created, sizeof created, h->created_realtime_ns);

    printf("magic         0x%08" PRIx32 " \"%s\"\n", h->magic, magic);
    printf("version       %u.%u\n", (unsigned)h->version_major, (unsigned)h->version_minor);
    printf("header_size   %" PRIu32 "\n", h->header_size);
    printf("slot_header   %" PRIu32 "\n", h->slot_header_size);
    printf("slots         %" PRIu32 " x %" PRIu32 " B stride\n", h->slot_count, h->slot_stride);
    printf("capacity      %" PRIu32 "\n", h->capacity);
    printf("payload_type  0x%08" PRIx32 " (%" PRIu32 ")\n", h->payload_type, h->payload_type);
    printf("config_flags  0x%08" PRIx32 "%s\n", h->config_flags,
           (h->config_flags & PSMI_CONFIG_NO_NOTIFY) ? " NO_NOTIFY" : "");
    printf("state         0x%08" PRIx32 "%s\n", h->state,
           (h->state & PSMI_STATE_RETIRED) ? " RETIRED" : "");
    printf("writer_pid    %" PRIu32 "\n", h->writer_pid);
    printf("created       %s\n", created);
}

static void print_latest(uint32_t latest, const psmi_slot *slots, uint32_t slot_count)
{
    if (latest == PSMI_LATEST_NONE) {
        puts("latest        none (nothing published)");
        return;
    }
    uint32_t i   = psmi_latest_slot(latest);
    uint32_t tag = (latest >> 4) & PSMI_LATEST_TAG_MASK;
    printf("latest        0x%08" PRIx32 ": slot %" PRIu32 ", tag %" PRIu32, latest, i, tag);
    if (!psmi_latest_valid(latest, slot_count)) {
        puts("  INVALID: readers get FORMAT");
        return;
    }
    uint32_t seq = slots[i].seq;
    if ((seq & 1u) != 0 || psmi_latest(i, seq) != latest)
        printf("  STALE: slot seq %" PRIu32 " (tag %" PRIu32 "); readers get BUSY",
               seq, (seq >> 1) & PSMI_LATEST_TAG_MASK);
    putchar('\n');
}

static void print_slots(const psmi_slot *slots, const psmi_header *h, uint32_t latest)
{
    uint64_t now = psmsgr_now_ns();
    puts("slot  seq         generation  length      age");
    for (uint32_t i = 0; i < h->slot_count; ++i) {
        const psmi_slot *s = &slots[i];
        char age[32] = "-";
        if (s->seq != 0)
            fmt_age(age, sizeof age, s->timestamp_ns, now);
        char notes[96];
        snprintf(notes, sizeof notes, "%s%s%s",
                 (s->seq & 1u) ? "  odd: being written, or aborted/crashed" : "",
                 s->length > h->capacity ? "  length > capacity" : "",
                 latest != PSMI_LATEST_NONE && psmi_latest_slot(latest) == i ? "  <- latest" : "");
        printf("%-4" PRIu32 "  %-10" PRIu32 "  %-10" PRIu32 "  %-10" PRIu32 "  %-*s%s\n", i,
               s->seq, s->generation, s->length, notes[0] != '\0' ? 10 : 0, age, notes);
    }
}

/* Prints the raw view of the data file. Returns 0 or EXIT_INVALID. */
static int dump_raw(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT)
            error("channel not found: %s", path);
        else
            error("cannot open %s: %s", path, strerror(errno));
        return EXIT_INVALID;
    }
    int rc = EXIT_INVALID;
    psmi_header h;
    psmi_slot slots[PSMI_MAX_SLOTS];
    struct stat st;
    const char *problem = NULL;
    if (fstat(fd, &st) != 0) {
        error("cannot stat %s: %s", path, strerror(errno));
        goto out;
    }
    if (!S_ISREG(st.st_mode)) {
        error("invalid channel file %s: not a regular file", path);
        goto out;
    }
    if (st.st_size < (off_t)sizeof h || pread(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) {
        error("invalid channel file %s: shorter than a header", path);
        goto out;
    }
    print_header(&h);
    if ((problem = header_problem(&h, (uint64_t)st.st_size)) != NULL) {
        error("invalid channel file %s: %s", path, problem);
        goto out;
    }
    /* The header copy is older than the slots read below; reread `latest`
     * just before them to keep the snapshot's window small. */
    uint32_t latest = h.latest;
    if (pread(fd, &latest, sizeof latest, (off_t)offsetof(psmi_header, latest)) != (ssize_t)sizeof latest)
        goto short_read;
    for (uint32_t i = 0; i < h.slot_count; ++i) {
        off_t off = (off_t)(PSMI_HEADER_SIZE + (uint64_t)i * h.slot_stride);
        if (pread(fd, &slots[i], sizeof slots[i], off) != (ssize_t)sizeof slots[i])
            goto short_read;
    }
    print_latest(latest, slots, h.slot_count);
    print_slots(slots, &h, latest);
    rc = 0;
    goto out;
short_read:
    error("%s changed size while being read", path);
out:
    (void)close(fd);
    return rc;
}

/* ---- API view -------------------------------------------------------------- */

static int peek(psmsgr_state_reader *r, psmsgr_state_info *info)
{
    int rc = PSMSGR_E_BUSY;
    for (int i = 0; i < BUSY_RETRIES && rc == PSMSGR_E_BUSY; ++i)
        rc = psmsgr_state_peek(r, info);
    return rc;
}

static void print_liveness(psmsgr_state_reader *r)
{
    int alive = psmsgr_state_writer_alive(r);
    if (alive == 1)
        puts("writer        alive");
    else if (alive == 0)
        puts("writer        not running");
    else if (alive == PSMSGR_E_SYS)
        printf("writer        unknown: %s\n", strerror(errno));
    else
        printf("writer        unknown: %s\n", psmsgr_strerror(alive));
}

static void print_value(psmsgr_state_reader *r)
{
    psmsgr_state_info info;
    int rc = peek(r, &info);
    if (rc != PSMSGR_OK) {
        printf("value         %s\n", psmsgr_strerror(rc));
        return;
    }
    char age[32];
    fmt_age(age, sizeof age, info.timestamp_ns, psmsgr_now_ns());
    printf("value         generation %" PRIu32 ", length %" PRIu32 ", age %s\n", info.generation,
           info.length, age);
}

static void print_hex(psmsgr_state_reader *r)
{
    psmsgr_state_desc desc;
    int rc = psmsgr_state_describe(r, &desc);
    if (rc != PSMSGR_OK) {
        printf("payload       %s\n", psmsgr_strerror(rc));
        return;
    }
    /* capacity <= 16 MiB; the whole payload is copied so the read is atomic. */
    unsigned char *buf = desc.capacity != 0 ? malloc(desc.capacity) : NULL;
    if (desc.capacity != 0 && buf == NULL) {
        puts("payload       out of memory");
        return;
    }
    psmsgr_state_info info;
    rc = PSMSGR_E_BUSY;
    for (int i = 0; i < BUSY_RETRIES && rc == PSMSGR_E_BUSY; ++i)
        rc = psmsgr_state_read(r, buf, desc.capacity, &info);
    if (rc != PSMSGR_OK) {
        printf("payload       %s\n", psmsgr_strerror(rc));
    } else {
        printf("payload       generation %" PRIu32 ", %" PRIu32 " bytes\n", info.generation,
               info.length);
        hexdump(buf, info.length < HEX_MAX ? info.length : HEX_MAX);
        if (info.length > HEX_MAX)
            printf("  ... %" PRIu32 " more bytes\n", info.length - HEX_MAX);
    }
    free(buf);
}

static int dump(psmsgr_state_reader *r, const options *o, const char *path)
{
    printf("channel       %s (%s)\n", o->name, path);
    int rc = dump_raw(path);
    print_liveness(r);
    if (rc == 0) {
        print_value(r);
        if (o->hex)
            print_hex(r);
    }
    fflush(stdout);
    return rc;
}

/* ---- watch ----------------------------------------------------------------- */

/* What a redraw depends on, apart from the ages. */
typedef struct watch_key {
    dev_t    dev;
    ino_t    ino;
    int      peek_rc;
    uint32_t generation;
    int      alive;
} watch_key;

static watch_key watch_key_of(psmsgr_state_reader *r, const char *path)
{
    watch_key k = { 0 };
    struct stat st;
    if (lstat(path, &st) == 0) {
        k.dev = st.st_dev;
        k.ino = st.st_ino;
    }
    psmsgr_state_info info;
    k.peek_rc    = peek(r, &info);
    k.generation = k.peek_rc == PSMSGR_OK ? info.generation : 0;
    k.alive      = psmsgr_state_writer_alive(r);
    return k;
}

static bool watch_key_equal(const watch_key *a, const watch_key *b)
{
    return a->dev == b->dev && a->ino == b->ino && a->peek_rc == b->peek_rc
           && a->generation == b->generation && a->alive == b->alive;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    (void)nanosleep(&ts, NULL); /* EINTR: the caller checks `stop` */
}

static int watch(psmsgr_state_reader *r, const options *o, const char *path)
{
    /* No SA_RESTART: SIGINT must interrupt the wait. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigemptyset(&sa.sa_mask);
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    bool tty = isatty(STDOUT_FILENO) == 1;
    watch_key key = watch_key_of(r, path);
    for (bool first = true; !stop; first = false) {
        if (tty)
            fputs("\033[H\033[2J", stdout);
        else if (!first)
            putchar('\n');
        (void)dump(r, o, path);

        watch_key next = key;
        while (!stop && watch_key_equal(&next, &key)) {
            int rc = psmsgr_state_wait(r, key.generation, WATCH_TIMEOUT_MS);
            if (rc != PSMSGR_OK && rc != PSMSGR_E_TIMEOUT && rc != PSMSGR_E_INTR)
                sleep_ms(WATCH_POLL_MS); /* NOTSUP (NO_NOTIFY), FORMAT, SYS */
            if (!stop)
                next = watch_key_of(r, path);
        }
        key = next;
    }
    return 0;
}

/* ---- main ------------------------------------------------------------------ */

static int usage_error(const char *msg)
{
    if (msg != NULL)
        fprintf(stderr, "psmsgr-dump: %s\n", msg);
    fputs(usage_text, stderr);
    return EXIT_USAGE;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "dir", required_argument, NULL, 'd' },
        { "watch", no_argument, NULL, 'w' },
        { "hex", no_argument, NULL, 'x' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    options o = { 0 };
    int c;
    while ((c = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (c) {
        case 'd': o.dir = optarg; break;
        case 'w': o.watch = true; break;
        case 'x': o.hex = true; break;
        case 'h': fputs(usage_text, stdout); return 0;
        default:  return usage_error(NULL);
        }
    }
    if (optind != argc - 1)
        return usage_error(optind == argc ? "missing channel name" : "too many arguments");
    o.name = argv[optind];

    /* Validates the name (and dir) before it goes into a path. */
    psmsgr_state_reader *r = NULL;
    int rc = psmsgr_state_reader_open(o.name, o.dir, &r);
    if (rc == PSMSGR_E_INVAL)
        return usage_error("invalid channel name or directory");
    if (rc != PSMSGR_OK) {
        error("%s", strerror(errno));
        return EXIT_INVALID;
    }

    /* The library's directory rule (state-channel.md §2). */
    const char *dir = o.dir;
    if (dir == NULL) {
        dir = getenv("PSMSGR_DIR");
        if (dir == NULL || dir[0] == '\0')
            dir = "/dev/shm";
    }
    size_t len = strlen(dir) + strlen(o.name) + sizeof "/psmsgr..state";
    char *path = malloc(len);
    if (path == NULL) {
        psmsgr_state_reader_close(r);
        error("out of memory");
        return EXIT_INVALID;
    }
    snprintf(path, len, "%s/psmsgr.%s.state", dir, o.name);

    rc = o.watch ? watch(r, &o, path) : dump(r, &o, path);
    free(path);
    psmsgr_state_reader_close(r);
    return rc;
}
