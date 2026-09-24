/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The payload of the "motor" channel that every example writes and reads.
 * This header is the definition; python/motor_status.py and
 * csharp/MotorStatus.cs mirror it field for field.
 */
#ifndef MOTOR_STATUS_H
#define MOTOR_STATUS_H

#include <stdint.h>

#define MOTOR_CHANNEL "motor"

/* payload_type of the channel: schema 1 in the upper 16 bits, version 1 in
 * the lower. Readers check it before trusting the bytes; a new layout gets a
 * new version. */
#define MOTOR_STATUS_V1 0x00010001u

/* Plain fixed-width fields with natural alignment, so that ctypes and
 * [StructLayout(Sequential)] lay it out the same way: 20 bytes of fields,
 * padded to 24 by the 8-byte alignment of sequence. */
struct motor_status {
    uint64_t sequence;
    float speed_rpm;
    float current_a;
    float temperature_c;
};

_Static_assert(sizeof(struct motor_status) == 24, "motor_status layout changed");

#endif /* MOTOR_STATUS_H */
