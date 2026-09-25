# Go binding on the BeagleBone Black: 2026-09-25, 803cba5

First run of the Go binding on 32-bit ARM hardware. The C baseline is
[`../2026-09-25-bbb-adea019/`](../2026-09-25-bbb-adea019/), and the other
bindings are in [`../2026-09-24-bbb-bindings-ebc2ba0/`](../2026-09-24-bbb-bindings-ebc2ba0/).

## Summary

| Area | Result |
|---|---|
| Tests, linked dynamically | 28/28 passed, 3 runs, plus 3 runs next to a CPU hog |
| Tests, linked statically (`psmsgr_static`) | 28/28 passed, 3 runs. The executable needs only glibc. |
| Wait, close and cleanup tests, repeated | 30/30 runs, and 10/10 next to a CPU hog |

## Setup

- **Commit:** `803cba5` (origin/main), built in the build container. The
  tests are cross-built with trixie's Go 1.24.4 for `GOARCH=arm GOARM=7`.
  [`build.txt`](build.txt) has the commands and the sha256 of the binaries.
- **Board:** as in the C baseline; see [`board.txt`](board.txt). No Go
  toolchain on the board: only the test executables were copied over.
- **Installed:** `libpsmsgr1`, `libpsmsgr-dev` and `psmsgr-tools` 1.0.0.
  The installed `libpsmsgr.so.1.0.0` is byte-identical to the one in the
  `armhf-release` `.deb`.
- `PSMSGR_BUILD_DIR` pointed at a directory holding the `armhf-release`
  `tests/interop_helper`, which loads the installed library. Channels were
  in `/dev/shm` (`TMPDIR=/dev/shm`). Nothing ran under `chrt`.
- [`scripts/run-board.sh`](scripts/run-board.sh) runs everything. The CPU
  hog is a shell busy loop on the one core.

## Tests

The `-v` output is in `go-test-{1,2,3}.log`, `go-test-static-{1,2,3}.log`
and `go-test-hog-{1,2,3}.log`. No test was skipped: the tests that need a
second process ran with `interop_helper`.

`go-test-wait-x30.log` and `go-test-wait-hog-x10.log` repeat the tests
matching `Wait|Close|Signal|Intr`: `TestWaitWakesOnPublish`,
`TestWaitTimeouts`, `TestWaitContext`, `TestCloseStopsWait`,
`TestWaitFollowsRetire`, `TestWaitFollowsUnlink`,
`TestWaitNotSupportedWithoutNotify`, `TestUseAfterClose` and
`TestCleanupClosesForgottenHandles`.

## Per-call cost

`go test -bench . -benchmem -count 5` ([`go-bench.txt`](go-bench.txt),
[`go-bench-static.txt`](go-bench-static.txt)). Each row is the median of the
5 runs, in µs per call. Every channel notifies. The C column is the median
from the baseline's `bench-batch100.csv`. No call allocates.

| Operation | C | Go | Go, static | Go − C |
|---|--:|--:|--:|--:|
| publish 16 B | 4.54 | 14.98 | 13.86 | +10.4 |
| publish 4096 B | 9.09 | 20.81 | 20.72 | +11.7 |
| read 16 B | 0.46 | 5.11 | 4.86 | +4.6 |
| read 4096 B | 3.47 | 11.10 | 10.74 | +7.6 |
| peek | 0.44 | 4.14 | 3.82 | +3.7 |
| `ReadValue` (24 B struct) | – | 7.61 | 6.33 | |

- Each of these operations is one cgo call. On the Cortex-A8, a cgo call
  costs about 3.5–4.5 µs, about ten times the C `peek`. That is between C#
  (+0.4 µs) and Python (+50 µs).
- A notifying publish costs about 10 µs more than in C, twice the cost of a
  read. This run doesn't show why; a `NO_NOTIFY` benchmark would show
  whether the `FUTEX_WAKE` inside the cgo call causes it.
- Static linking saves 0.1–1.3 µs per call.
- The Go benchmarks time `b.N` calls in a row, not 100-call samples, so
  they give a median over runs and no p99.
