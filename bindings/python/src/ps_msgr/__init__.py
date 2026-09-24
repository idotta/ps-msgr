# SPDX-License-Identifier: Apache-2.0
"""ps-msgr state channels: the latest value of a payload, published by one
writer to many readers through shared memory. A ctypes binding of
libpsmsgr.so.1; ``$PSMSGR_LIBRARY`` overrides the library path."""

from ._errors import (
    ChannelBusyError,
    ChannelFormatError,
    ChannelMismatchError,
    ErrorCode,
    PayloadTooLargeError,
    PsMsgrError,
    WriterExistsError,
)
from ._state import (
    ChannelDesc,
    Snapshot,
    StateInfo,
    StateReader,
    StateWriter,
    now_ns,
    unlink,
)

__all__ = [
    "ChannelBusyError",
    "ChannelDesc",
    "ChannelFormatError",
    "ChannelMismatchError",
    "ErrorCode",
    "PayloadTooLargeError",
    "PsMsgrError",
    "Snapshot",
    "StateInfo",
    "StateReader",
    "StateWriter",
    "WriterExistsError",
    "now_ns",
    "unlink",
]
