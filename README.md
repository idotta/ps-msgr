# ps-msgr

Inter-process state exchange for embedded Linux, with a primary target of
the BeagleBone Black. The core is one C library (`libpsmsgr`) with thin
Python and C# bindings.

**State channels** publish the latest value of an opaque payload from one
writer to many readers through a shared-memory file in `/dev/shm`:

- Lock-free reads that never see a torn value.
- No syscalls on the hot path.
- Generation and timestamp without copying the payload (`peek`).
- Optional blocking `wait`.
- Readers can start before the writer.

Status: **specification draft**, not implemented yet. See [`spec/`](spec/README.md).

| Directory | Contents |
|---|---|
| `spec/` | Channel format, protocol, APIs, build and test requirements |
| `c/` | `libpsmsgr`, `psmsgr-dump`, C tests |
| `python/` | `ps_msgr` (ctypes binding) |
| `csharp/` | `PsMsgr` (P/Invoke binding) |

License: Apache-2.0.
