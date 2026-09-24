# SPDX-License-Identifier: Apache-2.0
"""The payload of the "motor" channel: mirrors examples/motor_status.h, the definition."""

import ctypes

MOTOR_CHANNEL = "motor"
MOTOR_STATUS_V1 = 0x0001_0001  # payload_type: schema 1, version 1


class MotorStatus(ctypes.Structure):
    """Same fields in the same order as the C struct: ctypes then applies the
    same natural alignment, so the size is 24, padded after temperature_c."""

    _fields_ = [
        ("sequence", ctypes.c_uint64),
        ("speed_rpm", ctypes.c_float),
        ("current_a", ctypes.c_float),
        ("temperature_c", ctypes.c_float),
    ]

    def __str__(self) -> str:
        return (
            f"seq={self.sequence} speed={self.speed_rpm:.1f} rpm "
            f"current={self.current_a:.2f} A temperature={self.temperature_c:.1f} C"
        )


assert ctypes.sizeof(MotorStatus) == 24
