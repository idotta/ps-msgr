#!/usr/bin/env python3
"""Per-call cost of the Python binding, in the psmsgr-bench CSV format.

Uncontended rows time --batch calls per sample and report ns per call (like
psmsgr-bench --batch). The wake-up row mirrors psmsgr-bench: a writer in
another process publishes a 16 B value stamped with now_ns() at --rate Hz; the
reader blocks in wait(), reads, and subtracts the stamp.

  percall.py [--iterations N] [--batch B] [--seconds S] [--rate HZ]
"""

import argparse
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import ps_msgr
from ps_msgr import StateReader, StateWriter

SIZES = (16, 256, 4096)


def row(test, size, samples, batch, missed=""):
    v = sorted(s / batch for s in samples)
    n = len(v)
    p99 = v[min(n - 1, (99 * n + 99) // 100 - 1)]
    print(f'"{test}",{size},{n},{v[0]:.0f},{v[n // 2]:.0f},{p99:.0f},{v[-1]:.0f},{missed}', flush=True)


def timed(fn, iterations, batch, warmup=1000):
    clock = time.perf_counter_ns
    for _ in range(warmup):
        fn()
    out = []
    for _ in range(iterations):
        t0 = clock()
        for _ in range(batch):
            fn()
        out.append(clock() - t0)
    return out


def writer_proc(d, name, rate, seconds):
    with StateWriter(name, 16, directory=d) as w:
        buf = bytearray(16)
        struct.pack_into("<Q", buf, 0, ps_msgr.now_ns())
        w.publish(buf)
        sys.stdout.write("ready\n")
        sys.stdout.flush()
        period = 1.0 / rate
        t0 = time.monotonic()
        i = 1
        while True:
            t = t0 + i * period
            if t - t0 > seconds:
                break
            delay = t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            struct.pack_into("<Q", buf, 0, ps_msgr.now_ns())
            w.publish(buf)
            i += 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=1000)
    ap.add_argument("--batch", type=int, default=100)
    ap.add_argument("--seconds", type=float, default=5.0)
    ap.add_argument("--rate", type=float, default=500.0)
    ap.add_argument("--writer", nargs=2, help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.writer:
        writer_proc(a.writer[0], a.writer[1], a.rate, a.seconds)
        return

    d = tempfile.mkdtemp(prefix="psmsgr-pybench-", dir="/dev/shm")
    try:
        print(f"# python: {sys.version.split()[0]}")
        print(f"# settings: iterations {a.iterations}, warm-up 1000, batch {a.batch}; wake-up {a.seconds} s at {a.rate:g} Hz")
        print("test,size_bytes,n,min_ns,median_ns,p99_ns,max_ns,missed")
        row("timer overhead", "", timed(lambda: None, a.iterations, 1, 0), 1)
        libc = ctypes.CDLL(None)
        noop = lambda: None  # noqa: E731
        row("python call (empty lambda)", "", timed(noop, a.iterations, a.batch), a.batch)
        row("os.getppid", "", timed(os.getppid, a.iterations, a.batch), a.batch)
        row("ctypes getppid", "", timed(libc.getppid, a.iterations, a.batch), a.batch)
        row("now_ns", "", timed(ps_msgr.now_ns, a.iterations, a.batch), a.batch)
        for size in SIZES:
            payload = bytes(range(256)) * (size // 256) if size >= 256 else bytes(size)
            with StateWriter(f"nn{size}", size, notify=False, directory=d) as w:
                row("publish NO_NOTIFY", size, timed(lambda: w.publish(payload), a.iterations, a.batch), a.batch)
            with StateWriter(f"p{size}", size, directory=d) as w, StateReader(f"p{size}", directory=d) as r:
                row("publish", size, timed(lambda: w.publish(payload), a.iterations, a.batch), a.batch)
                buf = bytearray(size)
                mv = memoryview(buf)
                row("read", size, timed(r.read, a.iterations, a.batch), a.batch)
                row("read_into", size, timed(lambda: r.read_into(mv), a.iterations, a.batch), a.batch)
                row("peek", size, timed(r.peek, a.iterations, a.batch), a.batch)

        p = subprocess.Popen(
            [sys.executable, __file__, "--writer", d, "wake", "--rate", str(a.rate), "--seconds", str(a.seconds)],
            stdout=subprocess.PIPE,
            text=True,
        )
        assert p.stdout.readline() == "ready\n"
        samples, missed = [], 0
        buf = bytearray(16)
        with StateReader("wake", directory=d) as r:
            info = r.read_into(buf)
            seen = info.generation
            end = time.monotonic() + a.seconds
            while time.monotonic() < end:
                if not r.wait(seen, 0.1):
                    continue
                try:
                    info = r.read_into(buf)
                except ps_msgr.ChannelBusyError:
                    continue
                t = ps_msgr.now_ns()
                samples.append(t - struct.unpack_from("<Q", buf)[0])
                missed += (info.generation - seen - 1) & 0xFFFFFFFF
                seen = info.generation
        p.wait()
        row("wait wake-up", 16, samples, 1, missed)
    finally:
        shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    main()
