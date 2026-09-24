"""Replays test_continuous with a timeline: when each poller's read --for 2500
window starts and ends, and when the writer opens the channel, publishes its
first and last value and exits. Run from interop/ with the suite's environment.

  diag_continuous.py WRITER_LANG
"""
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, ".")
from conftest import LANGS, Agent  # noqa: E402

writer_lang = sys.argv[1]
d = Path(tempfile.mkdtemp(dir="/dev/shm"))
t0 = time.monotonic()


def ts():
    return f"{(time.monotonic() - t0) * 1e3:7.0f} ms"


pollers = {lang: Agent(lang, "read", d, "chan", "--for", 2500) for lang in LANGS}
print(f"{ts()}  pollers spawned")
waiters = {lang: Agent(lang) for lang in LANGS}
print(f"{ts()}  waiter sessions spawned")
ends = {}


def poller_end(lang, a):
    lines = []
    while True:
        try:
            lines.append(a.next(timeout=60))
        except AssertionError:
            break
    ends[lang] = (ts(), sum(1 for r in lines if "generation" in r), lines[-1] if lines else None)


threads = [threading.Thread(target=poller_end, args=(lang, a)) for lang, a in pollers.items()]
for t in threads:
    t.start()
seqs = [0x0123_4567_89AB_0000 + i for i in range(2000)]
w = Agent(writer_lang, "write", d, "chan", "--rate", 1000, *seqs)
print(f"{ts()}  {writer_lang} writer spawned")
assert w.next(timeout=60) == {"opened": True}
print(f"{ts()}  writer opened")
w.next(timeout=60)
print(f"{ts()}  first publish")
for _ in seqs[1:]:
    w.next(timeout=60)
print(f"{ts()}  last publish (writer's 2000 values at 1 kHz should take 2000 ms)")
w.proc.stdin.close()
w.proc.wait()
for t in threads:
    t.join()
for lang, (end, n, last) in ends.items():
    print(f"{end}  {lang} poller ended: {n} values, summary {last}")
for a in waiters.values():
    a.proc.stdin.close()
    a.proc.wait()
