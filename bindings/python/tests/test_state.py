# SPDX-License-Identifier: Apache-2.0
"""The C unit tests (tests/test_state.c), where relevant, through the binding."""

from __future__ import annotations

import array
import ctypes
import errno
import gc
import os
import pickle
import signal
import stat
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from functools import partial
from pathlib import Path

import pytest
from conftest import CHAN, data_path, gen_after, lock_path

from ps_msgr import (
    ChannelBusyError,
    ChannelDesc,
    ChannelFormatError,
    ChannelMismatchError,
    ErrorCode,
    PayloadTooLargeError,
    PsMsgrError,
    Snapshot,
    StateInfo,
    StateReader,
    StateWriter,
    WriterExistsError,
    _native,
    now_ns,
    unlink,
)


def test_create_then_reuse(tmp_path: Path) -> None:
    w = StateWriter(CHAN, 16, payload_type=0x1234, mode=0o640, directory=tmp_path)
    st = os.stat(data_path(tmp_path))
    assert stat.S_IMODE(st.st_mode) == 0o640
    assert st.st_size == 128 + 3 * 64
    assert stat.S_IMODE(os.stat(lock_path(tmp_path)).st_mode) == 0o640
    assert w.capacity == 16

    first = w.publish(b"one")
    assert first != 0
    assert w.publish(b"two") == gen_after(first, 1)
    w.publish(b"three")
    ino = os.stat(data_path(tmp_path)).st_ino
    w.close()

    with StateWriter(CHAN, 16, payload_type=0x1234, mode=0o640, directory=tmp_path) as w:
        assert os.stat(data_path(tmp_path)).st_ino == ino
        gen = w.publish(b"four")
        assert gen == gen_after(first, 3)
        with StateReader(CHAN, directory=tmp_path) as r:
            assert r.read() == Snapshot(b"four", gen, r.peek().timestamp_ns, True)
            assert r.describe() == ChannelDesc(16, 3, 0x1234, True)


def test_directory_from_env(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("PSMSGR_DIR", str(tmp_path))
    with StateWriter(CHAN, 0) as w, StateReader(CHAN) as r:
        assert data_path(tmp_path).exists()
        w.publish(b"")
        assert r.peek() is not None


def test_mismatch_and_recreate(tmp_path: Path) -> None:
    with StateWriter(CHAN, 16, directory=tmp_path) as w:
        old_gen = w.publish(b"old")
    r = StateReader(CHAN, directory=tmp_path)
    snap = r.read()
    assert snap is not None and snap.data == b"old" and snap.attached

    for kwargs in (
        {"capacity": 32},
        {"capacity": 16, "slot_count": 4},
        {"capacity": 16, "notify": False},
        {"capacity": 16, "payload_type": 9},
    ):
        with pytest.raises(ChannelMismatchError) as e:
            StateWriter(CHAN, directory=tmp_path, **kwargs)
        assert e.value.code == ErrorCode.MISMATCH
    assert r.peek().generation == old_gen

    ino = os.stat(data_path(tmp_path)).st_ino
    w = StateWriter(CHAN, 32, recreate=True, directory=tmp_path)
    assert os.stat(data_path(tmp_path)).st_ino != ino
    assert w.capacity == 32

    # The reader follows the retire to the new, still empty file.
    assert r.peek() is None
    assert r.describe().capacity == 32
    gen = w.publish(b"new")
    assert gen == gen_after(old_gen, 1)
    assert r.read() == Snapshot(b"new", gen, r.peek().timestamp_ns, True)

    # recreate on a compatible file is a plain reuse.
    w.close()
    ino = os.stat(data_path(tmp_path)).st_ino
    StateWriter(CHAN, 32, recreate=True, directory=tmp_path).close()
    assert os.stat(data_path(tmp_path)).st_ino == ino
    r.close()


def test_second_writer_same_process(tmp_path: Path) -> None:
    a = StateWriter(CHAN, 8, slot_count=2, directory=tmp_path)
    with pytest.raises(WriterExistsError) as e:
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path)
    assert e.value.code == ErrorCode.WRITER_EXISTS
    assert e.value.filename == CHAN
    with pytest.raises(WriterExistsError):
        StateWriter(CHAN, 8, slot_count=2, recreate=True, directory=tmp_path)
    with pytest.raises(WriterExistsError):
        unlink(CHAN, directory=tmp_path)
    a.close()
    StateWriter(CHAN, 8, slot_count=2, directory=tmp_path).close()


CHILD_WRITER = """
import sys
from ps_msgr import StateWriter
w = StateWriter(sys.argv[1], 8, slot_count=2, directory=sys.argv[2])
w.publish(b"child")
print("ready", flush=True)
sys.stdin.read()
"""


def test_second_writer_other_process(tmp_path: Path, child_env: dict[str, str]) -> None:
    r = StateReader(CHAN, directory=tmp_path)
    assert not r.writer_alive()  # no lock file yet
    child = subprocess.Popen(
        [sys.executable, "-c", CHILD_WRITER, CHAN, str(tmp_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        env=child_env,
        text=True,
    )
    try:
        assert child.stdout is not None
        assert child.stdout.readline() == "ready\n"
        with pytest.raises(WriterExistsError):
            StateWriter(CHAN, 8, slot_count=2, directory=tmp_path)
        assert r.writer_alive()
    finally:
        child.kill()  # SIGKILL: the lock goes with the process
        child.wait()
        child.stdin.close()
        child.stdout.close()
    assert not r.writer_alive()
    assert r.read().data == b"child"
    with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path):
        assert r.writer_alive()
    assert not r.writer_alive()
    r.close()


def test_lazy_reader_attaches_later(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        assert r.read() is None
        assert r.read_into(bytearray(16)) is None
        assert r.peek() is None
        assert r.describe() is None
        assert r.wait(0, 0) is False
        assert not r.writer_alive()

        with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w:
            assert r.peek() is None  # attached, nothing published
            assert r.describe().capacity == 8
            assert r.writer_alive()
            gen = w.publish(b"hi")
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.generation, snap.attached) == (b"hi", gen, True)
            assert type(snap.data) is bytes


def test_read_results_and_sizes(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        with pytest.raises(PayloadTooLargeError) as e:
            w.publish(b"123456789")
        assert e.value.code == ErrorCode.TOOBIG
        assert isinstance(e.value, ValueError)
        assert r.peek() is None  # failures publish nothing
        gen = w.publish(b"12345678")

        small = bytearray(b"\xaa" * 7)
        with pytest.raises(PayloadTooLargeError) as e:
            r.read_into(small)
        assert e.value.code == ErrorCode.TOOSMALL
        assert "8 bytes" in str(e.value)
        assert small == b"\xaa" * 7  # nothing copied
        buf = bytearray(8)
        # The attach that the TOOSMALL result carried is reported now.
        assert r.read_into(buf) == StateInfo(gen, 8, r.peek().timestamp_ns, True)
        assert buf == b"12345678"
        assert r.read_into(buf).attached is False

        # Into the middle of a larger buffer, through a memoryview.
        big = bytearray(16)
        assert r.read_into(memoryview(big)[4:12]).length == 8
        assert big == bytes(4) + b"12345678" + bytes(4)
        with pytest.raises(TypeError):
            r.read_into(b"12345678")

        w.publish(b"")
        assert r.read_into(bytearray()).length == 0
        assert r.read().data == b""


def test_publish_buffers(tmp_path: Path) -> None:
    class Sample(ctypes.Structure):
        _fields_ = [("a", ctypes.c_uint32), ("b", ctypes.c_double)]

    with (
        StateWriter(CHAN, 64, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        cases: list[tuple[object, bytes]] = [
            (b"bytes", b"bytes"),
            (bytearray(b"bytearray"), b"bytearray"),
            (memoryview(bytearray(b"writable view"))[1:], b"ritable view"),
            (memoryview(b"read-only view")[5:], b"only view"),
            (memoryview(bytearray(b"abcdef"))[::2], b"ace"),
            (array.array("I", [1, 2]), array.array("I", [1, 2]).tobytes()),
            (Sample(7, 2.5), bytes(Sample(7, 2.5))),
            (bytearray(), b""),
            (memoryview(b""), b""),
        ]
        for data, expected in cases:
            gen = w.publish(data)
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.generation) == (expected, gen)
        with pytest.raises(TypeError):
            w.publish("str")


def test_heartbeat_channel(tmp_path: Path) -> None:
    with StateWriter(CHAN, 0, slot_count=2, directory=tmp_path) as w:
        assert w.capacity == 0
        assert os.stat(data_path(tmp_path)).st_size == 128 + 2 * 64
        with pytest.raises(PayloadTooLargeError):
            w.publish(b"x")
        first = w.publish(b"")
        gen = w.publish(b"")
        assert gen == gen_after(first, 1)
        with StateReader(CHAN, directory=tmp_path) as r:
            snap = r.read()
            assert snap is not None and (snap.data, snap.generation) == (b"", gen)
            assert r.describe().capacity == 0


def test_receive_buffer_follows_capacity(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        with StateWriter(CHAN, 8, directory=tmp_path) as w:
            w.publish(b"12345678")
            assert r.read().data == b"12345678"
        with StateWriter(CHAN, 64, recreate=True, directory=tmp_path) as w:
            w.publish(bytes(range(64)))
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.attached) == (bytes(range(64)), True)
            assert len(r._buf) == 64
        with StateWriter(CHAN, 4, recreate=True, directory=tmp_path) as w:
            w.publish(b"abcd")
            assert r.peek().attached
            snap = r.read()
            assert snap is not None
            assert (snap.data, snap.attached) == (b"abcd", False)  # peek reported it
            assert len(r._buf) == 4


def test_peek_timestamps(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        prev = 0
        first = 0
        for i in range(1, 21):
            before = now_ns()
            gen = w.publish(i.to_bytes(4, "little"))
            after = now_ns()
            if i == 1:
                first = gen
            assert gen == gen_after(first, i - 1)
            info = r.peek()
            assert info is not None
            assert (info.generation, info.length) == (gen, 4)
            assert before <= info.timestamp_ns <= after
            assert info.timestamp_ns >= prev
            assert info.attached == (i == 1)
            assert 0 <= info.age_ns < 10**10
            prev = info.timestamp_ns


def test_wait_wakes_on_publish(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        gen = w.publish(b"a")
        # A value that differs from last_generation returns at once.
        assert r.wait(0, 0) is True
        assert r.wait(gen_after(gen, 1), None) is True

        with ThreadPoolExecutor(1) as pool:
            waiting = pool.submit(r.wait, gen, 10)
            time.sleep(0.05)  # likely blocked by now; the test holds either way
            w.publish(b"b")
            assert waiting.result() is True
        assert r.peek().generation == gen_after(gen, 1)


def test_wait_timeouts(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        assert r.wait(0, 0) is False  # nothing published counts as unchanged
        gen = w.publish(b"a")
        assert r.wait(gen, 0) is False

        t0 = time.monotonic_ns()
        assert r.wait(gen, 0.12) is False
        assert time.monotonic_ns() - t0 >= 120_000_000

        with StateReader("absent", directory=tmp_path) as none:
            t0 = time.monotonic_ns()
            assert none.wait(0, 0.05) is False
            assert time.monotonic_ns() - t0 >= 50_000_000

        for bad in (-1, -0.001, float("nan")):
            with pytest.raises(ValueError):
                r.wait(gen, bad)
        with pytest.raises(ValueError):
            r.wait(-1, 0)
        with pytest.raises(ValueError):
            r.wait(1 << 32, 0)


def test_wait_huge_timeout(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        w.publish(b"a")
        assert r.wait(0, 1e300) is True
        assert r.wait(0, sys.float_info.max) is True


def test_wait_follows_retire(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w:
            gen = w.publish(b"a")
        assert r.wait(0, 0) is True  # attach
        with ThreadPoolExecutor(1) as pool:
            waiting = pool.submit(r.wait, gen, 10)
            time.sleep(0.05)
            with StateWriter(CHAN, 16, slot_count=2, recreate=True, directory=tmp_path) as w:
                time.sleep(0.05)
                assert not waiting.done()  # the retire alone does not return
                w.publish(b"b")
                assert waiting.result() is True
        assert r.describe().capacity == 16


def test_wait_follows_unlink(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w:
            gen = w.publish(b"a")
        assert r.wait(0, 0) is True
        with ThreadPoolExecutor(1) as pool:
            waiting = pool.submit(r.wait, gen, 10)
            time.sleep(0.05)
            assert unlink(CHAN, directory=tmp_path) is True
            assert not data_path(tmp_path).exists() and not lock_path(tmp_path).exists()
            time.sleep(0.05)
            assert not waiting.done()
            with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w:
                new_gen = w.publish(b"c")  # must not reuse gen
                assert waiting.result() is True
        assert r.peek().generation == new_gen
        assert unlink(CHAN, directory=tmp_path) is True
        assert r.peek() is None
        assert unlink(CHAN, directory=tmp_path) is False


def test_wait_not_supported_without_notify(tmp_path: Path) -> None:
    with (
        StateWriter(CHAN, 8, slot_count=2, notify=False, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        w.publish(b"a")
        with pytest.raises(PsMsgrError) as e:
            r.wait(0, 0.1)
        assert e.value.code == ErrorCode.NOTSUP
        assert type(e.value) is PsMsgrError
        assert r.describe() == ChannelDesc(8, 2, 0, False)


def test_close_stops_wait(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    closed: list[object] = []
    close = StateReader._close_fn

    def recording_close(h: object) -> None:
        closed.append(h)
        close(h)

    monkeypatch.setattr(StateReader, "_close_fn", staticmethod(recording_close))
    with StateWriter(CHAN, 8, directory=tmp_path) as w:
        gen = w.publish(b"a")
        for timeout in (None, 30):
            closed.clear()
            r = StateReader(CHAN, directory=tmp_path)
            with ThreadPoolExecutor(1) as pool:
                waiting = pool.submit(r.wait, gen, timeout)
                time.sleep(0.05)  # likely blocked by now; the test holds either way
                t0 = time.monotonic()
                r.close()
                assert r.closed
                with pytest.raises(ValueError, match="closed"):
                    waiting.result(timeout=10)
                assert time.monotonic() - t0 < 2
            assert len(closed) == 1  # by the waiter, once it no longer used the handle
            r.close()
            assert len(closed) == 1
            with pytest.raises(ValueError, match="closed"):
                r.writer_alive()


def _alarm(handler: object, seconds: float, interval: float = 0.0) -> object:
    old = signal.signal(signal.SIGALRM, handler)
    signal.setitimer(signal.ITIMER_REAL, seconds, interval)
    return old


def _no_alarm(old: object) -> None:
    signal.setitimer(signal.ITIMER_REAL, 0)
    signal.signal(signal.SIGALRM, old)


def test_wait_raises_keyboard_interrupt(tmp_path: Path) -> None:
    def interrupt(signum: int, frame: object) -> None:
        raise KeyboardInterrupt

    with (
        StateWriter(CHAN, 8, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        gen = w.publish(b"a")
        old = _alarm(interrupt, 0.1)
        t0 = time.monotonic()
        try:
            with pytest.raises(KeyboardInterrupt):
                r.wait(gen, None)
        finally:
            _no_alarm(old)
        assert time.monotonic() - t0 < 2


def test_wait_sigint_to_main_thread(tmp_path: Path) -> None:
    # Python's own SIGINT handler: what Ctrl-C does.
    old = signal.signal(signal.SIGINT, signal.default_int_handler)
    main = threading.main_thread().ident
    assert main is not None
    timer = threading.Timer(0.1, signal.pthread_kill, (main, signal.SIGINT))
    try:
        with (
            StateWriter(CHAN, 8, directory=tmp_path) as w,
            StateReader(CHAN, directory=tmp_path) as r,
        ):
            gen = w.publish(b"a")
            timer.start()
            t0 = time.monotonic()
            with pytest.raises(KeyboardInterrupt):
                r.wait(gen, 30)
            assert time.monotonic() - t0 < 2
    finally:
        timer.cancel()
        timer.join()
        signal.signal(signal.SIGINT, old)


def test_wait_resumes_after_signal_handler(tmp_path: Path) -> None:
    calls: list[int] = []
    with (
        StateWriter(CHAN, 8, directory=tmp_path) as w,
        StateReader(CHAN, directory=tmp_path) as r,
    ):
        gen = w.publish(b"a")
        old = _alarm(lambda signum, frame: calls.append(signum), 0.02, 0.02)
        t0 = time.monotonic_ns()
        try:
            assert r.wait(gen, 0.3) is False
        finally:
            _no_alarm(old)
        assert time.monotonic_ns() - t0 >= 300_000_000
        assert len(calls) >= 2


def test_writer_alive(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        assert not r.writer_alive()
        with StateWriter(CHAN, 8, directory=tmp_path):
            assert r.writer_alive()
        assert not r.writer_alive()


def test_attached_flag_once_per_attach(tmp_path: Path) -> None:
    with StateReader(CHAN, directory=tmp_path) as r:
        with StateWriter(CHAN, 8, slot_count=2, directory=tmp_path) as w:
            w.publish(b"")
            assert r.describe() is not None  # attaches, does not consume
            assert r.wait(0, 0) is True  # neither does wait
            assert r.peek().attached is True
            assert r.peek().attached is False
            assert r.read().attached is False
        with StateWriter(CHAN, 8, slot_count=3, recreate=True, directory=tmp_path) as w:
            assert r.peek() is None
            w.publish(b"")
            assert r.read().attached is True
            assert r.peek().attached is False


def test_unlink(tmp_path: Path) -> None:
    assert unlink(CHAN, directory=tmp_path) is False
    with StateWriter(CHAN, 8, directory=tmp_path), pytest.raises(WriterExistsError):
        unlink(CHAN, directory=tmp_path)
    assert unlink(CHAN, directory=tmp_path) is True
    assert not data_path(tmp_path).exists() and not lock_path(tmp_path).exists()
    assert unlink(CHAN, directory=tmp_path) is False


def test_invalid_arguments(tmp_path: Path) -> None:
    for name in ("", ".x", "a/b", "a b", "\xe4", "n" * 65):
        for call in (
            partial(StateReader, name, directory=tmp_path),
            partial(StateWriter, name, 8, directory=tmp_path),
            partial(unlink, name, directory=tmp_path),
        ):
            with pytest.raises(PsMsgrError) as e:
                call()
            assert type(e.value) is PsMsgrError
            assert e.value.code == ErrorCode.INVAL
            assert e.value.errno is None
    StateReader("n" * 64, directory=tmp_path).close()

    with pytest.raises(ValueError, match="null"):
        StateReader("a\0b", directory=tmp_path)
    with pytest.raises(ValueError, match="null"):
        StateWriter(CHAN, 8, directory=f"{tmp_path}\0x")
    with pytest.raises(TypeError):
        StateReader(b"chan", directory=tmp_path)

    for kwargs in ({"slot_count": 1}, {"slot_count": 17}, {"mode": 0o10644}):
        with pytest.raises(PsMsgrError) as e:
            StateWriter(CHAN, 8, directory=tmp_path, **kwargs)
        assert e.value.code == ErrorCode.INVAL
    with pytest.raises(PsMsgrError):
        StateWriter(CHAN, _native.STATE_MAX_CAPACITY + 1, directory=tmp_path)
    # Out of uint32 range: rejected before ctypes would truncate it.
    for kwargs in ({"capacity": 1 << 32}, {"capacity": -1}, {"payload_type": 1 << 32}):
        with pytest.raises(ValueError, match="out of range"):
            StateWriter(CHAN, **{"capacity": 8, **kwargs}, directory=tmp_path)
    with pytest.raises(TypeError):
        StateWriter(CHAN, 8.0, directory=tmp_path)
    assert not lock_path(tmp_path).exists()  # nothing created


def test_system_errors(tmp_path: Path) -> None:
    with pytest.raises(PsMsgrError) as e:
        StateWriter(CHAN, 8, directory=tmp_path / "missing")
    assert e.value.code == ErrorCode.SYS
    assert e.value.errno == errno.ENOENT
    assert isinstance(e.value, OSError)
    assert str(e.value) == f"[Errno {errno.ENOENT}] {os.strerror(errno.ENOENT)}: 'chan'"

    os.symlink(tmp_path / "elsewhere", data_path(tmp_path))
    with pytest.raises(PsMsgrError) as e:
        StateWriter(CHAN, 8, directory=tmp_path)
    assert e.value.errno == errno.ELOOP
    with StateReader(CHAN, directory=tmp_path) as r, pytest.raises(PsMsgrError) as e:
        r.peek()
    assert (e.value.code, e.value.errno) == (ErrorCode.SYS, errno.ELOOP)


def test_format_error(tmp_path: Path) -> None:
    data_path(tmp_path).write_bytes(b"not a channel" * 20)
    with pytest.raises(ChannelFormatError) as e:
        StateWriter(CHAN, 8, directory=tmp_path)
    assert e.value.code == ErrorCode.FORMAT
    with StateReader(CHAN, directory=tmp_path) as r, pytest.raises(ChannelFormatError):
        r.read()
    StateWriter(CHAN, 8, recreate=True, directory=tmp_path).close()


def test_errors() -> None:
    assert issubclass(PsMsgrError, OSError)
    for cls in (WriterExistsError, ChannelMismatchError, ChannelFormatError, ChannelBusyError):
        assert issubclass(cls, PsMsgrError)
    assert issubclass(PayloadTooLargeError, PsMsgrError)
    assert issubclass(PayloadTooLargeError, ValueError)
    assert set(ErrorCode) == {
        getattr(_native, name) for name in dir(_native) if name.startswith("E_")
    }

    e = ChannelBusyError(ErrorCode.BUSY, filename=CHAN)
    assert str(e) == "channel busy: 'chan'"
    assert e.errno is None
    for err in (e, PsMsgrError(ErrorCode.SYS, errno=errno.EACCES), PsMsgrError(-99)):
        copy = pickle.loads(pickle.dumps(err))
        assert type(copy) is type(err)
        assert (copy.code, copy.errno, copy.strerror, copy.filename, str(copy)) == (
            err.code,
            err.errno,
            err.strerror,
            err.filename,
            str(err),
        )
    assert PsMsgrError(-99).strerror == "unknown error"
    assert repr(e) == "ChannelBusyError(<ErrorCode.BUSY: -6>, 'channel busy', None, 'chan')"


def test_use_after_close(tmp_path: Path) -> None:
    w = StateWriter(CHAN, 8, directory=tmp_path)
    r = StateReader(CHAN, directory=tmp_path)
    assert not w.closed and not r.closed
    w.close()
    w.close()
    r.close()
    r.close()
    assert w.closed and r.closed
    assert "closed" in repr(w) and "closed" in repr(r)
    for call in (
        lambda: w.publish(b""),
        lambda: w.capacity,
        lambda: w.__enter__(),
        r.read,
        lambda: r.read_into(bytearray(8)),
        r.peek,
        lambda: r.wait(0, 0),
        r.writer_alive,
        r.describe,
        lambda: r.__enter__(),
    ):
        with pytest.raises(ValueError, match="closed"):
            call()


def test_unclosed_handles_warn(tmp_path: Path) -> None:
    w = StateWriter(CHAN, 8, directory=tmp_path)
    r = StateReader(CHAN, directory=tmp_path)
    with pytest.warns(ResourceWarning, match="unclosed"):
        del w, r
        gc.collect()
    StateWriter(CHAN, 8, directory=tmp_path).close()  # the lock was released
