# Bindings on the BeagleBone Black: 2026-09-24, ebc2ba0

This is the first run of the Python binding, the C# binding (Native AOT), the
interop suite and the examples on 32-bit ARM hardware. The C library's own
on-target baseline is in [`../2026-09-24-bbb-6511c25/`](../2026-09-24-bbb-6511c25/).
The per-call tables below compare against that run.

## Summary

| Area | Result |
|---|---|
| Python binding, pytest | 35/35 passed, 3 runs. The wait and signal tests passed 30 more runs, and the whole suite passed 10 more runs next to a CPU hog. |
| C# binding, AOT smoke app | `ok`, both variants: dynamic `libpsmsgr.so.1`, and `libpsmsgr.a` linked in with DirectPInvoke. 20/20 runs next to a CPU hog. |
| C# binding, xUnit (JIT) | 30/30 passed, 3 runs, plus 3 runs next to a CPU hog |
| Interop suite | 51/52 passed, 3 runs. **`test_continuous[py]` fails every time.** This is a timing assumption in the test, not a data error (see below). |
| Examples | All 9 writer × reader pairs, the standalone C build, `writer gone`, `stale` and the payload-type mismatch pass, 3 runs. Ctrl-C exits all 6 programs with status 0. |
| Native AOT on linux-arm | **Works.** Cross-published in the build container with no setup beyond `-p:ObjCopyName=arm-linux-gnueabihf-objcopy`, with 0 warnings. Every AOT binary ran on the board. |

## Setup

- **Commit:** `ebc2ba0` (origin/main), built in the build container.
  [`build.txt`](build.txt) has the commands, tool versions and the sha256 of
  the packages and the wheel.
- **Board:** TI AM335x BeagleBone Black, Cortex-A8 at 1000 MHz
  (`performance` governor), 1 CPU, 481 MiB RAM. Kernel
  `6.18.39-bone44-preditor.2` (PREEMPT), Debian 13.5, BeagleBoard.org
  trixie image 2026-05-19. Clocksource `dmtimer`, cpuidle `mpu_gate`
  enabled (as in the C baseline). Python 3.13.5, glibc 2.41. No .NET runtime
  and no libicu. See [`board.txt`](board.txt).
- **Installed:** `libpsmsgr1`, `libpsmsgr-dev` and `psmsgr-tools` 1.0.0
  from `armhf-release` with apt. The installed `libpsmsgr.so.1.0.0` is
  byte-identical to the one in the `.deb`.
  `ps_msgr-0.1.0-py3-none-any.whl` and pytest 8.3.5 went into a venv. The
  board has no internet access, so the pytest wheels were copied over.
- **Test library path:** the binding tests take `tests/interop_helper` from
  the directory of `$PSMSGR_LIBRARY`. On the board, `PSMSGR_LIBRARY` pointed
  at a copy of the installed library placed next to the `armhf-release`
  `interop_helper`. The interop suite and the examples used the installed
  library directly (`PSMSGR_LIBRARY` unset).
- Channels were in `/dev/shm` and `TMPDIR=/dev/shm`. Nothing ran under
  `chrt`.

## Native AOT on linux-arm

`scripts/publish.sh` runs `dotnet publish -c Release -r linux-arm
-p:ObjCopyName=arm-linux-gnueabihf-objcopy` in the container (.NET SDK
10.0.401, ILCompiler 10.0.12). ILCompiler links with `clang
--target=arm-linux-gnueabihf`. Without a `SysRoot`, clang finds the Debian
cross toolchain that the container already has. No sysroot or other setup
was needed.

| Binary | Size | Needs |
|---|--:|---|
| `PsMsgr.AotSmoke` | 1 387 588 B | glibc, `libpsmsgr.so.1` (loaded at the first call) |
| `PsMsgr.AotSmoke.Static` (DirectPInvoke + `libpsmsgr.a`) | 1 387 276 B | glibc only: `LD_DEBUG=files` shows no `libpsmsgr.so.1` load |
| `PsMsgr.InteropAgent` | 1 386 744 B | glibc, `libpsmsgr.so.1` |
| `MotorWriter` / `MotorReader` | 1 255 776 / 1 189 616 B | glibc, `libpsmsgr.so.1` |

Wall time and RSS come from `scripts/runstat.c`, which forks, execs and
reads `wait4()`. Medians of 10–20 runs:

| Program | Wall | Max RSS |
|---|--:|--:|
| `PsMsgr.AotSmoke` (whole run, every public API) | 130 ms | 2.9 MiB |
| `PsMsgr.AotSmoke.Static` | 128 ms | 2.9 MiB |
| C# interop agent, `payload-layout` (startup) | 20 ms | 2.5 MiB |
| C interop agent, `payload-layout` | 7 ms | 1.0 MiB |
| Python interop agent, `payload-layout` | 714 ms | 9.7 MiB |
| `python3 -c "import ps_msgr"` (venv) | 657 ms | 9.3 MiB |
| `/bin/true` (floor) | 6 ms | 0.9 MiB |

Logs: [`csharp-aot-smoke.log`](csharp-aot-smoke.log),
[`csharp-stress.log`](csharp-stress.log).

## Python binding

[`python-pytest.log`](python-pytest.log) has the full `-v` output of 3 runs.
Each run took 3.4 s inside pytest and 6.1 s wall.
[`python-pytest-stress.log`](python-pytest-stress.log) has the repeated
runs.

| Test | Result |
|---|---|
| `test_native.py`: `test_struct_layout` ×3, `test_constants`, `test_options_init_sets_struct_size`, `test_version_check`, `test_import_fails_clearly_without_library` | pass |
| `test_state.py`: `create_then_reuse`, `directory_from_env`, `mismatch_and_recreate`, `second_writer_same_process`, `second_writer_other_process`, `lazy_reader_attaches_later`, `read_results_and_sizes`, `publish_buffers`, `heartbeat_channel`, `receive_buffer_follows_capacity`, `peek_timestamps`, `writer_alive`, `attached_flag_once_per_attach`, `unlink`, `invalid_arguments`, `system_errors`, `format_error`, `errors`, `use_after_close`, `unclosed_handles_warn` | pass |
| `test_state.py` wait and signals: `wait_wakes_on_publish`, `wait_timeouts`, `wait_follows_retire`, `wait_follows_unlink`, `wait_not_supported_without_notify`, `wait_raises_keyboard_interrupt`, `wait_sigint_to_main_thread`, `wait_resumes_after_signal_handler` | pass. Also 30/30 repeats of `-k "wait or interrupt or signal"`, and 10/10 full runs next to `yes > /dev/null` on the one core. |

Nothing was skipped. The slowest test was
`test_import_fails_clearly_without_library` at 0.57 s, which starts a
Python subprocess.

## C# binding

The xUnit suite ran as a self-contained JIT build for `linux-arm`, so
nothing had to be installed on the board. It needs
`DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1` because the image has no libicu.
Without it, the runner aborts at startup.

| Suite | Result |
|---|---|
| `StateTests` (26 tests, including `WaitTimeouts`, `WaitResumesAfterSignals`, `WaitCancellation`, `WaitFollowsRetire`/`Unlink`, `SecondWriterOtherProcess`) | 26/26 pass |
| `NativeTests` (`VersionCheck`, `OptionsInitSetsStructSize`, `Constants`, `StructLayouts`) | 4/4 pass |
| Totals | 30/30, 3 runs, 11.5 s in the runner and 17.8 s wall, 50 MiB RSS. With a CPU hog: 30/30, 3 runs, 19.9 s. |

Log: [`csharp-xunit.log`](csharp-xunit.log).

## Per-call cost

`scripts/percall.py` (Python) and `scripts/cs-percall/` (C#, Native AOT)
mirror `psmsgr-bench --batch 100`. They time 100 calls per sample, over
1000 samples after 1000 warm-up calls, and report ns per call. The C column
is `bench-batch100.csv` from the C baseline. Wake-up rows mirror the
benchmark's `wait wake-up`: a writer process in the same language publishes
a 16 B value stamped with `now_ns` at 500 Hz for 5 s, and the reader waits,
reads and subtracts the stamp. The C wake-up is from the baseline's
`bench.csv`. Every channel notifies unless marked NO_NOTIFY.

Median / p99 in µs. Python "read, into caller's buffer" is
`read_into(memoryview)`, and "returning a copy" is `read()`. C# uses
`TryRead(Span)` and `Read(out info)`, and `TryPeek`.

| Operation | C | Python | C# AOT | C# AOT, static | Python − C | C# − C |
|---|--:|--:|--:|--:|--:|--:|
| now_ns | 1.42 / 1.72 | 8.63 / 9.14 | 1.58 / 1.84 | 1.54 / 1.83 | +7.2 | +0.16 |
| publish 16 B | 4.49 / 4.84 | 45.6 / 46.9 | 5.52 / 6.41 | 6.26 / 6.59 | +41.1 | +1.03 |
| publish NO_NOTIFY 16 B | 1.70 / 39.9 | 39.1 / 40.5 | 2.01 / 2.30 | 1.96 / 2.28 | +37.4 | +0.30 |
| read, into caller's buffer 16 B | 0.46 / 0.66 | 81.8 / 97.5 | 0.87 / 1.10 | 0.86 / 1.09 | +81.3 | +0.41 |
| read, returning a copy 16 B | – | 72.9 / 102 | 1.06 / 2.02 | 1.05 / 2.10 | | |
| peek 16 B | 0.42 / 0.61 | 51.5 / 53.5 | 0.86 / 1.08 | 0.88 / 1.09 | +51.0 | +0.44 |
| publish 256 B | 4.75 / 5.08 | 46.4 / 48.5 | 5.59 / 6.47 | 6.15 / 6.47 | +41.6 | +0.85 |
| publish NO_NOTIFY 256 B | 1.88 / 2.13 | 39.7 / 41.6 | 2.10 / 2.36 | 2.02 / 2.25 | +37.9 | +0.23 |
| read, into caller's buffer 256 B | 0.59 / 0.80 | 82.5 / 90.0 | 1.09 / 1.33 | 1.10 / 1.32 | +82.0 | +0.49 |
| read, returning a copy 256 B | – | 74.4 / 76.4 | 1.69 / 4.97 | 1.60 / 4.99 | | |
| peek 256 B | 0.41 / 0.61 | 52.3 / 54.0 | 0.86 / 1.09 | 0.91 / 1.15 | +51.9 | +0.45 |
| publish 4096 B | 9.02 / 13.9 | 50.6 / 53.3 | 8.58 / 9.47 | 8.54 / 9.42 | +41.5 | −0.44 |
| publish NO_NOTIFY 4096 B | 5.71 / 43.0 | 44.1 / 45.9 | 4.79 / 5.05 | 4.40 / 7.43 | +38.4 | −0.93 |
| read, into caller's buffer 4096 B | 4.61 / 6.06 | 87.6 / 96.6 | 3.52 / 3.83 | 4.46 / 4.80 | +83.0 | −1.08 |
| read, returning a copy 4096 B | – | 85.8 / 88.2 | 10.4 / 15.2 | 10.3 / 15.0 | | |
| peek 4096 B | 0.43 / 0.62 | 52.0 / 53.4 | 0.88 / 1.11 | 0.91 / 1.14 | +51.5 | +0.45 |
| wait wake-up 16 B | 147 / 158 | 648 / 662 | 188 / 254 | 190 / 329 | +501 | +41 |

- **C#:** the binding adds about 0.2–0.5 µs to a read, peek or `now_ns` and
  about 1 µs to a notifying publish. The static link makes no measurable
  difference. At 4 KiB, the difference is within the noise between runs:
  the copy dominates, and the C run's own 4 KiB rows are bimodal (read
  minimum 3.4 µs, median 4.6 µs). `Read()` allocates a new `byte[]` on
  every call, which costs 6.9 µs more than `TryRead(Span)` at 4 KiB. The
  wake-up is 41 µs slower than C in the median.
- **Python:** every call costs 40–85 µs on the Cortex-A8, which is 10× the
  C cost for a publish and 100–180× for a read or peek. A second run
  matched within 2% ([`python-percall-run2.csv`](python-percall-run2.csv)).
  [`python-breakdown.txt`](python-breakdown.txt) shows where the time goes:
  - The bare ctypes call `state_peek(h, byref)` costs 21 µs. Of that,
    conversion through the `argtypes` costs about 15 µs: a ctypes
    `getppid()` with no arguments costs 6.1 µs, and the syscall itself
    0.66 µs.
  - Building the frozen `StateInfo` dataclass costs 15 µs.
  - Reading 4 fields of the ctypes `Structure` costs 7 µs.
  - `read_into()` is slower than `read()`, because it makes a `memoryview`
    and a `c_char.from_buffer` on every call.

  The wake-up median is 648 µs, which is 0.5 ms above C: the Python writer
  and the Python reader each add their own call cost. This limits Python to
  a few kHz of calls on this board, which is fine for the 10 Hz examples.
  It also explains the interop failure below.

CSVs: [`python-percall.csv`](python-percall.csv),
[`csharp-percall.csv`](csharp-percall.csv) (and `-run2`),
[`csharp-percall-static.csv`](csharp-percall-static.csv).

## Interop suite

Ran natively with all three agents: the C `interop_helper` from
`armhf-release`, `agent_py.py` on the venv, and the C# agent Native AOT.
Nothing was skipped: at 481 MiB RAM, the board carries it. The suite has no
`PSMSGR_INTEROP_SECONDS`: `test_continuous` always publishes 2000 values at
1 kHz. One run takes 65 s. Log: [`interop-pytest.log`](interop-pytest.log).

| Scenario | c→c | c→py | c→cs | py→c | py→py | py→cs | cs→c | cs→py | cs→cs |
|---|---|---|---|---|---|---|---|---|---|
| basic, lazy attach, wait/notify, recreate, writer death | pass | pass | pass | pass | pass | pass | pass | pass | pass |

| Per language | c | py | cs |
|---|---|---|---|
| `test_continuous` (writer) | pass | **fail** (3/3 runs) | pass |
| `test_errors` | pass | pass | pass |

`test_payload_layout` passes.

**`test_continuous[py]`** fails with `AssertionError: c poller`, `assert 0 >
0`. [`interop-continuous-diag.log`](interop-continuous-diag.log) replays
the scenario with timestamps (`scripts/diag_continuous.py`):

```
     40 ms  pollers spawned                 (each: read --for 2500)
    264 ms  py writer spawned
   2536 ms  c poller ended: 0 values
   2644 ms  cs poller ended: 0 values
   3217 ms  writer opened
   7028 ms  last publish                    (2000 values at "1 kHz": 3.8 s)
```

The pollers' 2.5 s window starts when they are spawned. Three Python
interpreters (the py poller, the py waiter and the py writer) start
together on the one core, and each needs about 0.7 s alone. So the Python
writer opens the channel after the C and C# pollers have already stopped,
and they record no values. The Python writer also cannot keep up 1 kHz next
to six readers.

Every value that was read decoded correctly and matched its generation. The
waiting readers all ended on the last generation. With the C or C# writer,
the py poller starts late too and sees only 2 values. That scenario
passes, but covers little on this board.

## Examples

[`examples.log`](examples.log): `scripts/examples-board.sh` is the run
section of `examples/check.sh`, pointed at the board's programs. It uses
the C examples from `armhf-release`, and builds the standalone C examples
on the board against `libpsmsgr-dev`. The image has neither cmake nor
pkg-config, so the build is `gcc … -lpsmsgr`. It also uses the Python
examples on the wheel and the C# examples in Native AOT.

| Writer ↓ / Reader → | C | Python | C# |
|---|---|---|---|
| C | pass | pass | pass |
| Python | pass | pass | pass |
| C# | pass | pass | pass |

Also passed: standalone → standalone, `writer gone`, `stale`, and the
payload-type mismatch (every reader refuses `0x00020001`, and a Python
writer replaces the channel for a C# reader). The whole script took 23 s,
and 3 runs passed.

[`examples-sigint.log`](examples-sigint.log): each reader attaches to a
writer in another language, reports `writer gone`, and exits 0 on SIGINT.
Each writer runs at 10 Hz and exits 0 on SIGINT after 2 s. Within those
2 s, the C writer printed 21 values, C# 20 and Python 14 (its 0.7 s
startup).

## Differences from CI

1. `test_continuous[py]` fails on the board. In CI, interpreter startup is
   short compared to the fixed 2.5 s window.
2. Python calls are 10–180× the C cost. In CI they are small next to the
   test timeouts. Nothing else is timing-sensitive on one core: the wait,
   KeyboardInterrupt and signal tests passed every run, including under
   load.
3. With DirectPInvoke, `PSMSGR_LIBRARY` still matters. The binding's static
   constructor `dlopen`s it:
   - A bad path fails the first call with `TypeInitializationException` /
     `DllNotFoundException`, exit 134. This was shown on the board and is
     not ARM-specific.
   - A valid path loads a second copy that nothing uses.

   `bindings/csharp/README.md` says it "has no effect then".
4. A JIT .NET app on the stock image needs invariant globalization (no
   libicu). The AOT projects already set `InvariantGlobalization`, but the
   test project does not.
5. A background job started by a non-interactive `sh` inherits SIGINT as
   ignored, and the Python examples keep ignoring it. The C and C#
   examples install their own handler. From a terminal, all six programs
   behave the same.
6. The binding tests resolve `$PSMSGR_LIBRARY` through symlinks before
   they look for `tests/interop_helper`. A symlink to the installed library
   therefore fails 4 layout tests with "interop_helper is missing".
   Copying the library works.
7. The container image built before .NET was added had no `dotnet`.
   `docker/run.sh` does not rebuild on Dockerfile changes:
   `PSMSGR_REBUILD_IMAGE=1` was needed.

## Suggested doc and spec changes (text only)

- **`interop/README.md`, scenario 6:** state that the pollers' 2.5 s window
  starts at spawn. The test (a code change, not made here) should start
  the window once the writer has opened the channel, or wait for the first
  value, so that a slow writer start does not empty it. Until then, add:
  "On the BeagleBone Black, `test_continuous[py]` fails: Python startup
  (0.7 s per interpreter) delays the writer past the window. See
  `bench/results/2026-09-24-bbb-bindings-ebc2ba0/`."
- **`bindings/csharp/README.md`, Native AOT:** replace "`PSMSGR_LIBRARY` has
  no effect then" with "Leave `PSMSGR_LIBRARY` unset then: the binding
  still loads it, and fails if it cannot, although the calls go to the
  linked copy." Also: "Runs on the BeagleBone Black: 1.4 MB executable,
  2.9 MiB RSS, about 0.2–0.5 µs over C per read or peek, 1 µs per
  publish."
- **`bindings/python/README.md`:** add a performance note: "On a
  BeagleBone Black each call costs 40–85 µs (publish 46 µs, peek 51 µs,
  read 73–82 µs) and the wait wake-up is about 0.65 ms, against 0.4–5 µs
  and 0.15 ms from C. `import ps_msgr` takes about 0.65 s. Use C or C# for
  rates above a few hundred Hz."
- **`examples/README.md`, "On the BeagleBone Black":** replace "None of the
  examples has run on the board yet" with a pointer to this run. Note that
  the stock image has no cmake or pkg-config: `gcc motor_writer.c
  -I.. -lpsmsgr` builds against `libpsmsgr-dev`, or install cmake first.
- **`spec/build-and-test.md`, "On-target validation":** add the bindings.
  Before each release, run on the board the Python tests, the C# xUnit
  suite (self-contained JIT, with `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1`),
  the AOT smoke app (both link variants), the interop suite and the
  examples, and commit the logs to `bench/results/<date>-bbb-bindings-<commit>/`.
  Also state that the binding tests need `PSMSGR_LIBRARY` to be a file
  (not a symlink) next to `tests/interop_helper`.
- **`spec/bindings.md`:** record that DirectPInvoke does not bypass the
  `PSMSGR_LIBRARY` preload. Either document that, or specify that the
  preload is skipped when the library is linked statically.

## Scripts

[`scripts/`](scripts/) holds what produced these files. CI does not build
or run any of it. They ran from `build/bbb/`, so the relative paths in
the `.csproj` files and in `publish.sh` assume that location.

- `publish.sh`: the linux-arm publishes and the wheel, in the container.
- `percall.py`, `cs-percall/` and `cs-percall-static/`: the per-call timing.
- `peek_breakdown.py`: where a Python call spends its time.
- `aot-static/`: the smoke app with `libpsmsgr.a` linked in.
- `runstat.c`: wall time and RSS.
- `diag_continuous.py`: the interop timeline.
- `examples-board.sh` and `examples-sigint.sh`: the examples runs.
