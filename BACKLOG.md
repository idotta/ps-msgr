# Backlog

Items found in the pre-1.0 review. Remove an item in the pull request that
resolves it.

## Should fix

- [ ] **Python install on Debian 13.** `pip install bindings/python`
  (`bindings/python/README.md:14`, `examples/README.md:113`) fails with PEP 668
  "externally-managed-environment". Document a venv.
- [ ] **Debian packages:** install `copyright`/`LICENSE`
  (`CMakeLists.txt:140-173`).
- [ ] C#: `Errors.cs:72` uses `strerror`. Use `strerror_r`, which is
  thread-safe.
- [ ] C#: the README usage snippet doesn't compile (CS8803, type before
  top-level statements).
- [ ] Python: `wait(0, 1e300)` raises `OverflowError` (`_state.py:362`). Clamp
  large finite timeouts.
- [ ] Examples: `examples/c/motor_writer.c:96` ignores the result of
  `publish`.
