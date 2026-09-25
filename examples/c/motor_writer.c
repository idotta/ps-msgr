/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Publishes a changing motor status on the "motor" channel.
 *
 *   motor_writer [--dir DIR] [--rate HZ] [--count N]
 */
#define _GNU_SOURCE /* getopt_long */

#include "motor_status.h"

#include <psmsgr/psmsgr.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int usage(void)
{
    fputs("usage: motor_writer [--dir DIR] [--rate HZ] [--count N]\n", stderr);
    return 2;
}

static int fail(int rc)
{
    fprintf(stderr, "motor_writer: %s: %s\n", MOTOR_CHANNEL,
            rc == PSMSGR_E_SYS ? strerror(errno) : psmsgr_strerror(rc));
    return 1;
}

int main(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "dir", required_argument, NULL, 'd' },
        { "rate", required_argument, NULL, 'r' },
        { "count", required_argument, NULL, 'n' },
        { 0 },
    };
    const char *dir = NULL; /* NULL: $PSMSGR_DIR, else /dev/shm */
    double rate = 10;
    uint64_t count = 0; /* 0: until Ctrl-C */
    char *end = "";
    int c;
    while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
        switch (c) {
        case 'd': dir = optarg; break;
        case 'r': rate = strtod(optarg, &end); break;
        case 'n': count = strtoull(optarg, &end, 10); break;
        default:  return usage();
        }
        if (*end != '\0')
            return usage();
    }
    if (optind != argc || !(rate > 0))
        return usage();

    /* No SA_RESTART: Ctrl-C interrupts the sleep. */
    struct sigaction sa = { .sa_handler = on_signal };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0); /* one line per value, also through a pipe */

    psmsgr_state_options opt;
    psmsgr_state_options_init(&opt);
    opt.capacity = sizeof(struct motor_status);
    opt.payload_type = MOTOR_STATUS_V1;
    opt.dir = dir;
    /* This program owns the channel: replace one left behind with another
     * capacity or payload type (e.g. by an older version) instead of failing
     * with MISMATCH. Attached readers move to the new file. */
    opt.flags = PSMSGR_STATE_RECREATE;

    psmsgr_state_writer *w;
    int rc = psmsgr_state_writer_open(MOTOR_CHANNEL, &opt, &w);
    if (rc != PSMSGR_OK)
        return fail(rc);

    const uint64_t period_ns = (uint64_t)(1e9 / rate);
    uint64_t next_ns = psmsgr_now_ns(); /* CLOCK_MONOTONIC, like clock_nanosleep below */
    int status = 0;
    for (uint64_t seq = 1; !stop; seq++) {
        struct motor_status s = {
            .sequence = seq,
            .speed_rpm = 1500.0f + (float)(seq % 100) * 5.0f,
            .current_a = 2.0f + (float)(seq % 8) * 0.25f,
            .temperature_c = 40.0f + (float)(seq % 40) * 0.5f,
        };
        /* Copies the struct into the channel; readers never see half of it. */
        rc = psmsgr_state_publish(w, &s, sizeof s, NULL);
        if (rc != PSMSGR_OK) {
            status = fail(rc);
            break;
        }
        printf("seq=%" PRIu64 " speed=%.1f rpm current=%.2f A temperature=%.1f C\n", s.sequence,
               (double)s.speed_rpm, (double)s.current_a, (double)s.temperature_c);
        if (seq == count)
            break;

        next_ns += period_ns;
        struct timespec next = { .tv_sec = (time_t)(next_ns / 1000000000u),
                                 .tv_nsec = (long)(next_ns % 1000000000u) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    /* Releases the writer lock; the channel and its last value stay, and
     * readers see writer_alive() turn false. */
    psmsgr_state_writer_close(w);
    return status;
}
