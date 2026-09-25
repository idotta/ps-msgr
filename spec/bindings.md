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
- A reader owns one receive buffer of `capacity` bytes. When a result
  carries `PSMSGR_INFO_ATTACHED`, the binding calls `describe()` and
  reallocates the buffer if the capacity changed. Reads never allocate
  except for the returned copy (Python `bytes`, C# convenience overloads).
  - A result that doesn't fill the owned buffer (`peek`, reads into a
    caller's buffer) only marks the buffer for that check at the next read
    into it.
  - If a read into the owned buffer still gets `PSMSGR_E_TOOSMALL`, the
    binding calls `describe()`, resizes and retries (a few times, then
    `PSMSGR_E_BUSY`).
- `PSMSGR_INFO_ATTACHED` is exposed as `StateInfo.attached` / `Attached`
  (and on Python's `Snapshot`), so applications can re-check `payload_type`.
  Like the C flag it is reported once per attach. If the result that
  carried it raises instead (`PSMSGR_E_TOOSMALL` from a read into a
  caller's buffer), the next result reports it.
- `PSMSGR_E_NODATA` is not an error: it maps to `None` / `false` / `null`. Every other
  negative code maps to an exception that carries the code and, for
  `PSMSGR_E_SYS`, the `errno`. `PSMSGR_E_BUSY` gets its own exception type
  (`ChannelBusyError` / `PsMsgrError.Busy`), documented as transient.
- `wait` handles `PSMSGR_E_INTR` itself. It checks for cancellation
  (Python signals, C# `CancellationToken`) and then retries with the
  remaining timeout.
- Timestamps are `CLOCK_MONOTONIC` ns, and each binding exposes the library's
  `now_ns()`, so an age is always `now_ns() - info.timestamp_ns`, computed on
  the same clock.
- Handles are closed deterministically (context manager / `IDisposable`),
  with a finalizer as a safety net.

## Python — `ps_msgr`

- Python ≥ 3.11. The target (trixie) ships 3.13; the lower floor only
  exists so that development hosts with an older Python still work. No
  runtime dependencies. `ctypes`, so it is a pure-Python wheel, built with
  setuptools.
- Every C function is declared with explicit `argtypes` and `restype`, and
  the three structs are mirrored as `ctypes.Structure`. Options are set up
  with `psmsgr_state_options_init_sized(opt, sizeof(opt))`, and `describe`
  calls `psmsgr_state_describe_sized(r, desc, sizeof(desc))`. The binding's
  tests compare every size, offset and constant with a helper compiled from
  the C headers (`tests/interop_helper.c`).
- Loading: `ctypes` opens `$PSMSGR_LIBRARY` if set, else `libpsmsgr.so.1`.
  A missing library, a missing symbol or an incompatible version raises
  `ImportError` at import, naming the library and what is wrong.
- The GIL: `wait` and the calls that do file system work (opening a
  writer or a reader, `writer_alive`, `unlink`) release it, so a blocking
  `wait` doesn't stall other threads. `publish`, `read`, `peek`,
  `describe`, `now_ns` and `close` keep it (`ctypes.PyDLL`): releasing and
  reacquiring the GIL would cost more than these calls, and with other busy
  threads reacquiring can take a whole switch interval.
- When `wait` gets `PSMSGR_E_INTR`, the binding returns to the interpreter
  so that pending signal handlers run (`KeyboardInterrupt` is raised
  normally), then retries with the remaining time, following PEP 475.
  Signals reach Python handlers only in the main thread.
- Timeouts are float seconds: `None` (or `math.inf`) waits indefinitely, `0`
  polls once, and a positive value is rounded up to whole milliseconds per
  call, so it never becomes a poll. `wait` waits in slices of at most
  100 ms. A finite timeout longer than about 292 years (2^63 - 1
  nanoseconds) is clamped to that. Negative or NaN raises `ValueError`.
- Integer arguments are checked against their C type (`uint32_t`) and
  raise `ValueError` out of range, instead of being truncated by `ctypes`.
  A channel name or directory with a NUL character raises `ValueError`. The
  library validates everything else (`PSMSGR_E_INVAL`).
- `publish` takes any C-contiguous buffer (`bytes`, `bytearray`, writable
  `memoryview`, `array`, `ctypes.Structure`, …) without copying it first;
  a read-only buffer that isn't `bytes`, or a non-contiguous one, is copied
  once. `read_into` needs a writable C-contiguous buffer (`TypeError` or
  `BufferError` otherwise) and writes the value to its start.
- Handles close deterministically with `close()` or `with`. `__del__`
  closes a forgotten handle and emits a `ResourceWarning`, like an unclosed
  file. Calls on a closed handle raise `ValueError`. Like the C handles,
  the objects are not thread-safe: use one per thread.
- The one exception: another thread may `close()` a reader during `wait` or
  `writer_alive`, which release the GIL. `close()` returns at once and the
  reader counts as closed; the call in progress closes the native handle
  when it returns, and `wait` raises `ValueError` before its next slice.
  This is the way to stop a thread blocked in `wait(timeout=None)`. A
  per-reader lock and a count of those calls in progress implement it.

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
    @property
    def closed(self) -> bool: ...
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
    @property
    def closed(self) -> bool: ...
    def close(self) -> None: ...

@dataclass(frozen=True, slots=True)
class StateInfo:  generation: int; length: int; timestamp_ns: int; attached: bool
    # .age_ns property: now_ns() - timestamp_ns

@dataclass(frozen=True, slots=True)
class Snapshot:   data: bytes; generation: int; timestamp_ns: int; attached: bool

@dataclass(frozen=True, slots=True)
class ChannelDesc: capacity: int; slot_count: int; payload_type: int; notify: bool

def now_ns() -> int: ...
def unlink(name: str, *, directory: str | os.PathLike | None = None) -> bool: ...  # False: absent

class ErrorCode(IntEnum): INVAL = -1; SYS = -2; ...  # mirrors PSMSGR_E_*

class PsMsgrError(OSError):               # base; errno set for PSMSGR_E_SYS
    code: int                             # an ErrorCode for known codes
    def __init__(self, code: int, message: str | None = None,
                 errno: int | None = None, filename: str | None = None) -> None: ...
class WriterExistsError(PsMsgrError): ...
class ChannelMismatchError(PsMsgrError): ...
class ChannelFormatError(PsMsgrError): ...
class PayloadTooLargeError(PsMsgrError, ValueError): ...
class ChannelBusyError(PsMsgrError): ...      # transient: retry
```

- `filename` is the channel name. `strerror` is `os.strerror(errno)` for
  `PSMSGR_E_SYS`, else `psmsgr_strerror(code)`. The exceptions pickle.
- `PayloadTooLargeError` has code `TOOBIG` from `publish`, and `TOOSMALL`
  from `read_into` when `buf` is too small.
- Other codes, e.g. `INVAL`, or `NOTSUP` from `wait` on a channel without
  notification, raise `PsMsgrError` itself.

Payload encoding is up to the application: `struct`, `ctypes.Structure`,
`numpy.frombuffer`, etc.

## C# — `PsMsgr`

- Target framework: **`netstandard2.1`** only. That one build runs on
  .NET Core 3.x / .NET 5+ and can be compiled into Native AOT apps. No
  package dependencies.
  - Built with `LangVersion` `latest`, `Nullable` `enable`,
    `AllowUnsafeBlocks` and `TreatWarningsAsErrors`.
  - Compiler attributes that `netstandard2.1` lacks (`IsExternalInit` for
    records and `init`) are defined `internal`.
- **Mono is not supported.** Mono resolves a P/Invoke when it compiles the
  calling method, before the static constructor that honors
  `PSMSGR_LIBRARY` runs, and Mono 6.12's corlib (trixie's) lacks the
  `ref readonly` signature of `ReadOnlySpan<T>.GetPinnableReference`, which
  `fixed` on a span binds to.

### Native AOT compatibility

The library MUST work in Native AOT apps. The proof is that a Native AOT app
using it publishes with **zero** trim/AOT warnings (build-and-test.md).
Rules that keep it that way:

- No reflection, `dynamic`, `Type`-based marshalling
  (`Marshal.SizeOf(Type)`, `Marshal.PtrToStructure(IntPtr, Type)`),
  `Activator`, `Expression`, `Reflection.Emit` or runtime-generated code.
  Generic payload helpers are constrained to `unmanaged` and use
  `sizeof(T)` and pointers. They use no
  `Unsafe` class: on `netstandard2.1` that would pull in a package
  dependency, and the library has none.
- No P/Invoke callbacks or delegates. The C API has none, and it MUST NOT
  grow any that the binding would need.
- Blittable P/Invoke signatures only (see *Native interop*). Native AOT
  compiles these without marshalling logic.
- Native AOT on the BeagleBone Black needs a **.NET 9+** app, because
  linux-arm (32-bit) is a Native AOT target only since .NET 9. Use the
  current LTS runtime.
- **Static linking (MAY):** an AOT app can link `libpsmsgr.a` into its
  executable with `<DirectPInvoke Include="libpsmsgr.so.1" />` and
  `<NativeLibrary Include="…/libpsmsgr.a" />`, so no `.so` needs deploying on
  the device.

### Native interop

- P/Invoke uses `[DllImport("libpsmsgr.so.1")]` with the versioned name
  hard-coded, and
  **blittable signatures only**: raw pointers, integers, and `byte*` for
  strings. The three structs are mirrored with `[StructLayout(Sequential)]`,
  and the tests compare their sizes and offsets, and every constant, with
  `tests/interop_helper layout`. Options are set up with
  `psmsgr_state_options_init_sized(&opt, sizeof(opt))`, and `Describe` calls
  `psmsgr_state_describe_sized(r, &desc, sizeof(desc))`.
  - The binding encodes strings to NUL-terminated UTF-8 itself, only in
    `Open` and `Unlink` (not the hot path).
  - Native handles live in `SafeHandle` subclasses. The P/Invoke signatures
    take the raw pointer (`DangerousGetHandle()`), and the wrapper checks for
    disposal first. Wrapper objects are not thread-safe, which matches the C
    API contract, so this is sound, and it avoids SafeHandle marshalling on
    the hot path. Each call is followed by `GC.KeepAlive(this)`, so that the
    handle's finalizer cannot close it during the call (e.g. a blocking
    `Wait` on an otherwise unreferenced reader).
  - The exception: another thread may `Dispose` a reader during `Wait` or
    `IsWriterAlive`. These two hold a reference on the handle
    (`DangerousAddRef`/`DangerousRelease`, instead of `GC.KeepAlive`), so
    the native handle closes when the call returns. The reader keeps its
    own disposed flag, because a `SafeHandle` with a reference outstanding
    doesn't report `IsClosed`. `Wait` then throws `ObjectDisposedException`
    before its next slice.
  - Calls that can return `PSMSGR_E_SYS` according to the header
    (`writer_open`, `reader_open`, `read`, `peek`, `wait`, `writer_alive`,
    `describe`, `unlink`) use `SetLastError = true`. `errno` is read with
    `Marshal.GetLastWin32Error()`, which works on Unix and in AOT, and its
    text comes from libc's `strerror`.
- `PSMSGR_LIBRARY` override: `NativeLibrary` isn't available on
  `netstandard2.1`. The static
  constructor of the interop class calls
  `dlopen($PSMSGR_LIBRARY, RTLD_NOW | RTLD_GLOBAL)` through
  `[DllImport("libdl.so.2")]` when the variable is set. glibc reuses an
  already-loaded library whose SONAME matches a later `dlopen` of that name,
  so the `DllImport` resolves to the preloaded copy. This works identically
  under JIT and Native AOT: both bind a P/Invoke at its first call, after the
  static constructor has run (a bare P/Invoke such as `Clock.NowNs()` runs
  it too). Without the variable, the normal search path applies
  (`LD_LIBRARY_PATH`, `ld.so.cache`).
- Load failures surface at the first call as a `TypeInitializationException`
  whose inner exception is a `DllNotFoundException` naming the library and
  the loader's error (a missing library, or a missing `psmsgr_version`), or
  a `PsMsgrException` with code `NotSup` for an incompatible version.
- The NuGet package MAY bundle `runtimes/linux-arm/native/libpsmsgr.so.1` and
  `runtimes/linux-x64/native/libpsmsgr.so.1` (for development). It doesn't
  yet: the library comes from the system.

```csharp
namespace PsMsgr;

public sealed class StateWriter : IDisposable
{
    public static StateWriter Open(string name, StateOptions options);
    public uint Capacity { get; }
    public uint Publish(ReadOnlySpan<byte> data);                     // -> generation
    public uint Publish<T>(in T value) where T : unmanaged;           // sizeof(T) bytes
    public WriteScope Begin();
    public void Dispose();                                            // aborts an open WriteScope
}

public readonly ref struct WriteScope                                 // use with `using`
{
    public Span<byte> Buffer { get; }                                 // Capacity bytes, 32-byte aligned
    public uint Commit(int length);                                   // -> generation; TooBig keeps it open
    public void Dispose();                                            // aborts unless committed
}

public sealed class StateOptions
{
    public StateOptions(uint capacity) { Capacity = capacity; }
    public uint Capacity    { get; }
    public uint SlotCount   { get; set; } = 3;
    public uint PayloadType { get; set; }
    public uint Mode        { get; set; } = 0x1A4;                   // 0644 (C# has no octal literals)
    public bool Recreate    { get; set; }
    public bool Notify      { get; set; } = true;
    public string? Directory { get; set; }
}

public sealed class StateReader : IDisposable
{
    public static StateReader Open(string name, string? directory = null);
    public bool TryRead(Span<byte> destination, out StateInfo info); // false: no data; TooSmall if too small
    public bool TryRead<T>(out T value, out StateInfo info) where T : unmanaged; // Mismatch if length != sizeof(T)
    public byte[]? Read(out StateInfo info);                          // allocating convenience; null: no data
    public bool TryPeek(out StateInfo info);                          // no copy, no syscall
    public bool Wait(uint lastGeneration, TimeSpan timeout, CancellationToken cancellationToken = default);
    public bool IsWriterAlive { get; }
    public ChannelDesc? Describe();                                   // null: not attached
    public void Dispose();
}

public readonly record struct StateInfo(uint Generation, uint Length, ulong TimestampNs, bool Attached)
{
    public TimeSpan Age { get; }                                      // Clock.NowNs() - TimestampNs
}
public readonly record struct ChannelDesc(uint Capacity, uint SlotCount, uint PayloadType, bool Notify);

public static class Clock   { public static ulong NowNs(); }             // psmsgr_now_ns
public static class Channel { public static bool Unlink(string name, string? directory = null); } // false: absent

public class PsMsgrException : IOException
{
    public PsMsgrException(PsMsgrError code, string? message = null, int errno = 0, string? channelName = null);
    public PsMsgrError Code { get; }
    public int Errno { get; }                                         // PSMSGR_E_SYS only, else 0
    public string? ChannelName { get; }
}
public enum PsMsgrError { Inval = -1, Sys = -2, NoData = -3, TooSmall = -4, TooBig = -5, Busy = -6,
    Timeout = -7, Intr = -8, WriterExists = -9, Mismatch = -10, Format = -11, NotSup = -12, State = -13 }
```

- Every failing call throws `PsMsgrException`; there are no subclasses,
  the `Code` tells them apart. The message is `strerror(errno)` with
  `(errno N)` for `Sys`, else `psmsgr_strerror(code)` or the binding's own
  text, followed by `: 'channel'`, e.g.
  `No such file or directory (errno 2): 'chan'`.
- `TryRead<T>` fails with `Mismatch` when the payload length isn't
  `sizeof(T)`, in either direction. Like a `TooSmall` from `TryRead`, such a
  result passes its `Attached` on to the next result.
- `Wait`: `Timeout.InfiniteTimeSpan` means no timeout, `TimeSpan.Zero` polls
  once, and a positive timeout is rounded up to whole milliseconds per call,
  so it never becomes a poll. Timeouts beyond the C API's `int32_t`
  milliseconds (up to `TimeSpan.MaxValue`) are waited in chunks. It waits in
  slices of at most 100 ms, and before each slice checks the
  `CancellationToken` (`OperationCanceledException`) and whether the reader
  was disposed (`ObjectDisposedException`).
  `PSMSGR_E_INTR` is expected, not only from the application: the runtime
  signals threads too. `Wait` retries with the remaining time.
- `WriteScope` holds only an id; the writer holds the state. So copies of a
  scope, including the read-only variable of a `using`, stay coherent: a
  commit through one ends them all, and `Dispose` after a commit does
  nothing. `Buffer` and `Commit` on an ended scope throw
  `InvalidOperationException`.
- Argument validation: a `null` name or options throw
  `ArgumentNullException`, a NUL character in a name or directory
  `ArgumentException`, a negative timeout (other than `InfiniteTimeSpan`) or
  commit length `ArgumentOutOfRangeException`. The library validates
  everything else (`Inval`). The integer arguments are already `uint`.
- Calls on a disposed handle throw `ObjectDisposedException`; `Dispose` is
  idempotent. `SafeHandle`'s finalizer closes a forgotten handle.
- `Publish<T>` / `TryRead<T>` copy the raw bytes of `T`. Declare `T` with
  `[StructLayout(LayoutKind.Sequential, Pack = …)]`, matching the C
  definition.

## Payload conventions (non-normative)

The library never looks at the payload. Some suggestions for applications:

- For hot, fixed-size state, use plain structs with fixed-width
  little-endian fields and explicit padding, defined once in a C header and
  mirrored in `ctypes.Structure` / `[StructLayout]`.
- Put a schema identifier in `payload_type`, e.g. a 16-bit schema ID in the
  upper half and a 16-bit schema version in the lower half. Readers check it with
  `describe()` before trusting the bytes.
- Variable or evolving data can use a self-describing encoding (MessagePack,
  CBOR, protobuf, FlatBuffers), with the size cost counted against
  `capacity`.
