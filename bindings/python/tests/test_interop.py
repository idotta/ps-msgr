# SPDX-License-Identifier: Apache-2.0
"""C and Python on either side of a channel, in separate processes."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest
from conftest import CHAN, gen_after

from ps_msgr import StateReader, StateWriter, WriterExistsError, _native


class CWriter:
    """tests/interop_helper holding a writer until stopped."""

    def __init__(
        self, helper: Path, directory: Path, capacity: int, flags: int, *payloads: str
    ) -> None:
        self.proc = subprocess.Popen(
            [helper, "write", directory, CHAN, str(capacity), str(flags), *payloads],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        assert self.proc.stdout is not None
        self.generations = [int(self.proc.stdout.readline()) for _ in payloads]

    def stop(self) -> None:
        self.proc.communicate()
        assert self.proc.returncode == 0


def test_c_writer_python_reader(helper: Path, tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        c = CWriter(helper, tmp_path, 16, 0, "one", "two")
        try:
            assert r.wait(0, 10) is True
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.generation, snap.attached) == (b"two", c.generations[1], True)
            assert c.generations[1] == gen_after(c.generations[0], 1)
            assert r.writer_alive()
            desc = r.describe()
            assert desc is not None and (desc.capacity, desc.notify) == (16, True)
            with pytest.raises(WriterExistsError):
                StateWriter(CHAN, 16, directory=tmp_path)
        finally:
            c.stop()
        assert not r.writer_alive()
        assert r.read().data == b"two"


def test_c_writer_recreates_under_python_reader(helper: Path, tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        with StateWriter(CHAN, 8, directory=tmp_path) as w:
            gen = w.publish(b"python")
        assert r.read().data == b"python"
        c = CWriter(helper, tmp_path, 32, _native.STATE_RECREATE, "c" * 20)
        try:
            assert c.generations == [gen_after(gen, 1)]
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.attached) == (b"c" * 20, True)
            assert snap.generation == gen_after(gen, 1)
            assert r.describe().capacity == 32
        finally:
            c.stop()


def test_python_writer_c_reader(dump_tool: Path, tmp_path: Path) -> None:
    def dump() -> str:
        p = subprocess.run(
            [dump_tool, CHAN, "--dir", tmp_path, "--hex"], capture_output=True, text=True
        )
        assert p.returncode == 0, p.stderr
        return p.stdout

    with StateWriter(CHAN, 16, payload_type=0x42, directory=tmp_path) as w:
        gen = w.publish(b"hello, dump")
        out = dump()
        assert "capacity      16\n" in out
        assert "payload_type  0x00000042 (66)\n" in out
        assert "writer        alive\n" in out
        assert f"payload       generation {gen}, 11 bytes\n" in out
        assert " 68 65 6c 6c 6f 2c 20 64  75 6d 70 " in out
    assert "writer        not running\n" in dump()
