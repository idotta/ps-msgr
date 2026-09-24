/* SPDX-License-Identifier: Apache-2.0 */
/*
 * State channels: latest-value publish/subscribe over shared memory.
 * Interface: spec/c-api.md. Protocol: spec/state-channel.md.
 */
#ifndef PSMSGR_STATE_H
#define PSMSGR_STATE_H

#include <psmsgr/psmsgr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSMSGR_NAME_MAX            64
#define PSMSGR_STATE_MAX_CAPACITY  (16u << 20)
#define PSMSGR_STATE_DEFAULT_SLOTS 3

typedef struct psmsgr_state_writer psmsgr_state_writer;
typedef struct psmsgr_state_reader psmsgr_state_reader;

enum {
    PSMSGR_STATE_RECREATE  = 1u << 0,  /* replace an incompatible existing channel     */
    PSMSGR_STATE_NO_NOTIFY = 1u << 1,  /* no futex wake per publish; wait() -> NOTSUP */
};

enum {
    PSMSGR_INFO_ATTACHED = 1u << 0,    /* first result from a newly (re)attached file */
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

/* ---- writer ------------------------------------------------------------ */

PSMSGR_API void psmsgr_state_options_init(psmsgr_state_options *opt);

/* Opens or creates the channel and takes the writer lock (state-channel.md §5.1).
 * Errors: INVAL, WRITER_EXISTS, MISMATCH, FORMAT,
 *         SYS (e.g. ENOENT: dir missing, ENOSPC: tmpfs full, EACCES, ELOOP: symlink). */
PSMSGR_API int psmsgr_state_writer_open(const char *name,
                                        const psmsgr_state_options *opt,
                                        psmsgr_state_writer **out);

/* Aborts an open begin, releases the lock; the channel and its last value
 * remain. NULL is a no-op. */
PSMSGR_API void psmsgr_state_writer_close(psmsgr_state_writer *w);

/* Copies and publishes a value. generation may be NULL.
 * Errors: TOOBIG, STATE (a begin is open). */
PSMSGR_API int psmsgr_state_publish(psmsgr_state_writer *w, const void *data,
                                    uint32_t len, uint32_t *generation);

/* Zero-copy publish. *buf points at `capacity` writable bytes (32-byte
 * aligned) until commit/abort. Exactly one of commit/abort must follow. */
PSMSGR_API int psmsgr_state_begin(psmsgr_state_writer *w, void **buf);
PSMSGR_API int psmsgr_state_commit(psmsgr_state_writer *w, uint32_t len,
                                   uint32_t *generation);
PSMSGR_API int psmsgr_state_abort(psmsgr_state_writer *w);

PSMSGR_API uint32_t psmsgr_state_writer_capacity(const psmsgr_state_writer *w);

/* ---- reader ------------------------------------------------------------ */

/* Always succeeds for a valid name; attaches lazily (state-channel.md §6.1).
 * dir: NULL -> $PSMSGR_DIR, else /dev/shm. Errors: INVAL, SYS (ENOMEM). */
PSMSGR_API int psmsgr_state_reader_open(const char *name, const char *dir,
                                        psmsgr_state_reader **out);
PSMSGR_API void psmsgr_state_reader_close(psmsgr_state_reader *r);

/* Copies the latest value into buf. info must not be NULL.
 * OK | NODATA | TOOSMALL (nothing copied, info->length set) | BUSY | FORMAT. */
PSMSGR_API int psmsgr_state_read(psmsgr_state_reader *r, void *buf, uint32_t size,
                                 psmsgr_state_info *info);

/* Generation, length and timestamp of the latest value without copying it.
 * No syscalls while attached. OK | NODATA | BUSY | FORMAT. */
PSMSGR_API int psmsgr_state_peek(psmsgr_state_reader *r, psmsgr_state_info *info);

/* Blocks until the generation differs from last_generation (0 = "any value").
 * timeout_ms < 0: infinite, 0: poll once.
 * OK | TIMEOUT | INTR | NOTSUP | FORMAT. */
PSMSGR_API int psmsgr_state_wait(psmsgr_state_reader *r, uint32_t last_generation,
                                 int32_t timeout_ms);

/* 1 if a writer currently holds the channel, 0 if not, <0 on error.
 * Syscalls; also runs the orphan identity check (state-channel.md §6.2) and
 * reattaches if the file was replaced behind the library's back. */
PSMSGR_API int psmsgr_state_writer_alive(psmsgr_state_reader *r);

/* Constant channel properties. OK | NODATA (not attached) | FORMAT. */
PSMSGR_API int psmsgr_state_describe(psmsgr_state_reader *r, psmsgr_state_desc *desc);

/* ---- management -------------------------------------------------------- */

/* Retires and deletes the channel (state-channel.md §7).
 * OK | NODATA (absent) | WRITER_EXISTS | INVAL | SYS. */
PSMSGR_API int psmsgr_state_unlink(const char *name, const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* PSMSGR_STATE_H */
