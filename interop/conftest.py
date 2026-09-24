# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import contextlib
import json
import os
import queue
import subprocess
import sys
import threading
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import Any

import pytest

HERE = Path(__file__).resolve().parent
LANGS = ("c", "py", "cs")
TIMEOUT = 10.0  # seconds for any one result


def agent_command(lang: str) -> list[str]:
    if lang == "py":
        return [sys.executable, str(HERE / "agent_py.py")]
    var = {"c": "PSMSGR_INTEROP_C", "cs": "PSMSGR_INTEROP_CS"}[lang]
    path = os.environ.get(var)
    if not path:
        pytest.fail(f"{var} is not set: run interop/check.sh")
    return [path]


class Agent:
    """An agent process. A thread collects its stdout lines, so that a slow test
    never blocks it on a full pipe."""

    def __init__(self, lang: str, *args: object) -> None:
        self.lang = lang
        self.proc = subprocess.Popen(
            [*agent_command(lang), *map(str, args)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._pump = threading.Thread(target=self._read_lines, daemon=True)
        self._pump.start()

    def _read_lines(self) -> None:
        assert self.proc.stdout is not None
        for line in self.proc.stdout:
            self._lines.put(line)
        self._lines.put(None)

    def next(self, timeout: float = TIMEOUT) -> dict[str, Any]:
        """The next result."""
        try:
            line = self._lines.get(timeout=timeout)
        except queue.Empty:
            raise AssertionError(f"{self.lang} agent: no result within {timeout} s") from None
        if line is None:
            self._lines.put(None)
            raise AssertionError(f"{self.lang} agent exited with {self.proc.wait()}")
        return json.loads(line)

    def quiet(self, seconds: float) -> None:
        """Asserts that no result arrives for this long."""
        try:
            line = self._lines.get(timeout=seconds)
        except queue.Empty:
            return
        raise AssertionError(f"{self.lang} agent: unexpected result {line!r}")

    def send(self, *args: object) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(" ".join(map(str, args)) + "\n")
        self.proc.stdin.flush()

    def ask(self, *args: object) -> dict[str, Any]:
        self.send(*args)
        return self.next()

    def finish(self, returncode: int = 0) -> list[dict[str, Any]]:
        """Closes stdin, checks the exit status and returns the remaining results."""
        assert self.proc.stdin is not None
        self.proc.stdin.close()
        assert self.proc.wait(timeout=TIMEOUT) == returncode, f"{self.lang} agent"
        self._pump.join(TIMEOUT)
        results = []
        while (line := self._lines.get_nowait()) is not None:
            results.append(json.loads(line))
        self._lines.put(None)
        return results

    def kill(self) -> None:
        self.proc.kill()
        self.proc.wait()

    def close(self) -> None:
        if self.proc.poll() is None:
            self.kill()
        assert self.proc.stdin is not None and self.proc.stdout is not None
        with contextlib.suppress(BrokenPipeError):
            self.proc.stdin.close()
        self._pump.join(TIMEOUT)
        self.proc.stdout.close()


Spawn = Callable[..., Agent]


@pytest.fixture
def spawn() -> Iterator[Spawn]:
    """Starts agents: spawn(lang, *args); without args, a session of reader commands.
    Kills whatever is still running at the end of the test."""
    agents: list[Agent] = []

    def start(lang: str, *args: object) -> Agent:
        agents.append(Agent(lang, *args))
        return agents[-1]

    yield start
    for agent in agents:
        agent.close()
