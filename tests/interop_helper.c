/* SPDX-License-Identifier: Apache-2.0 */
/* The C side of the binding tests (bindings/python/tests); not run by CTest.
 *
 *   interop_helper layout
 *     Prints the public struct layouts and constants, one "kind key value"
 *     per line, so that the bindings compare their mirrors with this header.
 *   interop_helper write DIR NAME CAPACITY FLAGS PAYLOAD...
 *     Opens a writer with PSMSGR_STATE_* FLAGS, publishes each PAYLOAD's
 *     bytes and prints its generation, then keeps the writer open until
 *     stdin reaches EOF. Exits 1 with the error on stderr if a call fails.
 */
#include <psmsgr/psmsgr.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE(type)          printf("sizeof %s %zu\n", #type, sizeof(type))
#define OFFSET(type, field) printf("offsetof %s.%s %zu\n", #type, #field, offsetof(type, field))
#define CONST(name)         printf("const %s %lld\n", #name, (long long)(name))

static int layout(void)
{
    SIZE(psmsgr_state_options);
    OFFSET(psmsgr_state_options, struct_size);
    OFFSET(psmsgr_state_options, capacity);
    OFFSET(psmsgr_state_options, slot_count);
    OFFSET(psmsgr_state_options, payload_type);
    OFFSET(psmsgr_state_options, mode);
    OFFSET(psmsgr_state_options, flags);
    OFFSET(psmsgr_state_options, dir);
    SIZE(psmsgr_state_info);
    OFFSET(psmsgr_state_info, generation);
    OFFSET(psmsgr_state_info, length);
    OFFSET(psmsgr_state_info, timestamp_ns);
    OFFSET(psmsgr_state_info, flags);
    OFFSET(psmsgr_state_info, reserved);
    SIZE(psmsgr_state_desc);
    OFFSET(psmsgr_state_desc, capacity);
    OFFSET(psmsgr_state_desc, slot_count);
    OFFSET(psmsgr_state_desc, payload_type);
    OFFSET(psmsgr_state_desc, flags);

    CONST(PSMSGR_VERSION_MAJOR);
    CONST(PSMSGR_VERSION_MINOR);
    CONST(PSMSGR_OK);
    CONST(PSMSGR_E_INVAL);
    CONST(PSMSGR_E_SYS);
    CONST(PSMSGR_E_NODATA);
    CONST(PSMSGR_E_TOOSMALL);
    CONST(PSMSGR_E_TOOBIG);
    CONST(PSMSGR_E_BUSY);
    CONST(PSMSGR_E_TIMEOUT);
    CONST(PSMSGR_E_INTR);
    CONST(PSMSGR_E_WRITER_EXISTS);
    CONST(PSMSGR_E_MISMATCH);
    CONST(PSMSGR_E_FORMAT);
    CONST(PSMSGR_E_NOTSUP);
    CONST(PSMSGR_E_STATE);
    CONST(PSMSGR_NAME_MAX);
    CONST(PSMSGR_STATE_MAX_CAPACITY);
    CONST(PSMSGR_STATE_DEFAULT_SLOTS);
    CONST(PSMSGR_STATE_RECREATE);
    CONST(PSMSGR_STATE_NO_NOTIFY);
    CONST(PSMSGR_INFO_ATTACHED);
    return 0;
}

static int fail(const char *what, int rc)
{
    fprintf(stderr, "interop_helper: %s: %s\n", what, psmsgr_strerror(rc));
    return 1;
}

static int write_values(int argc, char **argv)
{
    psmsgr_state_options o;
    psmsgr_state_options_init(&o);
    o.dir = argv[0];
    o.capacity = (uint32_t)strtoul(argv[2], NULL, 0);
    o.flags = (uint32_t)strtoul(argv[3], NULL, 0);
    psmsgr_state_writer *w = NULL;
    int rc = psmsgr_state_writer_open(argv[1], &o, &w);
    if (rc != PSMSGR_OK)
        return fail("open", rc);
    for (int i = 4; i < argc; ++i) {
        uint32_t gen;
        rc = psmsgr_state_publish(w, argv[i], (uint32_t)strlen(argv[i]), &gen);
        if (rc != PSMSGR_OK) {
            psmsgr_state_writer_close(w);
            return fail("publish", rc);
        }
        printf("%u\n", (unsigned)gen);
    }
    fflush(stdout);
    while (getchar() != EOF) {
    }
    psmsgr_state_writer_close(w);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "layout") == 0)
        return layout();
    if (argc >= 6 && strcmp(argv[1], "write") == 0)
        return write_values(argc - 2, argv + 2);
    fputs("usage: interop_helper layout\n"
          "       interop_helper write DIR NAME CAPACITY FLAGS PAYLOAD...\n",
          stderr);
    return 2;
}
