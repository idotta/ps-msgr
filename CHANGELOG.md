# Changelog

All notable changes to this project are documented in this file. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
library follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
as described in [`spec/README.md`](spec/README.md).

## [Unreleased]

### Added

- `libpsmsgr`: state channels (channel format 1.0), which publish the latest
  value of a payload from one writer to many readers through a shared-memory
  file.
- `psmsgr-dump`: inspect a channel.
- `psmsgr-bench`: on-target latency benchmark.
- Debian packages for amd64 and armhf, a CMake package config and `psmsgr.pc`.
- `ps_msgr`: the Python binding (`ctypes`, Python ≥ 3.11, no dependencies),
  in `bindings/python/`.
- `PsMsgr`: the C# binding (P/Invoke, `netstandard2.1`, Native AOT
  compatible, no dependencies), in `bindings/csharp/`. CI builds its NuGet
  package.
- Build container: the .NET SDK 10 from Microsoft's apt repository.
- `interop/`: cross-language tests with a C, a Python and a C# (Native AOT)
  agent, every writer against every reader, and a CI job for them. They
  replace the interop tests of the Python and C# bindings.
- `examples/`: a writer and a reader of a motor status in C, Python and C#
  (Native AOT, also for `linux-arm`), all on one channel, so that any
  writer works with any reader. The C examples build in-tree
  (`PSMSGR_BUILD_EXAMPLES`) or against an installed library.
  `examples/check.sh` runs them against each other in CI.
