from __future__ import annotations

import os
import tempfile
from contextlib import suppress
from pathlib import Path


class AtomicIpcState:
    """
    Single-writer / multiple-reader atomic state file.

    A writer creates a complete temporary file and atomically replaces
    the published target using os.replace().

    Readers therefore see either the previous complete snapshot or the
    new complete snapshot, never a partially-written file.
    """

    def __init__(
        self,
        name: str,
        directory: str | os.PathLike[str] = "/tmp",
        extension: str = ".bin",
        *,
        mode: int = 0o644,
        sync: bool = False,
    ) -> None:
        self.directory: Path = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)

        self.name: str = name
        self.extension: str = (
            extension if extension.startswith(".") else f".{extension}"
        )

        self.target_path: Path = (
            self.directory / f"{self.name}{self.extension}"
        )

        self.mode: int = mode
        self.sync: bool = sync

    def write(self, data: str | bytes) -> None:
        """Atomically publish a new snapshot."""

        fd, temp_name = tempfile.mkstemp(
            prefix=f"{self.name}_",
            suffix=".tmp",
            dir=self.directory,
        )

        temp_path: Path = Path(temp_name)

        try:
            with os.fdopen(fd, "wb") as f:
                if isinstance(data, str):
                    data = data.encode("utf-8")

                f.write(data)
                f.flush()

                if self.sync:
                    os.fsync(f.fileno())

            os.chmod(temp_path, self.mode)
            os.replace(temp_path, self.target_path)

        finally:
            with suppress(FileNotFoundError):
                temp_path.unlink()

    def read_bytes(self) -> bytes | None:
        """Read the current snapshot as bytes."""
        try:
            return self.target_path.read_bytes()
        except FileNotFoundError:
            return None

    def read_text(self) -> str | None:
        """Read the current snapshot as UTF-8 text."""
        try:
            return self.target_path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return None

    def clear(self) -> None:
        """Remove the published snapshot if it exists."""
        with suppress(FileNotFoundError):
            self.target_path.unlink()

    @property
    def exists(self) -> bool:
        """Whether a snapshot is currently published."""
        return self.target_path.exists()
