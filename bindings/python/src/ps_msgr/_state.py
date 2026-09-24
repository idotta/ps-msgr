# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import math
import operator
import os
import time
import warnings
from ctypes import Array, byref, c_char, c_uint32, sizeof
from dataclasses import dataclass
from types import TracebackType
from typing import Any, Final, Self, cast

from . import _native
from ._errors import PayloadTooLargeError, error

StrPath = str | os.PathLike[str]

_U32_MAX: Final = 0xFFFF_FFFF
_I32_MAX: Final = 0x7FFF_FFFF
# A TOOSMALL after resizing to the described capacity means the channel was
# replaced in between; more than a few in a row is not going to happen.
_READ_ATTEMPTS: Final = 4


def now_ns() -> int:
    """CLOCK_MONOTONIC in nanoseconds: the clock of ``timestamp_ns``."""
    return int(_native.now_ns())


@dataclass(frozen=True, slots=True)
class StateInfo:
    generation: int
    length: int
    timestamp_ns: int
    attached: bool
    """First result since the reader (re)attached to a channel file: re-check
    ``describe().payload_type`` before trusting the bytes."""

    @property
    def age_ns(self) -> int:
        return now_ns() - self.timestamp_ns


@dataclass(frozen=True, slots=True)
class Snapshot:
    data: bytes
    generation: int
    timestamp_ns: int
    attached: bool
    """As ``StateInfo.attached``."""


@dataclass(frozen=True, slots=True)
class ChannelDesc:
    capacity: int
    slot_count: int
    payload_type: int
    notify: bool


def _u32(what: str, value: int) -> int:
    value = operator.index(value)
    if not 0 <= value <= _U32_MAX:
        raise ValueError(f"{what} out of range: {value}")
    return value


def _encode_name(name: str) -> bytes:
    if not isinstance(name, str):
        raise TypeError(f"name must be str, not {type(name).__name__}")
    b = name.encode("utf-8", "surrogateescape")
    if b"\0" in b:
        raise ValueError("embedded null character in name")
    return b


def _encode_dir(directory: StrPath | None) -> bytes | None:
    if directory is None:
        return None
    b = os.fsencode(directory)
    if b"\0" in b:
        raise ValueError("embedded null byte in directory")
    return b


def _closed(obj: object) -> ValueError:
    return ValueError(f"operation on closed {type(obj).__name__}")


def unlink(name: str, *, directory: StrPath | None = None) -> bool:
    """Retires and deletes a channel. False if it does not exist; raises
    ``WriterExistsError`` while a writer holds it."""
    rc = _native.state_unlink(_encode_name(name), _encode_dir(directory))
    if rc == _native.OK:
        return True
    if rc == _native.E_NODATA:
        return False
    raise error(rc, name)


class StateWriter:
    """The single writer of a channel: opens or creates it and takes the
    writer lock. Like the C handle, not thread-safe."""

    __slots__ = ("_capacity", "_gen", "_gen_ref", "_h", "_name")

    _close_fn = _native.state_writer_close  # still reachable from __del__ at exit

    def __init__(
        self,
        name: str,
        capacity: int,
        *,
        slot_count: int = _native.STATE_DEFAULT_SLOTS,
        payload_type: int = 0,
        mode: int = 0o644,
        recreate: bool = False,
        notify: bool = True,
        directory: StrPath | None = None,
    ) -> None:
        self._h: Any = None
        self._name = name
        opt = _native.StateOptions()
        _native.state_options_init_sized(byref(opt), sizeof(opt))
        opt.capacity = _u32("capacity", capacity)
        opt.slot_count = _u32("slot_count", slot_count)
        opt.payload_type = _u32("payload_type", payload_type)
        opt.mode = _u32("mode", mode)
        opt.flags = (_native.STATE_RECREATE if recreate else 0) | (
            0 if notify else _native.STATE_NO_NOTIFY
        )
        opt.dir = _encode_dir(directory)
        h = _native.WriterPtr()
        rc = _native.state_writer_open(_encode_name(name), byref(opt), byref(h))
        if rc != _native.OK:
            raise error(rc, name)
        self._h = h
        self._capacity = int(_native.state_writer_capacity(h))
        self._gen = c_uint32()
        self._gen_ref = byref(self._gen)

    def publish(self, data: bytes | bytearray | memoryview) -> int:
        """Copies and publishes a value; returns its generation. Any
        contiguous buffer works; a read-only one other than bytes is copied
        once more first."""
        h = self._h
        if h is None:
            raise _closed(self)
        buf: Any
        if type(data) is bytes:
            buf, n = data, len(data)
        elif type(data) is bytearray:
            n = len(data)
            buf = byref(c_char.from_buffer(data)) if n else None
        else:
            mv = memoryview(data)
            n = mv.nbytes
            if n == 0:
                buf = None
            elif mv.readonly or not mv.c_contiguous:
                buf = mv.tobytes()
            else:
                buf = byref(c_char.from_buffer(mv))
        if n > _U32_MAX:
            raise PayloadTooLargeError(_native.E_TOOBIG, filename=self._name)
        rc = _native.state_publish(h, buf, n, self._gen_ref)
        if rc != _native.OK:
            raise error(rc, self._name)
        return self._gen.value

    @property
    def capacity(self) -> int:
        if self._h is None:
            raise _closed(self)
        return self._capacity

    @property
    def closed(self) -> bool:
        return self._h is None

    def close(self) -> None:
        """Releases the writer lock; the channel and its last value remain."""
        h, self._h = self._h, None
        if h is not None:
            self._close_fn(h)

    def __enter__(self) -> Self:
        if self._h is None:
            raise _closed(self)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_h", None) is not None:
            try:
                warnings.warn(f"unclosed {self!r}", ResourceWarning, source=self, stacklevel=1)
            finally:
                self.close()

    def __repr__(self) -> str:
        state = "closed" if self._h is None else f"capacity={self._capacity}"
        return f"<StateWriter {self._name!r} {state}>"


class StateReader:
    """A reader of a channel. Opening always succeeds for a valid name: the
    reader attaches when the channel appears, and follows it when it is
    recreated. Like the C handle, not thread-safe."""

    __slots__ = (
        "_buf",
        "_desc",
        "_desc_ref",
        "_h",
        "_info",
        "_info_ref",
        "_name",
        "_pending",
        "_recheck",
    )

    _close_fn = _native.state_reader_close

    def __init__(self, name: str, *, directory: StrPath | None = None) -> None:
        self._h: Any = None
        self._name = name
        h = _native.ReaderPtr()
        rc = _native.state_reader_open(_encode_name(name), _encode_dir(directory), byref(h))
        if rc != _native.OK:
            raise error(rc, name)
        self._h = h
        self._info = _native.StateInfo()
        self._info_ref = byref(self._info)
        self._desc = _native.StateDesc()
        self._desc_ref = byref(self._desc)
        # Receive buffer for read(), sized to the channel's capacity.
        self._buf: Array[c_char] | None = None
        # An attach that peek()/read_into() reported: read() re-checks the
        # capacity.
        self._recheck = False
        # An attach consumed by a result that raised: reported by the next
        # result instead.
        self._pending = False

    def _handle(self) -> Any:
        h = self._h
        if h is None:
            raise _closed(self)
        return h

    def _attached(self) -> bool:
        attached = self._pending or bool(self._info.flags & _native.INFO_ATTACHED)
        self._pending = False
        return attached

    def _fit(self, h: Any, need: int = 0) -> None:
        """Resizes the receive buffer to the channel's capacity."""
        self._recheck = False
        rc = _native.state_describe_sized(h, self._desc_ref, sizeof(self._desc))
        if rc == _native.OK:
            size = self._desc.capacity
        elif rc == _native.E_NODATA:
            size = need
        else:
            raise error(rc, self._name)
        size = max(size, need)
        if size != (len(self._buf) if self._buf is not None else 0):
            self._buf = (c_char * size)() if size else None

    def read(self) -> Snapshot | None:
        """A copy of the latest value, or None if there is none."""
        h = self._handle()
        info = self._info
        attached = False
        resized = False
        for _ in range(_READ_ATTEMPTS):
            buf = self._buf
            size = len(buf) if buf is not None else 0
            rc = _native.state_read(h, buf, size, self._info_ref)
            if rc == _native.OK:
                attached |= self._attached()
                n = info.length
                # c_char arrays slice to bytes: the one copy read() makes.
                data = cast(bytes, buf[:n]) if buf is not None and n else b""
                if (attached and not resized) or self._recheck:
                    self._fit(h)
                return Snapshot(data, info.generation, info.timestamp_ns, attached)
            if rc == _native.E_NODATA:
                self._pending |= attached
                return None
            if rc != _native.E_TOOSMALL:
                self._pending |= attached
                raise error(rc, self._name)
            attached |= self._attached()
            self._fit(h, info.length)
            resized = True
        self._pending |= attached
        raise error(_native.E_BUSY, self._name)

    def read_into(self, buf: bytearray | memoryview) -> StateInfo | None:
        """Copies the latest value to the start of ``buf``, a writable
        contiguous buffer, without allocating. None if there is no value;
        raises ``PayloadTooLargeError`` if ``buf`` is too small."""
        h = self._handle()
        n = len(buf) if type(buf) is bytearray else memoryview(buf).nbytes
        ref: Any = byref(c_char.from_buffer(buf)) if n else None
        rc = _native.state_read(h, ref, min(n, _U32_MAX), self._info_ref)
        info = self._info
        if rc == _native.OK:
            attached = self._attached()
            self._recheck |= attached
            return StateInfo(info.generation, info.length, info.timestamp_ns, attached)
        if rc == _native.E_NODATA:
            return None
        if rc == _native.E_TOOSMALL:
            self._pending |= bool(info.flags & _native.INFO_ATTACHED)
            self._recheck |= self._pending
            raise error(
                rc,
                self._name,
                f"payload of {info.length} bytes does not fit in a {n}-byte buffer",
            )
        raise error(rc, self._name)

    def peek(self) -> StateInfo | None:
        """Generation, length and timestamp of the latest value, without
        copying it and without syscalls while attached. None if there is no
        value."""
        h = self._handle()
        rc = _native.state_peek(h, self._info_ref)
        if rc == _native.OK:
            info = self._info
            attached = self._attached()
            self._recheck |= attached
            return StateInfo(info.generation, info.length, info.timestamp_ns, attached)
        if rc == _native.E_NODATA:
            return None
        raise error(rc, self._name)

    def wait(self, last_generation: int = 0, timeout: float | None = None) -> bool:
        """Blocks until the generation differs from ``last_generation`` (0:
        until there is any value). ``timeout`` in seconds: None waits
        indefinitely, 0 polls once. False on timeout. Signal handlers run
        while waiting (a KeyboardInterrupt propagates); the wait then
        resumes with the remaining time (PEP 475)."""
        h = self._handle()
        last = _u32("last_generation", last_generation)
        deadline = None
        if timeout is not None:
            t = float(timeout)
            if not t >= 0:
                raise ValueError(f"timeout must be non-negative or None, not {timeout!r}")
            if t != math.inf:
                deadline = time.monotonic_ns() + int(t * 1e9)
        while True:
            if deadline is None:
                ms = -1
            else:
                remaining = deadline - time.monotonic_ns()
                ms = min(-(-remaining // 1_000_000), _I32_MAX) if remaining > 0 else 0
            rc = _native.state_wait(h, last, ms)
            if rc == _native.OK:
                return True
            if rc == _native.E_TIMEOUT:
                if deadline is not None and (ms == 0 or time.monotonic_ns() >= deadline):
                    return False
            elif rc == _native.E_INTR:
                _native.check_signals()
            else:
                raise error(rc, self._name)

    def writer_alive(self) -> bool:
        """Whether a writer holds the channel now. Makes syscalls; also
        reattaches if the channel file was replaced."""
        rc: int = _native.state_writer_alive(self._handle())
        if rc >= 0:
            return rc == 1
        raise error(rc, self._name)

    def describe(self) -> ChannelDesc | None:
        """The attached channel's constant properties; None if not attached."""
        rc = _native.state_describe_sized(self._handle(), self._desc_ref, sizeof(self._desc))
        if rc == _native.OK:
            d = self._desc
            return ChannelDesc(
                d.capacity,
                d.slot_count,
                d.payload_type,
                not d.flags & _native.STATE_NO_NOTIFY,
            )
        if rc == _native.E_NODATA:
            return None
        raise error(rc, self._name)

    @property
    def closed(self) -> bool:
        return self._h is None

    def close(self) -> None:
        h, self._h = self._h, None
        if h is not None:
            self._buf = None
            self._close_fn(h)

    def __enter__(self) -> Self:
        if self._h is None:
            raise _closed(self)
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        tb: TracebackType | None,
    ) -> None:
        self.close()

    def __del__(self) -> None:
        if getattr(self, "_h", None) is not None:
            try:
                warnings.warn(f"unclosed {self!r}", ResourceWarning, source=self, stacklevel=1)
            finally:
                self.close()

    def __repr__(self) -> str:
        return f"<StateReader {self._name!r}{' closed' if self._h is None else ''}>"
