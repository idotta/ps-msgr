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
- [ ] **ABI snapshot at the tag.** Record the 1.0.0 library's ABI with
  `abidw` (from `abigail-tools`), so a later release can be compared with
  `abidiff`. `abi_check` doesn't see struct layouts (`psmsgr_state_options`,
  `_info`, `_desc`).
- [ ] **Release docs.**
  - Set every `spec/*.md` from `Status: draft` to final.
  - Add a `[1.0.0]` section with on-target measurements to `CHANGELOG.md`.
  - Remove "on-target validation is pending" from `README.md:18`, and "None
    of the examples has run on the board yet" from `examples/README.md:133`.

## Should fix

- [ ] **Python install on Debian 13.** `pip install bindings/python`
  (`bindings/python/README.md:14`, `examples/README.md:113`) fails with PEP 668
  "externally-managed-environment". Document a venv.
- [ ] **Debian packages:** install `copyright`/`LICENSE`
  (`CMakeLists.txt:140-173`).
- [ ] Python: `wait(0, 1e300)` raises `OverflowError` (`_state.py:362`). Clamp
  large finite timeouts.
