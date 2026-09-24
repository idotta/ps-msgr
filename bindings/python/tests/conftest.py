# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import os
from pathlib import Path

import pytest

import ps_msgr

CHAN = "chan"


def gen_after(gen: int, n: int) -> int:
    """The generation n publishes after ``gen``: a new channel starts at a
    random one, and 0 is skipped on wrap."""
    for _ in range(n):
        gen = 1 if gen == 0xFFFF_FFFF else gen + 1
    return gen


def data_path(directory: Path, name: str = CHAN) -> Path:
    return directory / f"psmsgr.{name}.state"


def lock_path(directory: Path, name: str = CHAN) -> Path:
    return directory / f"psmsgr.{name}.lock"


def _build_file(relative: str) -> Path:
    library = os.environ.get("PSMSGR_LIBRARY")
    if not library:
        pytest.skip("PSMSGR_LIBRARY is not set, so there is no build tree with the C helpers")
    path = Path(library).resolve().parent / relative
    if not path.is_file():
        pytest.fail(f"{path} is missing: build the preset that built {library}")
    return path


@pytest.fixture(scope="session")
def helper() -> Path:
    return _build_file("tests/interop_helper")


@pytest.fixture(scope="session")
def child_env() -> dict[str, str]:
    """Environment for a Python child process that imports this ps_msgr."""
    return {**os.environ, "PYTHONPATH": str(Path(ps_msgr.__file__).parent.parent)}
