/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Prints the motor status published on the "motor" channel, one line per
 * value, and says so when the writer stalls or goes away.
 *
 *   motor_reader [--dir DIR] [--count N] [--timeout MS]
 */
#define _GNU_SOURCE /* getopt_long */

#include "motor_status.h"

#include <psmsgr/psmsgr.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int usage(void)
{
    fputs("usage: motor_reader [--dir DIR] [--count N] [--timeout MS]\n", stderr);
    return 2;
}

static int fail(int rc)
{
    fprintf(stderr, "motor_reader: %s: %s\n", MOTOR_CHANNEL,
            rc == PSMSGR_E_SYS ? strerror(errno) : psmsgr_strerror(rc));
    return 1;
}

/* PSMSGR_INFO_ATTACHED marks the first result from a newly attached channel
 * file: at the first value, and again whenever the writer recreated the
 * channel. The bytes are only a motor_status if the payload type says so. */
static bool payload_ok(psmsgr_state_reader *r, const psmsgr_state_info *info)
{
    psmsgr_state_desc desc;
    if ((info->flags & PSMSGR_INFO_ATTACHED) && psmsgr_state_describe(r, &desc) == PSMSGR_OK &&
        desc.payload_type != MOTOR_STATUS_V1) {
        fprintf(stderr,
                "motor_reader: %s: payload type 0x%08" PRIx32
                ", expected 0x%08x (MOTOR_STATUS_V1)\n",
                MOTOR_CHANNEL, desc.payload_type, MOTOR_STATUS_V1);
        return false;
    }
    if (info->length != sizeof(struct motor_status)) {
        fprintf(stderr, "motor_reader: %s: payload of %" PRIu32 " bytes, expected %zu\n",
                MOTOR_CHANNEL, info->length, sizeof(struct motor_status));
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "dir", required_argument, NULL, 'd' },
        { "count", required_argument, NULL, 'n' },
        { "timeout", required_argument, NULL, 't' },
        { 0 },
    };
    const char *dir = NULL; /* NULL: $PSMSGR_DIR, else /dev/shm */
    uint64_t count = 0;     /* 0: until Ctrl-C */
    long timeout_ms = 300;  /* 3 periods of a writer at 10 Hz */
    char *end = "";
    int c;
    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 'd': dir = optarg; break;
        case 'n': count = strtoull(optarg, &end, 10); break;
        case 't': timeout_ms = strtol(optarg, &end, 10); break;
        default:  return usage();
        }
        if (*end != '\0')
            return usage();
    }
    if (optind != argc || timeout_ms <= 0 || timeout_ms > INT32_MAX)
        return usage();

    /* No SA_RESTART: Ctrl-C interrupts the wait (PSMSGR_E_INTR). */
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0); /* one line per value, also through a pipe */

    /* Succeeds whether or not the writer has started: the reader attaches
     * to the channel file once it exists. */
    psmsgr_state_reader *r;
    int rc = psmsgr_state_reader_open(MOTOR_CHANNEL, dir, &r);
    if (rc != PSMSGR_OK)
        return fail(rc);

    int status = 0;
    uint32_t seen = 0; /* generation of the last value printed; 0: none */
    for (uint64_t n = 0; !stop && (count == 0 || n < count);) {
        psmsgr_state_info info;
        rc = psmsgr_state_wait(r, seen, (int32_t)timeout_ms);
        if (rc == PSMSGR_E_TIMEOUT) {
            /* No new value in time. peek gives the age of the last one without
             * copying it; writer_alive tells a stalled writer from a dead one. */
            if (psmsgr_state_peek(r, &info) != PSMSGR_OK) {
                puts("waiting for the writer");
            } else if (!payload_ok(r, &info)) {
                status = 1;
                break;
            } else {
                printf("%s: last value %.1f s old\n",
                       psmsgr_state_writer_alive(r) == 1 ? "stale" : "writer gone",
                       (double)(psmsgr_now_ns() - info.timestamp_ns) / 1e9);
            }
            continue;
        }
        if (rc == PSMSGR_E_INTR)
            continue;
        if (rc != PSMSGR_OK) {
            status = fail(rc);
            break;
        }

        /* Copies the latest value straight into the struct: no torn values,
         * no allocation. TOOSMALL: a larger payload, nothing copied, but info
         * is set, so payload_ok reports it. */
        struct motor_status s;
        rc = psmsgr_state_read(r, &s, sizeof s, &info);
        if (rc == PSMSGR_E_NODATA || rc == PSMSGR_E_BUSY)
            continue;
        if (rc != PSMSGR_OK && rc != PSMSGR_E_TOOSMALL) {
            status = fail(rc);
            break;
        }
        if (!payload_ok(r, &info)) {
            status = 1;
            break;
        }
        seen = info.generation;
        printf("seq=%" PRIu64 " speed=%.1f rpm current=%.2f A temperature=%.1f C\n", s.sequence,
               (double)s.speed_rpm, (double)s.current_a, (double)s.temperature_c);
        n++;
    }

    psmsgr_state_reader_close(r);
    return status;
}
