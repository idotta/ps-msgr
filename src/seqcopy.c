/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#include <string.h>

#if defined(__SANITIZE_THREAD__)
#define PSMI_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define PSMI_TSAN 1
#endif
#endif

#ifdef PSMI_TSAN
/* ThreadSanitizer dynamic annotations, provided by the TSan runtime. */
void AnnotateIgnoreReadsBegin(const char *file, int line);
void AnnotateIgnoreReadsEnd(const char *file, int line);
void AnnotateIgnoreWritesBegin(const char *file, int line);
void AnnotateIgnoreWritesEnd(const char *file, int line);
#endif

/* The seqlock copy races with the other side by design: the sequence
 * counter, not the copy, decides whether the result is used. */
void psmi_seq_copy(void *dst, const void *src, size_t n)
{
    if (n == 0)
        return;
#ifdef PSMI_TSAN
    AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
    AnnotateIgnoreWritesBegin(__FILE__, __LINE__);
#endif
    memcpy(dst, src, n);
#ifdef PSMI_TSAN
    AnnotateIgnoreWritesEnd(__FILE__, __LINE__);
    AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
#endif
}
