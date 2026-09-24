# Build, packaging and test

Status: **draft**.

## Repository layout

The C library is the product and lives at the root; the bindings are thin
wrappers under `bindings/`.

```
README.md
CMakeLists.txt                libpsmsgr
CMakePresets.json
cmake/                        toolchain file, package config, ABI check
include/psmsgr/psmsgr.h
include/psmsgr/state.h
src/                          implementation (state.c, futex/lock helpers, …)
tools/psmsgr-dump.c
tests/                        C unit + torture tests (CTest)
bench/                        psmsgr-bench: on-target latency benchmark
spec/                         this directory: the contract
bindings/
  python/
    pyproject.toml
    src/ps_msgr/              ctypes binding (src layout: tests run against the installed package)
    tests/                    pytest
  csharp/
    PsMsgr.sln
    PsMsgr/PsMsgr.csproj
    PsMsgr.Tests/PsMsgr.Tests.csproj
    PsMsgr.AotSmoke/          Native AOT smoke test (CI only)
interop/                      cross-language tests (C ⇄ Python ⇄ C#)
examples/                     one small writer/reader pair per language
docker/build.Dockerfile       the build container: CI and local builds
docker/run.sh                 runs a command in the build container
.github/workflows/
```

## Toolchain

All builds, for every target, run in **one build container** based on
`debian:trixie`, defined in `docker/build.Dockerfile`. CI and developers use
the same image.

- **Why trixie:** a binary built against a newer glibc than the target's
  fails to load there. Building on the target's release rules that out.
- **Contents:**
  - `build-essential`, `crossbuild-essential-armhf`
  - `cmake`, `ninja-build`
  - `qemu-user` (runs armhf tests)
  - cmocka 2.0 (C test framework). trixie ships 1.1.7, which predates the
    current API, so the image builds the upstream release (pinned SHA-256)
    as a static library for the host and for armhf. Test binaries therefore
    need no cmocka at run time, on the board either.
  - `clang` (second compiler, sanitizers)
  - `python3`, the .NET SDK, and `mono-runtime` if trixie still ships it.
    Otherwise the Mono smoke test uses the Mono project's packages.
- **Two targets:**
  - host (x86-64 or AArch64, for development and CI)
  - `armhf` (BeagleBone Black), cross-compiled with Debian's
    `arm-linux-gnueabihf-gcc`
- **Code generation:** keep Debian armhf's defaults (ARMv7-A, Thumb-2,
  VFPv3-D16). No `-mcpu`/`-mfpu` flags: the library has no use for NEON.
- **64-bit `time_t`:** trixie armhf defaults to 64-bit `time_t`
  (`_TIME_BITS=64`) and 64-bit `off_t`. The public API contains no
  `time_t`, `off_t` or `struct timespec`, so the ABI doesn't depend on
  either.
- **Running armhf tests:** the toolchain file sets
  `CMAKE_CROSSCOMPILING_EMULATOR` to `qemu-arm -L /usr/arm-linux-gnueabihf`,
  so `ctest` runs armhf tests unchanged. If qemu-user turns out to mishandle
  OFD locks or cross-process futexes, those tests are marked
  `board-only` and run in the on-target validation instead.
- **On-target debugging:** `gdbserver` on the board, `gdb-multiarch` in the
  container.

### CMake presets

`CMakePresets.json` (Ninja generator; builds go to `build/<preset>`):

| Preset | Target | Purpose |
|---|---|---|
| `dev` | host | Debug build with ASan+UBSan: the everyday build. |
| `dev-clang` | host | Same as `dev`, built with clang. |
| `tsan` | host | Debug build with ThreadSanitizer. |
| `release` | host | `RelWithDebInfo`; used by the binding tests and interop. Also builds the host `.deb`. |
| `armhf` | armhf | Debug build; tests run under qemu. |
| `armhf-release` | armhf | Release build plus CPack `.deb` for the board, including `psmsgr-bench`. |

Each preset also has a workflow preset (configure → build → test, plus
package for the release presets), so one command reproduces a CI job:
`cmake --workflow --preset dev`. `docker/run.sh` runs it in the build
container. All presets set `PSMSGR_WERROR=ON`; the option defaults to `OFF`
for people building from source with other compilers.

Packages install into the Debian multiarch directory
(`/usr/lib/<triplet>`) and ship a `shlibs` file, so programs built against
the library get correct package dependencies.

## C library

- CMake ≥ 3.25 (presets v6, workflow presets; trixie ships 3.31), C11, and
  no dependencies beyond glibc. `_GNU_SOURCE` is set internally. The tests
  also need cmocka ≥ 2.0; `-DPSMSGR_BUILD_TESTS=OFF` builds without them.
- `CMAKE_EXPORT_COMPILE_COMMANDS` is on, for clangd.
- Build outputs:
  - `libpsmsgr.so.1` (SONAME) and `libpsmsgr.a`
  - `psmsgr-dump`
  - a CMake package config (`find_package(psmsgr)`) and `psmsgr.pc`
- Flags:
  - Build: `-std=c11 -O2 -fPIC -fvisibility=hidden`.
  - Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`, promoted to
    `-Werror` in CI.
  - Hardening, matching Debian's `dpkg-buildflags`: `-D_FORTIFY_SOURCE=3`,
    `-fstack-protector-strong`, `-fstack-clash-protection` and
    `-Wl,-z,relro,-z,now`.
  - Link with `-Wl,--no-undefined` and a version script that exports only
    `psmsgr_*`.
- **No `libatomic`:** CI fails if `nm -D libpsmsgr.so.1` lists any
  `__atomic_*` symbol, or if `readelf -d` shows `libatomic` in `NEEDED`.
  Either would mean a non-lock-free (e.g. 64-bit) atomic slipped in, which
  on ARMv7 becomes a library call instead of an `ldrex`/`strex` loop.
- Options:

  | Option | Default | Effect |
  |---|---|---|
  | `PSMSGR_BUILD_TESTS` | `ON` | Build the unit and torture tests. |
  | `PSMSGR_BUILD_TOOLS` | `ON` | Build `psmsgr-dump`. |
  | `PSMSGR_BUILD_BENCH` | `OFF` | Build `psmsgr-bench`, installed with the tools. `dev`, `dev-clang` and `armhf-release` turn it on. |
  | `PSMSGR_SANITIZE` | empty | Sanitizer to enable: `address`, `undefined` or `thread`. |

- Packaging: CPack DEB for `armhf` and `amd64`, built in the trixie
  container, produces `libpsmsgr1`, `libpsmsgr-dev` and `psmsgr-tools`.
  Installing the `.deb` is the supported deployment path on the BBB.
  Package dependencies come from `dpkg-shlibdeps` (the container has
  `libc6:armhf` via multiarch for that) and are therefore trixie's.

## Python

`pyproject.toml` with a PEP 517 backend (hatchling or setuptools). Tests use
pytest and need the built C library (`PSMSGR_LIBRARY=<build>/libpsmsgr.so.1`).

## C#

SDK-style projects. The library targets `netstandard2.1`. The test project
must target a concrete runtime: the current .NET LTS, with xUnit. On CI, point
`PSMSGR_LIBRARY` at the built library, which also exercises the `dlopen`
preload path. `bindings/csharp/PsMsgr.AotSmoke/` is a console app used only by the
Native AOT CI job.

## Tests

The C unit tests use cmocka 2's current API: typed assertions
(`assert_int_*` for signed values, `assert_uint_*` for unsigned ones) and
nothing deprecated. Each test program is one CTest test; cmocka's
`CMOCKA_TEST_FILTER` selects tests by name (e.g. `wait_*`). Every test has
a fixture that creates and removes its channel directory. Assertions are
fatal, so a test asserts nothing while a helper thread it started can still
touch its stack. Result codes are checked with `assert_rc`, which prints
both codes with their `psmsgr_strerror` text. In ASan builds CTest sets
`ASAN_OPTIONS=allow_user_segv_handler=0`, so that a crash gets ASan's
report and stack trace instead of cmocka's signal handler.

Every test uses its own temporary directory as the channel `dir`, never
`/dev/shm`, so that tests can run in parallel.

### Unit (C; mirrored where relevant in the bindings)

- Name validation, and the options defaults and limits.
- Create, then reuse, with the generation carried across a writer restart.
- `MISMATCH`, and `RECREATE` → old file `RETIRED`, readers reattach.
- A second writer in the same process and in another process gets
  `WRITER_EXISTS`. The lock is released when the holder is killed with
  `SIGKILL`.
- Crash mid-publish: a slot left with an odd `seq` stays unreadable until
  the next publish rewrites it, and stale `.tmp` files are cleaned up.
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
- Lock identity: `unlink` racing a second writer's open never produces two
  writers (fault-injection hook between `open` and `F_OFD_SETLK`).
- Orphans: `rm` the data file under an attached reader, then start a new
  writer. `writer_alive` and `wait` reattach the reader; `read`/`peek` alone
  keep the old value.
- Format minor mismatch → automatic recreate; `RECREATE` over an invalid
  file; `FORMAT` without it.
- `PSMSGR_INFO_ATTACHED` appears exactly once per attach.
- Symlinked channel or lock files are rejected (`O_NOFOLLOW`), and a missing
  `dir` gives `ENOENT`.
- Reader validation: fuzz the header (random bytes, extreme field values).
  Attach must fail with `FORMAT` and never crash.
- `psmsgr-dump` (`tests/test_dump.c`), run as a child process: usage errors,
  a missing channel, never published, published, `RETIRED`, a corrupt
  header (each §6.1 check, random bytes), a stale `latest` and odd `seq`,
  `--hex` and its cap, and `--watch` with and without `NO_NOTIFY`. Checks
  the exit code and key lines of the output. Under qemu the test runs the
  tool through the same emulator.
- `ENOSPC` from `posix_fallocate` on a size-limited tmpfs, where the
  environment allows mounting one.

### Torture (C)

`tests/test_torture.c`: one writer and N readers (threads *and* processes),
for a fixed duration. Each reader opens its own handle, so every reader has
its own mapping of the file.

- Each payload is `{generation, length, bytes derived from generation, CRC32}`,
  with a random length in `[0, capacity]`. A payload shorter than that
  12-byte frame is only the derived bytes.
- The writer mixes `publish`, `begin`/`commit` and, rarely, `begin` with a
  partial overwrite and `abort`. It starts just below the generation wrap.
- Every successful read MUST validate: the CRC is correct (for a short
  payload, the bytes match the ones derived from `info.generation`), the
  embedded generation equals `info.generation`, and the embedded length
  equals `info.length`.
- Per reader, the sequence of generations read and peeked MUST be
  non-decreasing as a wrapped sequence (`(int32_t)(g - prev) >= 0`), and so
  MUST the timestamps.
- `BUSY` is counted and reported, and must stay rare. It is not a hard
  threshold, because the rate depends on the machine.
- Variants cover `slot_count` 2 and 3, and capacities 16 B, 4 KiB and 1 MiB.
- Reader processes are forked while the test program is single-threaded,
  with `PR_SET_PDEATHSIG`, and report their counts through a pipe.
- `PSMSGR_TORTURE_SECONDS` sets the duration per variant (default 1;
  fractions allowed). `docker/run.sh` passes it through. CTest's 300 s
  timeout still applies, so for long runs on the board, run the binary
  directly. On the board, use `PSMSGR_TORTURE_SECONDS=60`: at 1 s the
  1 MiB variants get only about 15 publishes.
- **Fault injection.** A last test sets the hidden hook
  `psmi_test_skip_seq_recheck` (`src/internal.h`), which makes readers skip
  the second `seq` comparison, and MUST detect torn reads. Without this, a
  passing torture test would prove nothing. The hook is why the test links
  the static library.
- The CTest label is `torture`, not `unit`: `ctest -L torture` runs it, and
  `-LE torture` skips it. It still runs in every preset.

### Interop

Each binding writes and each other binding reads, including a C writer that
recreates the channel under Python and C# readers.

### Sanitizers

- ASan+UBSan on the unit and torture tests.
- TSan on the unit and torture tests. Any report is a bug. TSan cannot see
  the seqlock race itself, because each handle maps the file separately
  (state-channel.md §5.4); the torture test covers that.

## Conventions

- Every source file starts with `SPDX-License-Identifier: Apache-2.0`.
- Formatting and linting are enforced in CI: `.clang-format` for C, `ruff`
  for Python, `.editorconfig` plus `dotnet format` for C#.
- User-visible changes go into `CHANGELOG.md` (Keep a Changelog format).
- C header layouts are pinned with `_Static_assert(offsetof(...))`, matching
  the offset tables in state-channel.md.

## CI matrix

| Job | Purpose |
|---|---|
| x86-64, gcc + clang, ASan/UBSan, TSan | Main correctness gate. All jobs run in the build container. |
| AArch64 native runner (`ubuntu-24.04-arm`), `release` preset | Weakly ordered memory on real hardware: runs the torture test for 10 s per variant and prints its counters from CTest's `LastTest.log`, since CTest shows the output of passing tests only in verbose mode. x86 hides ordering bugs, and qemu-user on an x86 host keeps x86 ordering, so the armhf job cannot catch them. Uses the arm64 build of the same container image. |
| armhf cross build + tests under `qemu-arm` | Target ABI (32-bit atomics, alignment, 64-bit `time_t`) plus the `libatomic` check. |
| Python (x86-64) | Binding tests plus interop. |
| C# (x86-64) | Binding tests plus interop on .NET LTS; a Mono smoke test of the `netstandard2.1` assembly. |
| C# Native AOT | `dotnet publish -p:PublishAot=true` of `PsMsgr.AotSmoke` with `TrimmerSingleWarn=false` (per-warning detail for library code) and IL2xxx/IL3xxx as errors. Publish-time analysis only covers code the app reaches, so the smoke app MUST call every public API, including the generic helpers with a sample struct. Builds for linux-x64 and runs it. |
| CPack | Build the `.deb` for armhf and amd64. |

## On-target validation (before each release)

Run the torture test and `psmsgr-bench` on a real BeagleBone Black, with the
`performance` CPU frequency governor. `bench/README.md` describes the run.
The test fixture creates its channels under `$TMPDIR` (default `/tmp`),
which is not a tmpfs on the BeagleBoard.org image: run the unit tests on
the board with `TMPDIR=/dev/shm`.
Record in the release notes, under "On-target measurements", the
benchmark's CSV output with its header lines. It covers the publish, read
and peek latency for 16 B, 256 B, 4 KiB and 64 KiB payloads, uncontended
and against a writer in another process (with the `BUSY` count), the
wake-up latency of `wait` and of polling a `NO_NOTIFY` channel, and the
cost of `clock_gettime`, both through libc and as a raw syscall. Note
whether it ran under `chrt`. Also commit the CSVs and the torture output to
`bench/results/<date>-bbb-<commit>/`, the baseline the next run compares
against. There are no numeric targets yet; the first measurement is in
`bench/results/2026-09-24-bbb-6511c25/`.

CI builds `psmsgr-bench` (in `dev`, `dev-clang` and `armhf-release`) but
never runs it: numbers from x86 or qemu mean nothing.
