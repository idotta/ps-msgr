/* SPDX-License-Identifier: Apache-2.0 */
#include <psmsgr/psmsgr.h>

#include <string.h>

#include "test.h"

static void version_matches_header(void)
{
    CHECK(psmsgr_version() == PSMSGR_VERSION_NUMBER(PSMSGR_VERSION_MAJOR,
                                                    PSMSGR_VERSION_MINOR,
                                                    PSMSGR_VERSION_PATCH));
    CHECK((psmsgr_version() >> 16) == PSMSGR_VERSION_MAJOR);
}

static void strerror_covers_all_codes(void)
{
    const char *unknown = psmsgr_strerror(1);
    CHECK(unknown != NULL);
    CHECK(strcmp(psmsgr_strerror(PSMSGR_OK), "success") == 0);
    for (int code = PSMSGR_E_STATE; code <= PSMSGR_E_INVAL; ++code) {
        const char *msg = psmsgr_strerror(code);
        CHECK(msg != NULL);
        CHECK(strcmp(msg, unknown) != 0);
    }
    CHECK(strcmp(psmsgr_strerror(PSMSGR_E_STATE - 1), unknown) == 0);
}

static void now_ns_is_monotonic(void)
{
    uint64_t prev = psmsgr_now_ns();
    CHECK(prev > 0);
    for (int i = 0; i < 1000; ++i) {
        uint64_t now = psmsgr_now_ns();
        CHECK(now >= prev);
        prev = now;
    }
}

int main(void)
{
    RUN(version_matches_header);
    RUN(strerror_covers_all_codes);
    RUN(now_ns_is_monotonic);
    return TEST_EXIT();
}
