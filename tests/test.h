/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal test harness: no dependencies, runs unchanged under qemu. */
#ifndef PSMSGR_TEST_H
#define PSMSGR_TEST_H

#include <stdio.h>
#include <stdlib.h>

static int test_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                   \
            ++test_failures;                                                  \
        }                                                                     \
    } while (0)

/* Like CHECK, but returns from the (void) test function on failure. */
#define REQUIRE(cond)                                                         \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: REQUIRE failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                   \
            ++test_failures;                                                  \
            return;                                                           \
        }                                                                     \
    } while (0)

#define RUN(test_fn)                                    \
    do {                                                \
        int before_ = test_failures;                    \
        test_fn();                                      \
        printf("%s %s\n",                               \
               test_failures == before_ ? "ok  " : "FAIL", \
               #test_fn);                               \
    } while (0)

#define TEST_EXIT() (test_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE)

#endif /* PSMSGR_TEST_H */
