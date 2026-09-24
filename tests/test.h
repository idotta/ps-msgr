/* SPDX-License-Identifier: Apache-2.0 */
/* cmocka 2, plus an assertion for psmsgr result codes.
 *
 * Run a subset with cmocka's filter, e.g. CMOCKA_TEST_FILTER='wait_*'. */
#ifndef PSMSGR_TEST_H
#define PSMSGR_TEST_H

#include <cmocka.h>

#include <psmsgr/psmsgr.h>

/* Asserts that a call returns the expected psmsgr result code, and names
 * both codes on failure. */
#define assert_rc(expr, expected)                                                            \
    do {                                                                                     \
        const int rc_ = (expr), expected_ = (expected);                                      \
        if (rc_ != expected_) {                                                              \
            cmocka_print_error("%s: %d (%s), expected %d (%s)\n", #expr, rc_,                \
                               psmsgr_strerror(rc_), expected_, psmsgr_strerror(expected_)); \
            fail();                                                                          \
        }                                                                                    \
    } while (0)

#endif /* PSMSGR_TEST_H */
