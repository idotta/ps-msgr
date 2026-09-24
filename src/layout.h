/* SPDX-License-Identifier: Apache-2.0 */
/*
 * On-disk layout of a state channel data file, format 1.0.
 * The offsets are pinned against the tables in spec/state-channel.md §3.
 *
 * Fields marked "atomic" are accessed only through __atomic builtins, and
 * only 32-bit ones: those are lock-free everywhere, including ARMv7.
 */
#ifndef PSMSGR_LAYOUT_H
#define PSMSGR_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define PSMI_MAGIC            0x534D5350u /* bytes "PSMS" */
#define PSMI_VERSION_MAJOR    1u
#define PSMI_VERSION_MINOR    0u
#define PSMI_HEADER_SIZE      128u
#define PSMI_SLOT_HEADER_SIZE 32u
#define PSMI_SLOT_ALIGN       64u
#define PSMI_MIN_SLOTS        2u
#define PSMI_MAX_SLOTS        16u
#define PSMI_LATEST_NONE      0xFFFFFFFFu
#define PSMI_LATEST_SLOT_MASK 0xFu        /* header.latest bits 0-3: slot index */
#define PSMI_LATEST_TAG_MASK  0x07FFFFFFu /* bits 4-30: the slot's seq / 2 */

#define PSMI_CONFIG_NO_NOTIFY (1u << 0) /* header.config_flags */
#define PSMI_STATE_RETIRED    (1u << 0) /* header.state */

typedef struct psmi_header {
    uint32_t magic;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t header_size;
    uint32_t slot_header_size;
    uint32_t slot_count;
    uint32_t slot_stride;
    uint32_t capacity;
    uint32_t payload_type;
    uint32_t config_flags;
    uint32_t state;      /* atomic */
    uint32_t latest;     /* atomic */
    uint32_t notify;     /* atomic; futex word */
    uint32_t writer_pid;
    uint32_t reserved0;
    uint64_t created_realtime_ns;
    uint8_t  reserved1[64];
} psmi_header;

/* Slot header; the payload follows at PSMI_SLOT_HEADER_SIZE. */
typedef struct psmi_slot {
    uint32_t seq;        /* atomic; odd = unreadable */
    uint32_t generation;
    uint64_t timestamp_ns;
    uint32_t length;
    uint32_t reserved[3];
} psmi_slot;

/* header.latest for slot `i` committed with (even) `seq`: the index and the
 * slot's commit count, which readers check against the slot's seq (§6.3).
 * Bit 31 stays 0, so no published value equals PSMI_LATEST_NONE. */
static inline uint32_t psmi_latest(uint32_t i, uint32_t seq)
{
    return ((seq >> 1) & PSMI_LATEST_TAG_MASK) << 4 | i;
}

static inline uint32_t psmi_latest_slot(uint32_t latest)
{
    return latest & PSMI_LATEST_SLOT_MASK;
}

/* NONE, or an encoding with a slot index below slot_count. */
static inline int psmi_latest_valid(uint32_t latest, uint32_t slot_count)
{
    return latest == PSMI_LATEST_NONE
           || ((latest >> 31) == 0 && psmi_latest_slot(latest) < slot_count);
}

_Static_assert(sizeof(psmi_header) == PSMI_HEADER_SIZE, "header size");
_Static_assert(offsetof(psmi_header, magic) == 0, "magic");
_Static_assert(offsetof(psmi_header, version_major) == 4, "version_major");
_Static_assert(offsetof(psmi_header, version_minor) == 6, "version_minor");
_Static_assert(offsetof(psmi_header, header_size) == 8, "header_size");
_Static_assert(offsetof(psmi_header, slot_header_size) == 12, "slot_header_size");
_Static_assert(offsetof(psmi_header, slot_count) == 16, "slot_count");
_Static_assert(offsetof(psmi_header, slot_stride) == 20, "slot_stride");
_Static_assert(offsetof(psmi_header, capacity) == 24, "capacity");
_Static_assert(offsetof(psmi_header, payload_type) == 28, "payload_type");
_Static_assert(offsetof(psmi_header, config_flags) == 32, "config_flags");
_Static_assert(offsetof(psmi_header, state) == 36, "state");
_Static_assert(offsetof(psmi_header, latest) == 40, "latest");
_Static_assert(offsetof(psmi_header, notify) == 44, "notify");
_Static_assert(offsetof(psmi_header, writer_pid) == 48, "writer_pid");
_Static_assert(offsetof(psmi_header, created_realtime_ns) == 56, "created_realtime_ns");
_Static_assert(offsetof(psmi_header, reserved1) == 64, "reserved1");

_Static_assert(sizeof(psmi_slot) == PSMI_SLOT_HEADER_SIZE, "slot header size");
_Static_assert(offsetof(psmi_slot, seq) == 0, "seq");
_Static_assert(offsetof(psmi_slot, generation) == 4, "generation");
_Static_assert(offsetof(psmi_slot, timestamp_ns) == 8, "timestamp_ns");
_Static_assert(offsetof(psmi_slot, length) == 16, "length");
_Static_assert(offsetof(psmi_slot, reserved) == 20, "slot reserved");

/* align_up(slot_header_size + capacity, 64). No overflow for capacity <= 16 MiB. */
static inline uint32_t psmi_slot_stride(uint32_t capacity)
{
    return (PSMI_SLOT_HEADER_SIZE + capacity + PSMI_SLOT_ALIGN - 1) & ~(PSMI_SLOT_ALIGN - 1);
}

/* header_size + slot_count * slot_stride, in 64-bit arithmetic. */
static inline uint64_t psmi_file_size(uint32_t slot_count, uint32_t slot_stride)
{
    return PSMI_HEADER_SIZE + (uint64_t)slot_count * slot_stride;
}

#endif /* PSMSGR_LAYOUT_H */
