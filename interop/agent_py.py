# SPDX-License-Identifier: Apache-2.0
"""The Python agent of the interop suite (README.md), run with the installed ps_msgr."""

from __future__ import annotations

import ctypes
import json
import sys
import time
from typing import Any

from ps_msgr import ErrorCode, PsMsgrError, StateReader, StateWriter


class MotorStatus(ctypes.Structure):
    _fields_ = [
        ("sequence", ctypes.c_uint64),
        ("speed_rpm", ctypes.c_float),
        ("current_a", ctypes.c_float),
        ("temperature_c", ctypes.c_float),
    ]


FIELDS = MotorStatus._fields_
MOTOR_STATUS_V1 = 0x0001_0001

USAGE = """\
usage: agent_py.py payload-layout
       agent_py.py write DIR NAME [--capacity N] [--payload-type T] [--recreate]
                         [--no-notify] [--rate HZ] [--hold] SEQUENCE...
       agent_py.py read DIR NAME [--for MS]
       agent_py.py wait DIR NAME LAST_GENERATION TIMEOUT_MS
       agent_py.py alive DIR NAME
       agent_py.py    (the commands above but write, one per line of stdin)
"""


class UsageError(Exception):
    pass


def motor_status(n: int) -> MotorStatus:
    return MotorStatus(n, (n % 65536) / 2, (n % 1024) / 64, (n % 2048) / 8 - 40)


def emit(result: dict[str, Any]) -> None:
    print(json.dumps(result, separators=(",", ":")), flush=True)


def error(e: PsMsgrError) -> dict[str, Any]:
    return {"error": int(e.code), "exception": type(e).__name__}


def payload_layout() -> int:
    emit(
        {
            "sizeof": ctypes.sizeof(MotorStatus),
            "offsets": {name: getattr(MotorStatus, name).offset for name, _ in FIELDS},
            "payload_type": MOTOR_STATUS_V1,
        }
    )
    return 0


def write(args: list[str]) -> int:
    directory, name, *rest = args
    capacity, payload_type = ctypes.sizeof(MotorStatus), MOTOR_STATUS_V1
    recreate, notify, rate, hold = False, True, 0.0, False
    while rest and rest[0].startswith("--"):
        option = rest.pop(0)
        if option == "--recreate":
            recreate = True
        elif option == "--no-notify":
            notify = False
        elif option == "--hold":
            hold = True
        elif option in ("--capacity", "--payload-type", "--rate") and rest:
            value = rest.pop(0)
            if option == "--capacity":
                capacity = int(value, 0)
            elif option == "--payload-type":
                payload_type = int(value, 0)
            else:
                rate = float(value)
        else:
            raise UsageError
    try:
        w = StateWriter(
            name,
            capacity,
            payload_type=payload_type,
            recreate=recreate,
            notify=notify,
            directory=directory,
        )
    except PsMsgrError as e:
        emit(error(e))
        return 1
    with w:
        emit({"opened": True})
        start = time.monotonic()
        for k, sequence in enumerate(int(v, 0) for v in rest):
            if rate > 0:
                time.sleep(max(0.0, start + k / rate - time.monotonic()))
            try:
                generation = w.publish(motor_status(sequence))
            except PsMsgrError as e:
                emit(error(e))
                return 1
            emit({"generation": generation, "sequence": sequence})
        if hold:
            sys.stdin.read()
    return 0


def read_result(r: StateReader) -> dict[str, Any]:
    try:
        snap = r.read()
        if snap is None:
            return {"nodata": True}
        desc = r.describe()
    except PsMsgrError as e:
        return error(e)
    assert desc is not None
    value = None
    if len(snap.data) == ctypes.sizeof(MotorStatus):
        s = MotorStatus.from_buffer_copy(snap.data)
        value = {name: getattr(s, name) for name, _ in FIELDS}
    return {
        "generation": snap.generation,
        "length": len(snap.data),
        "timestamp_ns": snap.timestamp_ns,
        "attached": snap.attached,
        "payload_type": desc.payload_type,
        "value": value,
        "hex": snap.data.hex(),
    }


def read(r: StateReader, args: list[str]) -> int:
    if not args:
        result = read_result(r)
        emit(result)
        return 1 if "error" in result else 0
    if len(args) != 2 or args[0] != "--for":
        raise UsageError
    end = time.monotonic() + int(args[1]) / 1000
    reads = busy = 0
    last = None
    while True:
        reads += 1
        result = read_result(r)
        if result.get("error") == ErrorCode.BUSY:
            busy += 1
        elif result != last:
            emit(result)
            last = result
        if time.monotonic() >= end:
            break
    emit({"reads": reads, "busy": busy})
    return 0


def wait(r: StateReader, args: list[str]) -> int:
    if len(args) != 2:
        raise UsageError
    last_generation, timeout_ms = int(args[0], 0), int(args[1], 0)
    try:
        changed = r.wait(last_generation, None if timeout_ms < 0 else timeout_ms / 1000)
    except PsMsgrError as e:
        emit(error(e))
        return 1
    emit({"changed": changed})
    return 0


def alive(r: StateReader, args: list[str]) -> int:
    if args:
        raise UsageError
    try:
        emit({"alive": r.writer_alive()})
    except PsMsgrError as e:
        emit(error(e))
        return 1
    return 0


READER_COMMANDS = {"read": read, "wait": wait, "alive": alive}


def command(args: list[str], readers: dict[tuple[str, str], StateReader] | None) -> int:
    cmd, *rest = args
    if cmd == "payload-layout" and not rest:
        return payload_layout()
    if cmd == "write" and readers is None and len(rest) >= 2:
        return write(rest)
    if cmd not in READER_COMMANDS or len(rest) < 2:
        raise UsageError
    directory, name, *rest = rest
    if readers is None:
        with StateReader(name, directory=directory) as r:
            return READER_COMMANDS[cmd](r, rest)
    r = readers.get((directory, name))
    if r is None:
        r = readers[directory, name] = StateReader(name, directory=directory)
    return READER_COMMANDS[cmd](r, rest)


def session() -> int:
    readers: dict[tuple[str, str], StateReader] = {}
    try:
        for line in sys.stdin:
            if args := line.split():
                command(args, readers)
    finally:
        for r in readers.values():
            r.close()
    return 0


def main(args: list[str]) -> int:
    try:
        return command(args, None) if args else session()
    except (UsageError, ValueError):
        sys.stderr.write(USAGE)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
