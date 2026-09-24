"""Where a Python peek()/publish() call spends its time: the raw ctypes calls
and the Python-side pieces, timed separately (ns per call, median of 300 x 100)."""
import tempfile
import time

from ps_msgr import StateReader, StateWriter, _native
from ps_msgr._state import StateInfo


def t(fn, n=100, it=300):
    v = []
    for _ in range(it):
        t0 = time.perf_counter_ns()
        for _ in range(n):
            fn()
        v.append((time.perf_counter_ns() - t0) / n)
    return sorted(v)[it // 2]


d = tempfile.mkdtemp(dir="/dev/shm")
with StateWriter("c", 16, directory=d) as w, StateReader("c", directory=d) as r:
    p = bytes(16)
    w.publish(p)
    r.peek()
    h, ref, info = r._h, r._info_ref, r._info
    rows = [
        ("peek()", r.peek),
        ("_native.state_peek(h, ref) alone", lambda: _native.state_peek(h, ref)),
        ("4 ctypes Structure field reads", lambda: (info.generation, info.length, info.timestamp_ns, info.flags)),
        ("StateInfo(...) frozen slots dataclass", lambda: StateInfo(1, 2, 3, False)),
        ("publish()", lambda: w.publish(p)),
        ("_native.state_publish(...) alone", lambda: _native.state_publish(w._h, p, 16, w._gen_ref)),
    ]
    for name, fn in rows:
        print(f"{name}: {t(fn):.0f} ns")
