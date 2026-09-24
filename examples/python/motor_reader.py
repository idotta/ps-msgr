#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prints the motor status published on the "motor" channel, one line per value,
and says so when the writer stalls or goes away.

    motor_reader.py [--dir DIR] [--count N] [--timeout MS]
"""

import argparse
import ctypes
import sys

from motor_status import MOTOR_CHANNEL, MOTOR_STATUS_V1, MotorStatus
from ps_msgr import ChannelBusyError, PsMsgrError, StateInfo, StateReader


def check_type(reader: StateReader, info: StateInfo) -> None:
    """info.attached marks the first result from a newly attached channel file:
    at the first value, and again whenever the writer recreated the channel. The
    bytes are only a MotorStatus if the payload type says so."""
    desc = reader.describe() if info.attached else None
    if desc is not None and desc.payload_type != MOTOR_STATUS_V1:
        sys.exit(
            f"motor_reader: {MOTOR_CHANNEL}: payload type 0x{desc.payload_type:08x}, "
            f"expected 0x{MOTOR_STATUS_V1:08x} (MOTOR_STATUS_V1)"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", help="channel directory (default: $PSMSGR_DIR, else /dev/shm)")
    parser.add_argument("--count", type=int, default=0, metavar="N", help="stop after N values")
    parser.add_argument(
        "--timeout", type=int, default=300, metavar="MS", help="report after MS without a value"
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    # Succeeds whether or not the writer has started: the reader attaches to the
    # channel file once it exists.
    with StateReader(MOTOR_CHANNEL, directory=args.dir) as reader:
        status = MotorStatus()
        buf = memoryview(status)  # the struct's own memory, where read_into() writes
        seen = 0  # generation of the last value printed; 0: none
        n = 0
        while args.count == 0 or n < args.count:
            if not reader.wait(seen, timeout=args.timeout / 1000):
                # No new value in time. peek() gives the age of the last one without
                # copying it; writer_alive() tells a stalled writer from a dead one.
                info = reader.peek()
                if info is None:
                    print("waiting for the writer", flush=True)
                    continue
                check_type(reader, info)
                state = "stale" if reader.writer_alive() else "writer gone"
                print(f"{state}: last value {info.age_ns / 1e9:.1f} s old", flush=True)
                continue

            # read_into() copies the value straight into the struct: no torn values,
            # no bytes object per value. A larger payload raises PayloadTooLargeError.
            try:
                info = reader.read_into(buf)
            except ChannelBusyError:  # transient: the writer kept overwriting the slot
                continue
            if info is None:
                continue
            check_type(reader, info)
            if info.length != ctypes.sizeof(status):
                sys.exit(f"motor_reader: {MOTOR_CHANNEL}: payload of {info.length} bytes")
            seen = info.generation
            print(status, flush=True)
            n += 1


if __name__ == "__main__":
    try:
        main()
    except PsMsgrError as e:
        sys.exit(f"motor_reader: {e}")
    except KeyboardInterrupt:
        pass
