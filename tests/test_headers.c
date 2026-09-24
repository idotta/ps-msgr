/* SPDX-License-Identifier: Apache-2.0 */
/* Headers are self-contained and public struct layouts are pinned: these are
 * ABI, and the bindings (ctypes, P/Invoke) mirror them byte for byte. */
#include <psmsgr/state.h> /* first, in its own block: it must compile alone */

#include <psmsgr/psmsgr.h>

#include <stddef.h>

_Static_assert(sizeof(psmsgr_state_info) == 24, "psmsgr_state_info size");
_Static_assert(offsetof(psmsgr_state_info, generation) == 0, "generation");
_Static_assert(offsetof(psmsgr_state_info, length) == 4, "length");
_Static_assert(offsetof(psmsgr_state_info, timestamp_ns) == 8, "timestamp_ns");
_Static_assert(offsetof(psmsgr_state_info, flags) == 16, "flags");

_Static_assert(sizeof(psmsgr_state_desc) == 16, "psmsgr_state_desc size");

_Static_assert(offsetof(psmsgr_state_options, struct_size) == 0, "struct_size first");
_Static_assert(offsetof(psmsgr_state_options, dir) == 24, "dir after six u32");

int main(void)
{
    return 0;
}
