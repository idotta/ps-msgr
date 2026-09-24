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

## Building

All builds run in a Debian trixie container (`docker/build.Dockerfile`),
the same one CI uses. `docker/run.sh` builds the image on first use and runs
a command in it with the repository mounted:

```sh
cd c
../docker/run.sh cmake --workflow --preset dev            # host: debug, ASan+UBSan, tests
../docker/run.sh cmake --workflow --preset armhf          # BeagleBone Black: tests under qemu
../docker/run.sh cmake --workflow --preset armhf-release  # + libpsmsgr1/-dev .deb packages
```

Presets: `dev`, `dev-clang`, `tsan`, `release`, `armhf` and `armhf-release`.
See [`spec/build-and-test.md`](spec/build-and-test.md).

License: Apache-2.0.
