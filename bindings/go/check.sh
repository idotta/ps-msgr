#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Checks gofmt and go vet, and runs the tests against the built library:
# linked dynamically, with the race detector, and linked statically
# (psmsgr_static). Runs in the build container after the release preset (or
# the build directory given):
#
#   docker/run.sh cmake --workflow --preset release
#   docker/run.sh bindings/go/check.sh [build/<preset> [build/<armhf-preset>]]
#
# If the armhf build exists (default build/armhf-release; built, not
# necessarily tested), the tests are also cross-built for linux/arm and run
# under qemu-arm. The Go build cache is build/go (GOCACHE overrides it).
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/../.." && pwd -P)
build=$(cd "${1:-$repo/build/release}" && pwd -P)
armhf=${2:-$repo/build/armhf-release}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

export GOTOOLCHAIN=local
export GOCACHE="${GOCACHE:-$repo/build/go/cache}"
export GOPATH="$repo/build/go/path"
# The library's headers from the source tree, the library from the build.
export CGO_CFLAGS="-I$repo/include -O2 -g"
export CGO_LDFLAGS="-L$build"
export LD_LIBRARY_PATH="$build"
# The tests take tests/interop_helper from here.
export PSMSGR_BUILD_DIR="$build"

cd "$here"
unformatted=$(gofmt -l .)
if [ -n "$unformatted" ]; then
    echo "check.sh: not gofmt-formatted: $unformatted" >&2
    gofmt -d . >&2
    exit 1
fi
go vet ./...
go vet -tags psmsgr_static ./...

go test -count=1 ./...
go test -count=1 -race ./...
# The benchmarks run, for a few iterations: numbers from here mean nothing.
go test -count=1 -run '^$' -bench . -benchtime 100x ./...

# Linked statically: the executable must not need libpsmsgr.so.1.
go test -c -tags psmsgr_static -o "$tmp/static.test" ./psmsgr
if readelf -d "$tmp/static.test" | grep -q 'libpsmsgr'; then
    echo "check.sh: the psmsgr_static build still needs libpsmsgr.so.1" >&2
    exit 1
fi
(cd psmsgr && env -u LD_LIBRARY_PATH "$tmp/static.test" -test.count=1)

# 32-bit ARM (the BeagleBone Black) under qemu, without the helper process.
if [ -f "$armhf/libpsmsgr.so.1" ]; then
    armhf=$(cd "$armhf" && pwd -P)
    CGO_ENABLED=1 GOOS=linux GOARCH=arm GOARM=7 CC=arm-linux-gnueabihf-gcc \
        CGO_LDFLAGS="-L$armhf" go test -c -o "$tmp/armhf.test" ./psmsgr
    (cd psmsgr && env -u PSMSGR_BUILD_DIR qemu-arm -L /usr/arm-linux-gnueabihf \
        -E LD_LIBRARY_PATH="$armhf" "$tmp/armhf.test" -test.count=1)
else
    echo "check.sh: no $armhf/libpsmsgr.so.1, skipping linux/arm"
fi
