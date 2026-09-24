# psmsgr-bench

Latency of the state channel operations, for the on-target validation
before each release (`spec/build-and-test.md`). Only numbers from a
BeagleBone Black count. CI builds the benchmark but never runs it, and a run
on x86 or under qemu is only a check that it works.

## Getting it onto the board

`armhf-release` builds it (`PSMSGR_BUILD_BENCH=ON`) and ships it in
`psmsgr-tools`:

```sh
docker/run.sh cmake --workflow --preset armhf-release
scp build/armhf-release/libpsmsgr1_*.deb build/armhf-release/psmsgr-tools_*.deb debian@beaglebone:
ssh debian@beaglebone sudo apt install ./libpsmsgr1_*.deb ./psmsgr-tools_*.deb
```

## Running it

```sh
# Fixed CPU frequency: schedutil scales it under load, which shows up as
# bimodal latencies. The BeagleBoard.org trixie image already defaults to
# performance; note the governor first, and restore it afterwards.
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
echo performance | sudo tee /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor

psmsgr-bench | tee bench.txt
psmsgr-bench --csv > bench.csv        # for the release notes
# Per-operation cost without the timer overhead (see below).
psmsgr-bench --csv --batch 100 --iterations 1000 > bench-batch100.csv

# Optional: run as SCHED_FIFO so other tasks cannot preempt the measurement.
# The writer processes inherit the policy.
sudo chrt -f 50 psmsgr-bench --csv > bench-fifo.csv
```

The AM335x has a single core, so `taskset` has nothing to choose from. With
`chrt -f`, record that in the release notes: it changes the wake-up numbers
most. RT throttling (`/proc/sys/kernel/sched_rt_runtime_us`, 950000 by
default) still stops SCHED_FIFO tasks for 50 ms every second, which shows up
as about 50 ms in the `max` column. Ignore `max` in that run, or write -1
there for the run and restore it afterwards (the image has no `sysctl`).

On the BeagleBone Black, `clock_gettime` is a syscall (about 1.3 µs), so the
`timer overhead` row is larger than a `peek` and dominates the short rows.
The `--batch 100` run gives the per-operation figures.

A full run takes a minute or two with the defaults. Channels go into a private
directory under `/dev/shm` (or `--dir`), which is removed at exit.
`psmsgr-bench --help` lists the options.

## What it reports

The header records the kernel, CPU, board, CPU frequency governor and
current frequency, and the clocksource. Then one row per measurement, in
ns per operation: min, median, p99 and max.

| Row | What it measures |
|---|---|
| `timer overhead` | `clock_gettime` around nothing. Included in every uncontended row; subtract it when the operation is short. `--batch N` times N operations per sample instead, which amortizes it but hides the tail. |
| `clock_gettime` | Per call, through libc (the vDSO if the kernel serves it). |
| `clock_gettime syscall` | Per call, forced through the syscall. If this is about the same as `clock_gettime`, the vDSO is not serving `CLOCK_MONOTONIC`, which is the expectation on the Cortex-A8 (no ARM generic timer). Then every publish and every `psmsgr_now_ns()` makes a syscall. |
| `getppid syscall` | Baseline cost of a trivial syscall. |
| `psmsgr_now_ns` | Per call, through the library. |
| `publish`, `publish NO_NOTIFY` | Uncontended, per payload size. The difference is the `FUTEX_WAKE` that a notifying channel makes on every publish. |
| `read`, `peek` | Uncontended, per payload size, reader and writer in one process. |
| `read, writer active` | A reader polling in a tight loop while another process publishes at `--rate`. `busy` counts `PSMSGR_E_BUSY` results. |
| `wait wake-up` | The writer stamps `psmsgr_now_ns()` into the payload just before publishing; the reader blocks in `psmsgr_state_wait`, reads, and subtracts the stamp. Includes the publish, the futex wake, scheduling and the read. |
| `poll wake-up, N us` | The same on a `NO_NOTIFY` channel: the reader peeks every `--poll-us` instead of waiting. |

`missed` counts publishes the reader never saw, which is expected when a
wake-up takes longer than the publish period.

## Where the results go

Paste the CSV, with its `#` header lines, into the release notes under
"On-target measurements", together with the governor and whether `chrt`
was used.

Also commit the CSVs and the torture output to
`bench/results/<date>-bbb-<commit>/` (`bench-fifo.csv` is the `chrt` run),
so the next run has a baseline to compare against.
