# C API — `libpsmsgr`

Status: **draft**. Protocol semantics are defined in
[state-channel.md](state-channel.md); this document defines the interface.

## Conventions

- C11. Headers compile as C++ as well (`extern "C"`).
- Every function that can fail returns `int`: `PSMSGR_OK` (0) or a negative
  `PSMSGR_E_*` code. When the code is `PSMSGR_E_SYS`, `errno` holds the error
  from the failing system call.
- Handles are opaque. A handle is **not** thread-safe: synchronize externally
  or use one handle per thread. Different handles are independent, including
  handles to the same channel.
- Handles are not valid in a `fork()`ed child. All descriptors are
  `O_CLOEXEC`.
- Nothing allocates or takes a lock on the hot path (`publish`,
  `begin`/`commit`, `read`, `peek`). All allocation happens in `*_open`.
- `read` and `peek` make no syscalls while attached. `publish`/`commit`
  make at most two: the notify wake, and `clock_gettime` where the vDSO
  can't serve it (always on the AM335x, about 1.3 µs; see
  state-channel.md §8).
- The library never writes to stdout/stderr, never installs signal handlers,
  never calls `exit`/`abort`, and keeps no global mutable state apart from
  handles.
- Only symbols prefixed `psmsgr_` are exported (`-fvisibility=hidden` plus
  an export macro).
- ABI extensibility: structs passed *in* start with `struct_size`, set by
  their `*_init` function. The library reads only the fields it knows.

## `<psmsgr/psmsgr.h>`

```c
/* Library major == SONAME number. The first release is 1.0.0; until then the
 * API/ABI may change freely. */
#define PSMSGR_VERSION_MAJOR 1
#define PSMSGR_VERSION_MINOR 0
#define PSMSGR_VERSION_PATCH 0

/* (major << 16) | (minor << 8) | patch of the loaded library. */
uint32_t    psmsgr_version(void);

/* Static, never NULL. Unknown codes yield "unknown error". */
const char *psmsgr_strerror(int code);

/* CLOCK_MONOTONIC in nanoseconds: the clock used for timestamp_ns. */
uint64_t    psmsgr_now_ns(void);

enum {
    PSMSGR_OK              =   0,
    PSMSGR_E_INVAL         =  -1,  /* bad argument or channel name            */
    PSMSGR_E_SYS           =  -2,  /* system call failed; see errno           */
    PSMSGR_E_NODATA        =  -3,  /* channel absent or nothing published yet */
    PSMSGR_E_TOOSMALL      =  -4,  /* buffer too small; info->length is valid */
    PSMSGR_E_TOOBIG        =  -5,  /* payload larger than capacity            */
    PSMSGR_E_BUSY          =  -6,  /* read retries exhausted; transient, retry */
    PSMSGR_E_TIMEOUT       =  -7,
    PSMSGR_E_INTR          =  -8,  /* wait interrupted by a signal            */
    PSMSGR_E_WRITER_EXISTS =  -9,  /* another writer holds the channel        */
    PSMSGR_E_MISMATCH      = -10,  /* existing channel has other geometry     */
    PSMSGR_E_FORMAT        = -11,  /* bad magic/version/size, corrupt file    */
    PSMSGR_E_NOTSUP        = -12,  /* e.g. wait on a NO_NOTIFY channel        */
    PSMSGR_E_STATE         = -13,  /* call not valid now, e.g. commit w/o begin */
};
```

## `<psmsgr/state.h>`

### Types

```c
#define PSMSGR_NAME_MAX            64
#define PSMSGR_STATE_MAX_CAPACITY  (16u << 20)
#define PSMSGR_STATE_DEFAULT_SLOTS 3

typedef struct psmsgr_state_writer psmsgr_state_writer;
typedef struct psmsgr_state_reader psmsgr_state_reader;

enum {
    PSMSGR_STATE_RECREATE  = 1u << 0,  /* replace an incompatible existing channel */
    PSMSGR_STATE_NO_NOTIFY = 1u << 1,  /* no futex wake per publish; wait() -> NOTSUP */
};

typedef struct psmsgr_state_options {
    uint32_t    struct_size;   /* set by psmsgr_state_options_init */
    uint32_t    capacity;      /* max payload bytes, 0 .. PSMSGR_STATE_MAX_CAPACITY */
    uint32_t    slot_count;    /* 2 .. 16; default PSMSGR_STATE_DEFAULT_SLOTS */
    uint32_t    payload_type;  /* application tag; default 0 */
    uint32_t    mode;          /* file mode; default 0644 */
    uint32_t    flags;         /* PSMSGR_STATE_* */
    const char *dir;           /* NULL: $PSMSGR_DIR, else /dev/shm */
} psmsgr_state_options;

enum {
    PSMSGR_INFO_ATTACHED = 1u << 0,  /* first result from a newly (re)attached file */
};

/* Result of read/peek. */
typedef struct psmsgr_state_info {
    uint32_t generation;    /* change token, never 0 */
    uint32_t length;        /* payload length */
    uint64_t timestamp_ns;  /* CLOCK_MONOTONIC at publish */
    uint32_t flags;         /* PSMSGR_INFO_* */
    uint32_t reserved;      /* 0 */
} psmsgr_state_info;

/* Constant properties of an attached channel. */
typedef struct psmsgr_state_desc {
    uint32_t capacity;
    uint32_t slot_count;
    uint32_t payload_type;
    uint32_t flags;         /* PSMSGR_STATE_NO_NOTIFY if set on the channel */
} psmsgr_state_desc;
```

### Writer

```c
void psmsgr_state_options_init(psmsgr_state_options *opt);

/* Opens or creates the channel and takes the writer lock (state-channel.md §5.1).
 * opt NULL: psmsgr_state_options_init() defaults.
 * Errors: INVAL, WRITER_EXISTS, MISMATCH, FORMAT,
 *         SYS (e.g. ENOENT: dir missing, ENOSPC: tmpfs full, EACCES, ELOOP: symlink). */
int  psmsgr_state_writer_open(const char *name,
                              const psmsgr_state_options *opt,
                              psmsgr_state_writer **out);

/* Aborts an open begin, releases the lock; the channel and its last value
 * remain. NULL is a no-op. */
void psmsgr_state_writer_close(psmsgr_state_writer *w);

/* Copies and publishes a value. generation may be NULL.
 * Errors: TOOBIG, STATE (a begin is open). */
int  psmsgr_state_publish(psmsgr_state_writer *w, const void *data,
                          uint32_t len, uint32_t *generation);

/* Zero-copy publish. *buf points at `capacity` writable bytes (32-byte
 * aligned) until commit/abort. Exactly one of commit/abort must follow. */
int  psmsgr_state_begin (psmsgr_state_writer *w, void **buf);
int  psmsgr_state_commit(psmsgr_state_writer *w, uint32_t len, uint32_t *generation);
int  psmsgr_state_abort (psmsgr_state_writer *w);

uint32_t psmsgr_state_writer_capacity(const psmsgr_state_writer *w);
```

### Reader

```c
/* Always succeeds for a valid name; attaches lazily (state-channel.md §6.1).
 * dir: NULL -> $PSMSGR_DIR, else /dev/shm. Errors: INVAL, SYS (ENOMEM). */
int  psmsgr_state_reader_open(const char *name, const char *dir,
                              psmsgr_state_reader **out);
void psmsgr_state_reader_close(psmsgr_state_reader *r);

/* Copies the latest value into buf. info must not be NULL.
 * OK | NODATA | TOOSMALL (nothing copied, info->length set) | BUSY | FORMAT
 * | SYS (the channel file cannot be opened, e.g. EACCES, ELOOP). */
int  psmsgr_state_read(psmsgr_state_reader *r, void *buf, uint32_t size,
                       psmsgr_state_info *info);

/* Generation, length and timestamp of the latest value without copying it.
 * No syscalls while attached. OK | NODATA | BUSY | FORMAT | SYS. */
int  psmsgr_state_peek(psmsgr_state_reader *r, psmsgr_state_info *info);

/* Blocks until the generation differs from last_generation (0 = "any value").
 * timeout_ms < 0: infinite, 0: poll once.
 * OK | TIMEOUT | INTR | NOTSUP | FORMAT | SYS. */
int  psmsgr_state_wait(psmsgr_state_reader *r, uint32_t last_generation,
                       int32_t timeout_ms);

/* 1 if a writer currently holds the channel, 0 if not, <0 on error.
 * Syscalls; also runs the orphan identity check (state-channel.md §6.2) and
 * reattaches if the file was replaced behind the library's back. */
int  psmsgr_state_writer_alive(psmsgr_state_reader *r);

/* Constant channel properties. OK | NODATA (not attached) | FORMAT | SYS. */
int  psmsgr_state_describe(psmsgr_state_reader *r, psmsgr_state_desc *desc);
```

### Management

```c
/* Retires and deletes the channel (state-channel.md §7).
 * OK | NODATA (absent) | WRITER_EXISTS | INVAL | SYS. */
int  psmsgr_state_unlink(const char *name, const char *dir);
```

## Usage (non-normative)

```c
/* writer */
psmsgr_state_options o;
psmsgr_state_options_init(&o);
o.capacity     = sizeof(struct imu_sample);
o.payload_type = IMU_SAMPLE_V1;

psmsgr_state_writer *w;
if (psmsgr_state_writer_open("imu", &o, &w) != PSMSGR_OK) { /* ... */ }
struct imu_sample s = sample();
psmsgr_state_publish(w, &s, sizeof s, NULL);

/* reader: consume only changes, detect a stale producer */
psmsgr_state_reader *r;
psmsgr_state_reader_open("imu", NULL, &r);
uint32_t seen = 0;
for (;;) {
    psmsgr_state_info i;
    if (psmsgr_state_peek(r, &i) == PSMSGR_OK) {
        if (psmsgr_now_ns() - i.timestamp_ns > 100000000ull) { /* >100 ms old */ }
        if (i.generation != seen &&
            psmsgr_state_read(r, &s, sizeof s, &i) == PSMSGR_OK)
            seen = i.generation;               /* use s */
    }
    /* sleep, or: psmsgr_state_wait(r, seen, 500); */
}
```

## Tools

`psmsgr-dump <name> [--dir D] [--watch] [--hex]` prints a channel's header,
its slots (seq, generation, age, length), writer liveness and, optionally,
a hexdump of the latest payload. It is built on the public API plus a
read-only raw header view, and it is shipped in `psmsgr-tools` for
debugging on the target.

- **Raw view.** The data file is opened `O_RDONLY | O_NOFOLLOW` and read
  with `pread`, never written. The header passes the attach checks of
  state-channel.md §6.1 before any other field is used, so a corrupt file
  is reported, never crashed on. The raw fields are a snapshot and may be
  torn while a writer is active.
- **`latest`** is shown decoded (slot index, tag). A tag that doesn't match
  the named slot's current `seq`, or an odd `seq` there, is flagged as
  stale: a reader gets `BUSY` until the writer publishes again. An odd
  `seq` in any slot means a write in progress, or one left by an abort or
  a crash.
- **Consistent values** come only through the API: the latest value's
  generation, length and age (`peek`), writer liveness (`writer_alive`), and
  the `--hex` payload (`read`, at most the first 1 KiB shown).
- **`--watch`** redraws whenever the value, the writer's liveness or the file
  changes: it blocks in `psmsgr_state_wait`, polls on `NO_NOTIFY` channels,
  and exits 0 on `SIGINT` or `SIGTERM`. A missing or invalid channel is
  shown and watched, not an error.
- **Exit status:** 0 ok, 1 channel missing or invalid, 2 usage error
  (including an invalid name).
