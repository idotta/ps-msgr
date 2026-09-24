# ps-msgr for Python

`ps_msgr` is a `ctypes` binding of `libpsmsgr`: state channels publish the
latest value of a payload from one writer to many readers through shared
memory. Pure Python, no dependencies, Python 3.11 or later. The interface is
specified in [`spec/bindings.md`](../../spec/bindings.md).

## Install

The binding needs the C library, `libpsmsgr.so.1`: on the target, install
the `libpsmsgr1` package. Then install the binding:

```sh
pip install bindings/python
```

At import, `ps_msgr` loads `libpsmsgr.so.1` through the normal library
search path. To use another copy, such as a build tree, set
`PSMSGR_LIBRARY` to its path:

```sh
export PSMSGR_LIBRARY=$PWD/build/release/libpsmsgr.so.1
```

The import fails with an `ImportError` if the library is missing or its
major version differs, or its minor version is older than the binding needs.

## Usage

```python
import struct
from ps_msgr import StateReader, StateWriter

IMU_V1 = 0x0001_0001
imu = struct.Struct("<Qfff")  # sequence number, x, y, z

# Writer
with StateWriter("imu", imu.size, payload_type=IMU_V1) as w:
    generation = w.publish(imu.pack(1, 0.0, 0.0, 9.81))

# Reader: may start before the writer
with StateReader("imu") as r:
    seen = 0
    while r.wait(seen, timeout=0.5):  # False after 0.5 s without a change
        snap = r.read()
        if snap is None:
            continue
        if snap.attached and r.describe().payload_type != IMU_V1:
            raise RuntimeError("unexpected payload type")
        seen = snap.generation
        seq, x, y, z = imu.unpack(snap.data)
```

- `read()` returns a `Snapshot` with a copy of the value; `read_into(buf)`
  copies into a `bytearray` or writable `memoryview` without allocating.
  Both return `None` while there is no value.
- `peek()` returns the generation, length and timestamp without copying.
  `info.age_ns` is the value's age on `CLOCK_MONOTONIC`.
- `wait()` releases the GIL while it blocks. Ctrl-C raises
  `KeyboardInterrupt` as usual.
- Channels live in `/dev/shm` unless `directory=` or `$PSMSGR_DIR` says
  otherwise.
- Errors raise `PsMsgrError` (an `OSError`) or one of its subclasses; `code`
  is the library's result code.
- Handles are not thread-safe: use one per thread.

## Development

The tests run against a build of the C library. In the build container:

```sh
docker/run.sh cmake --workflow --preset release
docker/run.sh bindings/python/check.sh
```

`check.sh` builds and installs the wheel into a scratch directory, runs
pytest against it with `PSMSGR_LIBRARY` pointing at `build/release`, and
runs `ruff check` and `ruff format --check`.
