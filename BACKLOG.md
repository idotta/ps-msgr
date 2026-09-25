# Backlog

Items found in the pre-1.0 review. Remove an item in the pull request that
resolves it.

## Before tagging 1.0

These can't be fixed after the tag without a SONAME bump, or the release
process requires them.

- [ ] **On-target run at the tag commit.**
  - `bench/results/2026-09-24-bbb-6511c25/` predates 844645d, which changed
    `src/state.c`.
  - Re-run `psmsgr-bench` and the torture test on the BeagleBone Black and
    record the results, as `spec/build-and-test.md` requires before each
    release.
- [ ] **ABI baseline at the tag.**
  - `abi_check` compares the map, the headers and the library of the same
    commit. A symbol removed or renamed in all three passes, and it doesn't
    see struct layouts (`psmsgr_state_options`, `_info`, `_desc`) at all.
  - At the tag, record the 1.0.0 library's ABI (`abidw`, from
    `abigail-tools`), and have CI run `abidiff` against the latest release
    of the same major.
  - After a major bump, skip the comparison until that major's first
    release, which becomes the new baseline. Breaking changes stay allowed;
    they need a major bump.
- [ ] **Release docs.**
  - Set every `spec/*.md` from `Status: draft` to final.
  - Add a `[1.0.0]` section with on-target measurements to `CHANGELOG.md`.
  - Remove "on-target validation is pending" from `README.md:18`, and "None
    of the examples has run on the board yet" from `examples/README.md:133`.

## Should fix

- [ ] **Use after free: closing a reader during `wait`.**
  - C#: `StateReader` passes `DangerousGetHandle()` (`StateReader.cs:307`)
    without `DangerousAddRef`/`DangerousRelease`. `Dispose()` from another
    thread during `Wait` or `IsWriterAlive` frees the native handle while it
    is in use. Guard those two calls.
  - Python: `wait` releases the GIL, and `close()` (`_state.py:407`) frees
    the handle underneath it. Make `close()` raise or defer while a `wait` is
    running.
  - Python: there is no way to stop a thread blocked in
    `wait(timeout=None)`.
- [ ] **`psmsgr-bench` heap overflow on 32-bit.** `--iterations` accepts up
  to `UINT32_MAX` (`bench/psmsgr-bench.c:671`), and `malloc(cap * sizeof
  *s->v)` (`:115`) wraps `size_t` above 2²⁹ samples. Cap the value or check
  the multiplication.
- [ ] **Binding code on armhf in CI.**
  - Python, C#, interop and example jobs run only on x86-64 and arm64.
  - `examples/check.sh:43-50` builds the `linux-arm` Native AOT examples but
    never runs them.
  - Run pytest, the xUnit layout tests and the AOT smoke under qemu-arm.
  - Add one build of the documented static-link path (`DirectPInvoke` with
    `libpsmsgr.a`).
- [ ] **Release workflow.**
  - `.github/workflows/ci.yml` has no tag trigger.
  - Publish the .debs, the .nupkg and the Python wheel (never uploaded
    today) to a GitHub Release.
- [ ] **Python install on Debian 13.** `pip install bindings/python`
  (`bindings/python/README.md:14`, `examples/README.md:113`) fails with PEP 668
  "externally-managed-environment". Ship a `python3-ps-msgr` .deb, or
  document a venv.
- [ ] **Debian package metadata** (`CMakeLists.txt:140-173`).
  - Add an email address to the maintainer contact.
  - Install `copyright`/`LICENSE`.
  - Give each package its own description.
  - Consider a `-dbgsym` package, since the binaries are stripped.
  - Add a CI job that installs the .debs on a clean trixie and builds against
    `psmsgr.pc`.
- [ ] **Bench under `chrt`.**
  - With `chrt -f 50`, the forked writer inherits the reader's FIFO
    priority. On one core it never preempts the spinning reader, so the
    "read, writer active" rows probably measure reads with no writer running.
  - Report the writer's publish count and the distinct generations the
    reader saw, and give the writer its own priority.
- [ ] **README for 1.0 users.** Add an install/quickstart section and the
  stability policy, point to the `/dev/shm` permission notes
  (`spec/state-channel.md` §8), and add `interop/` to the directory table.

## Nice to have

- [ ] Core: map with `MAP_POPULATE` when the writer reuses an existing file
  (`src/state.c:508`), so the first publishes after a restart don't
  page-fault.
- [ ] Core: an `mlock` option for real-time users.
- [ ] Python: `wait(0, 1e300)` raises `OverflowError` (`_state.py:362`). Clamp
  large finite timeouts.
- [ ] Bindings: a positive timeout of a few µs can become a poll, because the
  first slice is computed after a second clock read (`_state.py:362,368`,
  `StateReader.cs:202-206,220`).
- [ ] Bindings: `PSMSGR_INFO_ATTACHED` is lost if `describe` fails right
  after the flag is consumed (`_state.py:294,303`,
  `StateReader.cs:139-155,286-297`).
- [ ] Bindings, API parity:
  - Python: zero-copy `begin`/`commit`/`abort`, and `age_ns` on `Snapshot`.
  - C#: `IsDisposed`.
  - Both: the binding's own version and the loaded library's version, plus
    `NAME_MAX` and `STATE_MAX_CAPACITY`.
- [ ] C# tests:
  - The finalizer releases the writer lock.
  - `Wait` on an otherwise unreferenced reader during a GC.
  - The `PSMSGR_LIBRARY` load-failure path.
- [ ] C#: `Errors.cs:72` uses `strerror`. Use `strerror_r`, which is
  thread-safe.
- [ ] C#: the README usage snippet doesn't compile (CS8803, type before
  top-level statements).
- [ ] NuGet: `RepositoryUrl`, SourceLink, a symbols package and
  `IsTrimmable`, and absolute links in the packed README.
- [ ] PyPI metadata: `authors`, `urls` and a `Development Status` classifier.
- [ ] `spec/bindings.md:104` calls Python `read_into` "zero-alloc", but it
  allocates small objects on every call. Fix the wording.
- [ ] CMake: guard `install()` and `include(CPack)` with
  `PROJECT_IS_TOP_LEVEL`, and namespace `psmsgr_flags`.
- [ ] Packaging: `psmsgr-tools` ships `psmsgr-bench` on armhf but not on
  amd64.
- [ ] Reproducibility:
  - Pin `debian:trixie` by digest and the GitHub actions by SHA.
  - `docker/run.sh:122` silently reuses a stale image.
- [ ] Bench:
  - Ctrl-C leaves `/dev/shm/psmsgr-bench-*` files behind.
  - `missed` (`:608`) counts one spurious miss at the generation wrap.
  - Two CSV test names contain unquoted commas.
- [ ] Examples:
  - `examples/c/motor_writer.c:96` ignores the result of `publish`.
  - The Python and C# readers check the size before the payload type, unlike
    the C reader.
  - `tests/interop_helper.c:79` duplicates `struct motor_status` instead of
    including `examples/motor_status.h`.

## Features for 1.x

These are additive and need no ABI break.

- [ ] Event-loop integration: an fd for `epoll`, asyncio and `Task`-based
  waits.
- [ ] `wait` with an absolute deadline.
- [ ] A writer-side describe: slot count, payload type and flags.
