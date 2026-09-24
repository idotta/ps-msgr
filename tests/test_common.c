/* SPDX-License-Identifier: Apache-2.0 */
#include <psmsgr/psmsgr.h>

#include <string.h>

#include "test.h"

static void version_matches_header(void **state)
{
    assert_uint_equal(
        psmsgr_version(),
        PSMSGR_VERSION_NUMBER(PSMSGR_VERSION_MAJOR, PSMSGR_VERSION_MINOR, PSMSGR_VERSION_PATCH));
    assert_uint_equal(psmsgr_version() >> 16, PSMSGR_VERSION_MAJOR);
}

static void strerror_covers_all_codes(void **state)
{
    const char *unknown = psmsgr_strerror(1);
    assert_non_null(unknown);
    assert_string_equal(psmsgr_strerror(PSMSGR_OK), "success");
    for (int code = PSMSGR_E_STATE; code <= PSMSGR_E_INVAL; ++code) {
        const char *msg = psmsgr_strerror(code);
        assert_non_null(msg);
        assert_string_not_equal(msg, unknown);
    }
    assert_string_equal(psmsgr_strerror(PSMSGR_E_STATE - 1), unknown);
}

static void now_ns_is_monotonic(void **state)
{
    uint64_t prev = psmsgr_now_ns();
    assert_true(prev > 0);
    for (int i = 0; i < 1000; ++i) {
        uint64_t now = psmsgr_now_ns();
        assert_true(now >= prev);
        prev = now;
    }
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(version_matches_header),
        cmocka_unit_test(strerror_covers_all_codes),
        cmocka_unit_test(now_ns_is_monotonic),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
