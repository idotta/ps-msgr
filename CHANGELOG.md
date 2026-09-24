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
