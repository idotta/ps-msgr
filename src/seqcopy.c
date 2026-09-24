/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#include <string.h>

/* The seqlock copy races with the other side by design: the sequence
 * counter, not the copy, decides whether the result is used. */
void psmi_seq_copy(void *dst, const void *src, size_t n)
{
    if (n != 0)
        memcpy(dst, src, n);
}
