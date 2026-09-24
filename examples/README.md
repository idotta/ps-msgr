# Examples

One writer and one reader per language, all on the same channel, `motor`,
with the same payload, so any writer works with any reader:

| | Writer | Reader |
|---|---|---|
| C | [`c/motor_writer.c`](c/motor_writer.c) | [`c/motor_reader.c`](c/motor_reader.c) |
| Python | [`python/motor_writer.py`](python/motor_writer.py) | [`python/motor_reader.py`](python/motor_reader.py) |
| C# | [`csharp/MotorWriter/`](csharp/MotorWriter/Program.cs) | [`csharp/MotorReader/`](csharp/MotorReader/Program.cs) |

The payload is `struct motor_status` in [`motor_status.h`](motor_status.h):
a `uint64_t` sequence number and three `float`s (speed, current,
temperature), 24 bytes with natural alignment, tagged with the payload type
`MOTOR_STATUS_V1` (`0x00010001`). [`python/motor_status.py`](python/motor_status.py)
(`ctypes.Structure`) and [`csharp/MotorStatus.cs`](csharp/MotorStatus.cs)
(`[StructLayout(Sequential)]`) mirror it.

## What they do

The programs behave the same in every language.

**Writer:** creates the channel with the struct's size as capacity and
`MOTOR_STATUS_V1` as payload type, replacing a channel left with another
geometry (`RECREATE`). It then publishes a changing status at `--rate` Hz
(default 10) and prints each value, until Ctrl-C or `--count` values.
Closing it releases the channel; the last value stays readable.

**Reader:** can start before the writer; it attaches once the channel
exists. It blocks in `wait`, then reads the latest value straight into the
struct (C `psmsgr_state_read`, Python `read_into(memoryview(status))`, C#
`TryRead<MotorStatus>`) and prints it, in the writer's format. The first
value from a newly attached channel file carries the *attached* flag; the
reader then checks the payload type with `describe` and refuses anything
but `MOTOR_STATUS_V1`. When `wait` times out (`--timeout` ms, default 300:
three periods at 10 Hz), it prints the age of the last value (`peek`) and
whether the writer is `stale` (alive but silent) or `writer gone`
(`writer_alive`), and keeps waiting. It exits on Ctrl-C or after `--count`
values.

Both take `--dir DIR` for the channel directory (default `$PSMSGR_DIR`,
else `/dev/shm`).

```
$ motor_reader
waiting for the writer
seq=1 speed=1505.0 rpm current=2.25 A temperature=40.5 C
seq=2 speed=1510.0 rpm current=2.50 A temperature=41.0 C
...
writer gone: last value 0.3 s old
```

## Running them

In the build container, after the `release` preset (which builds the C
examples, `PSMSGR_BUILD_EXAMPLES`), `check.sh` builds the rest and runs
the writers against the readers:

```sh
docker/run.sh cmake --workflow --preset release
docker/run.sh examples/check.sh
```

Then any writer × reader combination runs in a shell in the container
(`docker/run.sh bash`). The programs are:

| | Writer | Reader |
|---|---|---|
| C | `build/release/examples/c/motor_writer` | `build/release/examples/c/motor_reader` |
| Python | `examples/python/motor_writer.py` | `examples/python/motor_reader.py` |
| C# | `build/release/examples/csharp/MotorWriter/MotorWriter` | `build/release/examples/csharp/MotorReader/MotorReader` |

```sh
export PSMSGR_LIBRARY=$PWD/build/release/libpsmsgr.so.1   # for Python and C#
export PYTHONPATH=$PWD/bindings/python/src                 # or pip install bindings/python
examples/python/motor_reader.py --count 20 &
build/release/examples/csharp/MotorWriter/MotorWriter --count 30
```

- **C** builds as part of the library (`-DPSMSGR_BUILD_EXAMPLES=ON`, on in
  the `dev`, `dev-clang`, `release` and `armhf-release` presets), or on its
  own against an installed library, the way an application does:
  `cmake -S examples/c -B build/examples` finds it with
  `find_package(psmsgr)` (add `-DCMAKE_PREFIX_PATH=<prefix>` for a
  non-system install). The examples are not packaged.
- **Python** runs against the installed wheel, or the source tree with
  `PYTHONPATH=bindings/python/src`. `PSMSGR_LIBRARY` points it at a
  library that isn't installed.
- **C#** references `bindings/csharp/PsMsgr` as a project.
  `dotnet run --project examples/csharp/MotorReader -- --count 5` runs it
  with the JIT; `check.sh` publishes it with Native AOT:
  `dotnet publish examples/csharp/MotorReader -c Release -r linux-x64`.

`check.sh` builds the C examples in-tree and standalone (against the
release build installed to a scratch prefix), installs the Python wheel and
publishes the C# examples with Native AOT. It then runs every writer
against every reader, each reader started before its writer, and checks
that the reader prints values exactly as the writer sent them; checks
`writer gone` and `stale`; checks that every reader refuses a channel with
another payload type; and runs ruff and `dotnet format`. CI runs it in the
interop job.

## On the BeagleBone Black

The board needs the `libpsmsgr1` package from the `armhf-release` preset
(`build/armhf-release/libpsmsgr1_*_armhf.deb`); channels live in
`/dev/shm`.

- **C:** the `armhf-release` preset cross-builds
  `build/armhf-release/examples/c/motor_writer` and `motor_reader`: copy
  them to the board. Or build them there with `libpsmsgr-dev` installed:
  `cmake -S examples/c -B build && cmake --build build`.
- **Python:** build the wheel (`python3 -m pip wheel --no-deps
  bindings/python`; it is pure Python), `pip install` it on the board, and
  copy `examples/python/*.py`.
- **C#:** publish with Native AOT for `linux-arm` in the build container
  (32-bit ARM is a Native AOT target since .NET 9). The container's host
  `objcopy` does not read ARM binaries, so name the cross one:

  ```sh
  docker/run.sh dotnet publish examples/csharp/MotorReader -c Release -r linux-arm \
      -p:ObjCopyName=arm-linux-gnueabihf-objcopy -o build/armhf-release/examples/csharp/MotorReader
  ```

  This produces a self-contained executable that needs only glibc and
  `libpsmsgr.so.1`; no .NET runtime on the board. To drop the `.so` too,
  link `libpsmsgr.a` into the executable with `DirectPInvoke`
  ([`bindings/csharp/README.md`](../bindings/csharp/README.md#native-aot)).
  `check.sh` publishes both examples this way, to
  `build/release/examples/csharp-linux-arm/`, on x86-64 hosts. They also
  run in the container under qemu, against the `armhf-release` library:
  `qemu-arm -L /usr/arm-linux-gnueabihf -E LD_LIBRARY_PATH=<dir with its libpsmsgr.so.1> …`.
  None of the examples has run on the board yet.
