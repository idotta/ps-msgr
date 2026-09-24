# Bindings — Python and C#

Status: **draft**.

Both bindings are thin wrappers over `libpsmsgr.so.1`. They MUST NOT access
the channel files directly and MUST NOT reimplement any part of
[state-channel.md](state-channel.md). What they add is idiomatic types,
error mapping, and buffer management.

The rules that apply to both:

- Load `libpsmsgr.so.1` by SONAME, not `libpsmsgr.so`: the unversioned
  symlink only exists in dev packages. Honor an override path in the
  environment variable `PSMSGR_LIBRARY`.
- At load time, check `psmsgr_version()`: same major version, and at least
  the minimum minor version the binding needs. Fail loudly otherwise.
- A reader owns one receive buffer of `capacity` bytes, allocated on first
  attach and reallocated if the channel is recreated with a different
  capacity. Reads never allocate except for the returned copy
  (Python `bytes`, C# convenience overloads).
- `PSMSGR_E_NODATA` is not an error: it maps to `None` / `false`. Every other
  negative code maps to an exception that carries the code and, for
  `PSMSGR_E_SYS`, the `errno`.
- `wait` handles `PSMSGR_E_INTR` itself. It checks for cancellation
  (Python signals, C# `CancellationToken`) and then retries with the
  remaining timeout.
- Timestamps are `CLOCK_MONOTONIC` ns, and each binding exposes the library's
  `now_ns()`, so an age is always `now_ns() - info.timestamp_ns`, computed on
  the same clock.
- Handles are closed deterministically (context manager / `IDisposable`),
  with a finalizer as a safety net.

## Python — `ps_msgr`

- Python ≥ 3.9 (Debian 11 on older BeagleBone images). No runtime
  dependencies. `ctypes`, so it is a pure-Python wheel.
- `ctypes` releases the GIL during foreign calls, so a blocking `wait`
  doesn't stall other threads.
- When `wait` gets `PSMSGR_E_INTR`, the binding returns to the interpreter
  so that pending signal handlers run (`KeyboardInterrupt` is raised
  normally), then retries with the remaining time, following PEP 475.

```python
from ps_msgr import StateWriter, StateReader, Snapshot, StateInfo, now_ns, unlink

class StateWriter:                        # context manager
    def __init__(self, name: str, capacity: int, *, slot_count: int = 3,
                 payload_type: int = 0, mode: int = 0o644,
                 recreate: bool = False, notify: bool = True,
                 directory: str | os.PathLike | None = None) -> None: ...
    def publish(self, data: bytes | bytearray | memoryview) -> int: ...  # -> generation
    @property
    def capacity(self) -> int: ...
    def close(self) -> None: ...

class StateReader:                        # context manager
    def __init__(self, name: str, *, directory: str | os.PathLike | None = None) -> None: ...
    def read(self) -> Snapshot | None: ...                    # None: no data
    def read_into(self, buf: bytearray | memoryview) -> StateInfo | None: ...  # zero-alloc
    def peek(self) -> StateInfo | None: ...                   # no copy, no syscall
    def wait(self, last_generation: int = 0,
             timeout: float | None = None) -> bool: ...       # False on timeout
    def writer_alive(self) -> bool: ...
    def describe(self) -> ChannelDesc | None: ...
    def close(self) -> None: ...

@dataclass(frozen=True)
class StateInfo:  generation: int; length: int; timestamp_ns: int
    # .age_ns property: now_ns() - timestamp_ns

@dataclass(frozen=True)
class Snapshot:   data: bytes; generation: int; timestamp_ns: int

@dataclass(frozen=True)
class ChannelDesc: capacity: int; slot_count: int; payload_type: int; notify: bool

def now_ns() -> int: ...
def unlink(name: str, *, directory: str | os.PathLike | None = None) -> bool: ...

class PsMsgrError(OSError): code: int     # base; errno set for PSMSGR_E_SYS
class WriterExistsError(PsMsgrError): ...
class ChannelMismatchError(PsMsgrError): ...
class ChannelFormatError(PsMsgrError): ...
class PayloadTooLargeError(PsMsgrError, ValueError): ...
```

`read_into` raises `PayloadTooLargeError` if `buf` is too small. Payload
encoding is up to the application: `struct`, `ctypes.Structure`,
`numpy.frombuffer`, etc.

## C# — `PsMsgr`

- The library targets `net8.0`, so it also runs on newer runtimes (use the
  current LTS runtime on the device). It needs `AllowUnsafeBlocks` and uses
  `LibraryImport` source-generated P/Invoke, with no marshalling on the hot
  path.
- A `NativeLibrary.SetDllImportResolver` maps `"psmsgr"` to
  `$PSMSGR_LIBRARY` or `libpsmsgr.so.1`.
- The NuGet package MAY bundle `runtimes/linux-arm/native/libpsmsgr.so.1` and
  `runtimes/linux-x64/native/libpsmsgr.so.1` (for development). Otherwise the
  library comes from the system.
- Native handles are wrapped in `SafeHandle` subclasses.

```csharp
namespace PsMsgr;

public sealed class StateWriter : IDisposable
{
    public static StateWriter Open(string name, StateOptions options);
    public uint Capacity { get; }
    public uint Publish(ReadOnlySpan<byte> data);                     // -> generation
    public uint Publish<T>(in T value) where T : unmanaged;           // sizeof(T) bytes
    public WriteScope Begin();       // ref struct: Span<byte> Buffer; uint Commit(int length); Dispose() aborts if not committed
    public void Dispose();
}

public sealed record StateOptions
{
    public required uint Capacity { get; init; }
    public uint SlotCount   { get; init; } = 3;
    public uint PayloadType { get; init; }
    public UnixFileMode Mode { get; init; } =                          // 0644
        UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.GroupRead | UnixFileMode.OtherRead;
    public bool Recreate    { get; init; }
    public bool Notify      { get; init; } = true;
    public string? Directory { get; init; }
}

public sealed class StateReader : IDisposable
{
    public static StateReader Open(string name, string? directory = null);
    public bool TryRead(Span<byte> destination, out StateInfo info); // false: no data; throws if too small
    public bool TryRead<T>(out T value, out StateInfo info) where T : unmanaged; // throws if length != sizeof(T)
    public byte[]? Read(out StateInfo info);                          // allocating convenience
    public bool TryPeek(out StateInfo info);                          // no copy, no syscall
    public bool Wait(uint lastGeneration, TimeSpan timeout, CancellationToken ct = default);
    public bool IsWriterAlive { get; }
    public ChannelDesc? Describe();
    public void Dispose();
}

public readonly record struct StateInfo(uint Generation, uint Length, ulong TimestampNs)
{
    public TimeSpan Age => /* from Clock.NowNs() - TimestampNs */;
}
public readonly record struct ChannelDesc(uint Capacity, uint SlotCount, uint PayloadType, bool Notify);

public static class Clock   { public static ulong NowNs(); }             // psmsgr_now_ns
public static class Channel { public static bool Unlink(string name, string? directory = null); }

public class PsMsgrException : IOException { public PsMsgrError Code { get; } public int Errno { get; } }
public enum PsMsgrError { Inval = -1, Sys = -2, /* ... mirrors PSMSGR_E_* */ }
```

- `Wait` with a `CancellationToken` waits in slices of at most 100 ms and
  checks the token between slices. `Timeout.InfiniteTimeSpan` means no
  timeout.
- `Publish<T>` / `TryRead<T>` copy the raw bytes of `T`. Declare `T` with
  `[StructLayout(LayoutKind.Sequential, Pack = …)]`, matching the C
  definition.

## Payload conventions (non-normative)

The library never looks at the payload. Some suggestions for applications:

- For hot, fixed-size state, use plain structs with fixed-width
  little-endian fields and explicit padding, defined once in a C header and
  mirrored in `ctypes.Structure` / `[StructLayout]`.
- Put a schema identifier in `payload_type`, e.g. a FourCC in the upper
  16 bits and a schema version in the lower 16. Readers check it with
  `describe()` before trusting the bytes.
- Variable or evolving data can use a self-describing encoding (MessagePack,
  CBOR, protobuf, FlatBuffers), with the size cost counted against
  `capacity`.
