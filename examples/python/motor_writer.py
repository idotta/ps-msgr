#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Publishes a changing motor status on the "motor" channel.

motor_writer.py [--dir DIR] [--rate HZ] [--count N]
"""

import argparse
import ctypes
import itertools
import sys
import time

from motor_status import MOTOR_CHANNEL, MOTOR_STATUS_V1, MotorStatus
from ps_msgr import PsMsgrError, StateWriter


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dir", help="channel directory (default: $PSMSGR_DIR, else /dev/shm)")
    parser.add_argument("--rate", type=float, default=10.0, metavar="HZ", help="values per second")
    parser.add_argument("--count", type=int, default=0, metavar="N", help="stop after N values")
    args = parser.parse_args()
    if not args.rate > 0:
        parser.error("--rate must be positive")

    # This program owns the channel: recreate=True replaces one left behind with
    # another capacity or payload type (e.g. by an older version) instead of
    # raising ChannelMismatchError. Attached readers move to the new file.
    with StateWriter(
        MOTOR_CHANNEL,
        ctypes.sizeof(MotorStatus),
        payload_type=MOTOR_STATUS_V1,
        recreate=True,
        directory=args.dir,
    ) as writer:
        status = MotorStatus()
        period_ns = int(1e9 / args.rate)
        next_ns = time.monotonic_ns()
        for seq in itertools.count(1):
            status.sequence = seq
            status.speed_rpm = 1500 + (seq % 100) * 5
            status.current_a = 2 + (seq % 8) * 0.25
            status.temperature_c = 40 + (seq % 40) * 0.5
            # publish() takes the struct's buffer as is: no bytes object in between.
            writer.publish(memoryview(status))
            print(status, flush=True)
            if seq == args.count:
                break
            next_ns += period_ns
            time.sleep(max(0, next_ns - time.monotonic_ns()) / 1e9)
    # Leaving the with block closes the writer: the channel and its last value
    # stay, and readers see writer_alive() turn False.


if __name__ == "__main__":
    try:
        main()
    except PsMsgrError as e:
        sys.exit(f"motor_writer: {e}")
    except KeyboardInterrupt:
        pass
