# Build, packaging and test

Status: **draft**.

## Repository layout

```
README.md
spec/                         this directory: the contract
c/
  CMakeLists.txt
  cmake/arm-linux-gnueabihf.cmake   cross toolchain file (BeagleBone Black)
  include/psmsgr/psmsgr.h
  include/psmsgr/state.h
  src/                        implementation (state.c, futex/lock helpers, …)
  tools/psmsgr-dump.c
  tests/                      unit + torture tests (CTest)
  bench/                      latency/throughput benchmarks
python/
  pyproject.toml
  src/ps_msgr/                ctypes binding (src layout: tests run against the installed package)
  tests/                      pytest
csharp/
  PsMsgr.sln
  PsMsgr/PsMsgr.csproj
  PsMsgr.Tests/PsMsgr.Tests.csproj
interop/                      cross-language tests (C ⇄ Python ⇄ C#)
examples/                     one small writer/reader pair per language
.github/workflows/
```

## C library

- CMake ≥ 3.16, C11, and no dependencies beyond glibc. Needs Linux ≥ 3.15
  for OFD locks. `_GNU_SOURCE` is set internally.
- Build outputs:
  - `libpsmsgr.so.1` (SONAME) and `libpsmsgr.a`
  - `psmsgr-dump`
  - a CMake package config (`find_package(psmsgr)`) and `psmsgr.pc`
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion`, promoted to `-Werror`
  in CI. Built with `-fvisibility=hidden`.
- Cross build for the BBB: `cmake -DCMAKE_TOOLCHAIN_FILE=c/cmake/arm-linux-gnueabihf.cmake`.
  Keep Debian armhf's default code generation (ARMv7-A, Thumb-2, VFPv3-D16).
  The library has no use for NEON-specific flags.
- Options:

  | Option | Default | Effect |
  |---|---|---|
  | `PSMSGR_BUILD_TESTS` | `ON` | Build the unit and torture tests. |
  | `PSMSGR_BUILD_TOOLS` | `ON` | Build `psmsgr-dump`. |
  | `PSMSGR_BUILD_BENCH` | `OFF` | Build the benchmarks. |
  | `PSMSGR_SANITIZE` | empty | Sanitizer to enable: `address`, `undefined` or `thread`. |

- Packaging: CPack DEB for `armhf` and `amd64` produces `libpsmsgr1`,
  `libpsmsgr-dev` and `psmsgr-tools`. Installing the `.deb` is the supported
  deployment path on the BBB.

## Python

`pyproject.toml` with a PEP 517 backend (hatchling or setuptools). Tests use
pytest and need the built C library (`PSMSGR_LIBRARY=<build>/libpsmsgr.so.1`).

## C#

SDK-style projects. The library targets `netstandard2.1`. The test project
must target a concrete runtime: the current .NET LTS, with xUnit. On CI, point
`PSMSGR_LIBRARY` at the built library, which also exercises the `dlopen`
preload path.

## Tests

Every test uses its own temporary directory as the channel `dir`, never
`/dev/shm`, so that tests can run in parallel.

### Unit (C; mirrored where relevant in the bindings)

- Name validation, and the options defaults and limits.
- Create, then reuse, with the generation carried across a writer restart.
- `MISMATCH`, and `RECREATE` → old file `RETIRED`, readers reattach.
- A second writer in the same process and in another process gets
  `WRITER_EXISTS`. The lock is released when the holder is killed with
  `SIGKILL`.
- Crash mid-publish: a slot left with an odd `seq` is recovered, and stale
  `.tmp` files are cleaned up.
- A lazy reader opened before the writer exists attaches after the first
  publish.
- `NODATA` before the first publish, `TOOSMALL` (with the length reported),
  `TOOBIG`, and `capacity == 0` heartbeat channels.
- `begin`/`commit`/`abort`, and `publish` during an open `begin`
  (`STATE`).
- Generation wrap: a test hook sets the generation near `UINT32_MAX`, and the
  test checks that 0 is skipped.
- `peek`: the timestamp is monotonic across publishes and close to
  `psmsgr_now_ns()`.
- `wait`: wakes on publish, times out, returns 0-timeout immediately, wakes
  on retire and on unlink, returns `NOTSUP` on `NO_NOTIFY` channels, and
  returns `INTR` when a signal arrives without `SA_RESTART`.
- `writer_alive` before open, during, after close, and after `SIGKILL`.
- `ENOSPC` from `posix_fallocate` on a size-limited tmpfs, where the
  environment allows mounting one.

### Torture (C)

One writer and N readers (threads *and* processes), for a fixed duration:

- Each payload is `{generation, length, bytes derived from generation, CRC32}`,
  with a random length in `[0, capacity]`.
- Every successful read MUST validate: the CRC is correct, and the embedded
  generation equals `info.generation`.
- Per reader, the sequence of generations read MUST be non-decreasing (as a
  wrapped sequence).
- `BUSY` is counted and reported, and must stay rare.
- Variants cover `slot_count` 2 and 3, and capacities from 16 B to 1 MiB.

### Interop

Each binding writes and each other binding reads, including a C writer that
recreates the channel under Python and C# readers.

### Sanitizers

- ASan+UBSan on the unit and torture tests.
- TSan with the payload copy annotated (see state-channel.md §5.4). Any
  other report is a bug.

## CI matrix

| Job | Purpose |
|---|---|
| x86-64, gcc + clang, ASan/UBSan, TSan | Main correctness gate. |
| AArch64 native runner, torture test | Weakly ordered memory on real hardware. x86 hides ordering bugs. |
| armhf cross build + tests under `qemu-arm` | Target ABI (32-bit atomics, alignment, `time_t`). |
| Python (x86-64) | Binding tests plus interop. |
| C# (x86-64) | Binding tests plus interop on .NET LTS; a Mono smoke test of the `netstandard2.1` assembly. |
| CPack | Build the `.deb` for armhf and amd64. |

## On-target validation (before each release)

Run the torture test and `bench/` on a real BeagleBone Black. Record in the
release notes the publish, read and peek latency for 16 B, 256 B, 4 KiB and
64 KiB payloads, and the wake-up latency of `wait`. There are no numeric
targets until the first measurement.
