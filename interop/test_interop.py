# SPDX-License-Identifier: Apache-2.0
"""C, Python and C# agents on either side of a channel (README.md)."""

from __future__ import annotations

import struct
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

import pytest

from conftest import LANGS, Agent, Spawn

CHAN = "chan"
MOTOR_STATUS_V1 = 0x0001_0001
MOTOR_STATUS_V2 = 0x0001_0002
BASE = 0x0123_4567_89AB_0000  # sequences use all 8 bytes

# PSMSGR_E_*
BUSY, WRITER_EXISTS, MISMATCH, NOTSUP = -6, -9, -10, -12

# Each binding's mapping of an error code: C returns the code, Python raises a subclass
# of PsMsgrError, C# a PsMsgrException that carries the code.
EXCEPTIONS: dict[str, dict[int, str | None]] = {
    "c": dict.fromkeys((WRITER_EXISTS, MISMATCH, NOTSUP)),
    "py": {
        WRITER_EXISTS: "WriterExistsError",
        MISMATCH: "ChannelMismatchError",
        NOTSUP: "PsMsgrError",
    },
    "cs": dict.fromkeys((WRITER_EXISTS, MISMATCH, NOTSUP), "PsMsgrException"),
}

PAIRS = [pytest.param(w, r, id=f"{w}-{r}") for w in LANGS for r in LANGS]


def other(lang: str) -> str:
    return LANGS[(LANGS.index(lang) + 1) % len(LANGS)]


def error(lang: str, code: int) -> dict[str, Any]:
    return {"error": code, "exception": EXCEPTIONS[lang][code]}


def gen_after(gen: int, n: int) -> int:
    for _ in range(n):
        gen = 1 if gen == 0xFFFF_FFFF else gen + 1
    return gen


def motor_status(n: int) -> dict[str, Any]:
    """The value the agents publish for sequence n (README.md)."""
    return {
        "sequence": n,
        "speed_rpm": (n % 65536) / 2,
        "current_a": (n % 1024) / 64,
        "temperature_c": (n % 2048) / 8 - 40,
    }


def payload_hex(n: int) -> str:
    return struct.pack("<Qfff4x", *motor_status(n).values()).hex()


def check_value(result: dict[str, Any], sequence: int | None = None) -> None:
    """A read result that decodes to motor_status(its sequence), bit-exact."""
    assert "value" in result, result
    n = result["value"]["sequence"]
    assert result["value"] == motor_status(n)
    assert (result["length"], result["hex"]) == (24, payload_hex(n))
    if sequence is not None:
        assert n == sequence


def write(
    spawn: Spawn,
    lang: str,
    directory: Path,
    *sequences: int,
    options: tuple[object, ...] = (),
    hold: bool = False,
    name: str = CHAN,
) -> tuple[Agent, list[int]]:
    """Runs a writer agent and returns it and the generation of each publish. Without
    hold, the writer has exited when this returns."""
    w = spawn(lang, "write", directory, name, *options, *(["--hold"] if hold else []), *sequences)
    assert w.next() == {"opened": True}
    generations = []
    for n in sequences:
        result = w.next()
        assert result["sequence"] == n
        generations.append(result["generation"])
    if generations:
        assert generations == [gen_after(generations[0], i) for i in range(len(generations))]
    if not hold:
        assert w.finish() == []
    return w, generations


def run(spawn: Spawn, lang: str, *args: object, returncode: int = 0) -> list[dict[str, Any]]:
    return spawn(lang, *args).finish(returncode)


def test_payload_layout(spawn: Spawn) -> None:
    expected = {
        "sizeof": 24,
        "offsets": {"sequence": 0, "speed_rpm": 8, "current_a": 12, "temperature_c": 16},
        "payload_type": MOTOR_STATUS_V1,
    }
    assert {lang: run(spawn, lang, "payload-layout") for lang in LANGS} == {
        lang: [expected] for lang in LANGS
    }


@pytest.mark.parametrize(("writer", "reader"), PAIRS)
def test_basic(spawn: Spawn, tmp_path: Path, writer: str, reader: str) -> None:
    sequences = [BASE + i for i in range(3)]
    before = time.monotonic_ns()
    _, generations = write(spawn, writer, tmp_path, *sequences)
    [result] = run(spawn, reader, "read", tmp_path, CHAN)
    check_value(result, sequences[-1])
    assert result["generation"] == generations[-1]
    assert (result["payload_type"], result["attached"]) == (MOTOR_STATUS_V1, True)
    assert before <= result["timestamp_ns"] <= time.monotonic_ns()


@pytest.mark.parametrize(("writer", "reader"), PAIRS)
def test_lazy_attach(spawn: Spawn, tmp_path: Path, writer: str, reader: str) -> None:
    r = spawn(reader)
    assert r.ask("read", tmp_path, CHAN) == {"nodata": True}
    assert r.ask("alive", tmp_path, CHAN) == {"alive": False}
    w, [gen] = write(spawn, writer, tmp_path, BASE, hold=True)
    first = r.ask("read", tmp_path, CHAN)
    check_value(first, BASE)
    assert (first["generation"], first["attached"]) == (gen, True)
    assert r.ask("read", tmp_path, CHAN) == {**first, "attached": False}
    assert r.ask("alive", tmp_path, CHAN) == {"alive": True}
    w.finish()
    assert r.ask("read", tmp_path, CHAN) == {**first, "attached": False}


@pytest.mark.parametrize(("writer", "reader"), PAIRS)
def test_wait_notify(spawn: Spawn, tmp_path: Path, writer: str, reader: str) -> None:
    _, [gen] = write(spawn, writer, tmp_path, BASE)
    r = spawn(reader)
    assert r.ask("read", tmp_path, CHAN)["generation"] == gen
    r.send("wait", tmp_path, CHAN, gen, 10_000)
    r.quiet(0.2)  # blocked in wait
    _, [gen2] = write(spawn, writer, tmp_path, BASE + 1)
    assert r.next() == {"changed": True}
    result = r.ask("read", tmp_path, CHAN)
    check_value(result, BASE + 1)
    assert result["generation"] == gen2 == gen_after(gen, 1)
    assert r.ask("wait", tmp_path, CHAN, gen2, 0) == {"changed": False}

    _, [quiet_gen] = write(spawn, writer, tmp_path, BASE, options=("--no-notify",), name="quiet")
    assert r.ask("wait", tmp_path, "quiet", quiet_gen, 1000) == error(reader, NOTSUP)


@pytest.mark.parametrize(("writer", "reader"), PAIRS)
def test_recreate(spawn: Spawn, tmp_path: Path, writer: str, reader: str) -> None:
    _, [gen] = write(spawn, writer, tmp_path, BASE)
    r = spawn(reader)
    first = r.ask("read", tmp_path, CHAN)
    check_value(first, BASE)
    assert (first["attached"], first["payload_type"]) == (True, MOTOR_STATUS_V1)
    options = ("--capacity", 32, "--payload-type", MOTOR_STATUS_V2, "--recreate")
    w, [gen2] = write(spawn, writer, tmp_path, BASE + 1, options=options, hold=True)
    assert gen2 == gen_after(gen, 1)
    result = r.ask("read", tmp_path, CHAN)
    check_value(result, BASE + 1)
    assert (result["generation"], result["attached"]) == (gen2, True)
    assert result["payload_type"] == MOTOR_STATUS_V2
    assert r.ask("alive", tmp_path, CHAN) == {"alive": True}
    w.finish()


@pytest.mark.parametrize(("writer", "reader"), PAIRS)
def test_writer_death(spawn: Spawn, tmp_path: Path, writer: str, reader: str) -> None:
    w, generations = write(spawn, writer, tmp_path, BASE, BASE + 1, hold=True)
    r = spawn(reader)
    last = r.ask("read", tmp_path, CHAN)
    check_value(last, BASE + 1)
    assert last["generation"] == generations[-1]
    assert r.ask("alive", tmp_path, CHAN) == {"alive": True}
    w.kill()  # SIGKILL
    assert r.ask("alive", tmp_path, CHAN) == {"alive": False}
    assert r.ask("read", tmp_path, CHAN) == {**last, "attached": False}

    # Another language takes over the same file: the generation continues.
    w2, [gen] = write(spawn, other(writer), tmp_path, BASE + 2, hold=True)
    assert gen == gen_after(generations[-1], 1)
    assert r.ask("alive", tmp_path, CHAN) == {"alive": True}
    result = r.ask("read", tmp_path, CHAN)
    check_value(result, BASE + 2)
    assert (result["generation"], result["attached"]) == (gen, False)
    w2.finish()


def check_stream(results: list[dict[str, Any]], published: dict[int, int]) -> tuple[int, int]:
    """Checks one reader's results against the writer's (generation -> sequence) and
    returns the number of values and BUSY results."""
    values = busy = 0
    previous = None
    for result in results:
        if result.get("error") == BUSY:
            busy += 1
            continue
        if result == {"nodata": True}:
            assert previous is None
            continue
        check_value(result)
        gen = result["generation"]
        assert published.get(gen) == result["value"]["sequence"], result
        assert result["payload_type"] == MOTOR_STATUS_V1
        if previous is not None:
            assert (gen - previous) % 2**32 < 2**31, (previous, gen)  # non-decreasing
        previous = gen
        values += 1
    return values, busy


@pytest.mark.parametrize("writer", LANGS)
def test_continuous(spawn: Spawn, tmp_path: Path, writer: str) -> None:
    """A writer at 1 kHz for 2 s, and a polling and a waiting reader per language."""
    rate, count = 1000, 2000
    pollers = {lang: spawn(lang, "read", tmp_path, CHAN, "--for", 2500) for lang in LANGS}
    waiters = {lang: spawn(lang) for lang in LANGS}
    final: list[int] = []
    done = threading.Event()

    def follow(r: Agent) -> list[dict[str, Any]]:
        """Waits for and reads each change until the writer's last one (or the test
        failed: final stays empty)."""
        results: list[dict[str, Any]] = []
        last = 0
        while not done.is_set() or final[:1] not in ([], [last]):
            changed = r.ask("wait", tmp_path, CHAN, last, 100)
            assert changed in ({"changed": True}, {"changed": False}), changed
            if changed["changed"]:
                results.append(r.ask("read", tmp_path, CHAN))
                last = results[-1].get("generation", last)
        return results

    with ThreadPoolExecutor(len(waiters)) as pool:
        followed = {lang: pool.submit(follow, r) for lang, r in waiters.items()}
        try:
            sequences = [BASE + i for i in range(count)]
            w = spawn(writer, "write", tmp_path, CHAN, "--rate", rate, *sequences)
            assert w.next() == {"opened": True}
            published = {}
            for n in sequences:
                result = w.next()
                assert result["sequence"] == n
                published[result["generation"]] = n
            assert w.finish() == []
            final.append(result["generation"])
        finally:
            done.set()
        stream = {lang: future.result() for lang, future in followed.items()}

    for lang, r in pollers.items():
        *results, summary = r.finish()
        assert summary.keys() == {"reads", "busy"}
        values, _ = check_stream(results, published)
        assert values > 0, f"{lang} poller"
        print(f"{lang} poller: {summary['reads']} reads, {values} values, {summary['busy']} BUSY")
    for lang, results in stream.items():
        values, busy = check_stream(results, published)
        assert results[-1]["generation"] == final[0]
        print(f"{lang} waiter: {values} values, {busy} BUSY")


@pytest.mark.parametrize("lang", LANGS)
def test_errors(spawn: Spawn, tmp_path: Path, lang: str) -> None:
    w, _ = write(spawn, other(lang), tmp_path, hold=True)
    assert run(spawn, lang, "write", tmp_path, CHAN, returncode=1) == [error(lang, WRITER_EXISTS)]
    w.finish()
    assert run(spawn, lang, "write", tmp_path, CHAN, "--capacity", 32, returncode=1) == [
        error(lang, MISMATCH)
    ]
