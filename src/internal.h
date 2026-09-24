/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Library-internal declarations. Everything here has hidden visibility, so
 * none of it is exported from libpsmsgr.so; tests that need it link the
 * static library.
 */
#ifndef PSMSGR_INTERNAL_H
#define PSMSGR_INTERNAL_H

#include <psmsgr/state.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

static inline uint64_t psmi_clock_ns(clockid_t clock)
{
    struct timespec ts;
    /* Cannot fail for the clocks used here with a valid pointer. */
    (void)clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* memcpy for the racy side of a seqlock (state-channel.md §5.4). Defined in
 * its own translation unit so that, without LTO, the compiler can neither
 * inline it nor move its accesses across the fences around the call site.
 * Annotated for ThreadSanitizer. */
void psmi_seq_copy(void *dst, const void *src, size_t n);

/* ---- test hooks ---------------------------------------------------------- */

/* Fault injection for the lock identity check (state-channel.md §4): called
 * between opening the lock file and locking it. NULL outside of tests. */
extern void (*psmi_test_lock_opened)(void);

/* Sets the generation the writer's next publish will carry (must not be 0). */
void psmi_writer_set_generation(psmsgr_state_writer *w, uint32_t next);

#endif /* PSMSGR_INTERNAL_H */
