#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Builds the solution, runs the xUnit tests against the built library (through the
# PSMSGR_LIBRARY preload) and the Python binding's wheel, checks dotnet format,
# publishes PsMsgr.AotSmoke with Native AOT and runs it (and with the JIT), and packs the
# library. Runs in the build container after the release preset (or the build directory
# given):
#
#   docker/run.sh cmake --workflow --preset release
#   docker/run.sh bindings/csharp/check.sh [build/<preset>]
#
# Outputs go to <build>/csharp: aot/ (the smoke binary) and nupkg/.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/../.." && pwd -P)
build=$(cd "${1:-$repo/build/release}" && pwd -P)
out=$build/csharp
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case $(uname -m) in
    x86_64) rid=linux-x64 ;;
    aarch64) rid=linux-arm64 ;;
    *) echo "check.sh: no Native AOT runtime identifier for $(uname -m)" >&2; exit 1 ;;
esac

export NUGET_PACKAGES="${NUGET_PACKAGES:-$repo/build/nuget}"
export PSMSGR_LIBRARY="$build/libpsmsgr.so.1"

# The interop tests run a Python reader and writer: the ps_msgr wheel, installed into a
# scratch directory as bindings/python/check.sh does.
cp -R "$repo/bindings/python/pyproject.toml" "$repo/bindings/python/README.md" "$repo/bindings/python/src" "$tmp/"
python3 -m pip wheel --quiet --no-deps --no-build-isolation --wheel-dir "$tmp/dist" "$tmp"
python3 -m pip install --quiet --no-deps --no-index --target "$tmp/site" "$tmp"/dist/*.whl
export PYTHONPATH="$tmp/site"

cd "$here"
dotnet build PsMsgr.sln -c Release
dotnet test --project PsMsgr.Tests -c Release --no-build
dotnet format PsMsgr.sln --verify-no-changes --no-restore

dotnet publish PsMsgr.AotSmoke -c Release -r "$rid" -o "$out/aot"
"$out/aot/PsMsgr.AotSmoke"
dotnet "PsMsgr.AotSmoke/bin/Release/net10.0/$rid/PsMsgr.AotSmoke.dll"
# A library that cannot be loaded fails the first call with a readable message.
if PSMSGR_LIBRARY=/nonexistent "$out/aot/PsMsgr.AotSmoke" 2>"$tmp/err"; then
    echo "check.sh: PsMsgr.AotSmoke ran without its library" >&2
    exit 1
fi
grep -q 'DllNotFoundException: PsMsgr cannot load /nonexistent (PSMSGR_LIBRARY): /nonexistent: cannot open shared object file' "$tmp/err" \
    || { cat "$tmp/err" >&2; exit 1; }

dotnet pack PsMsgr -c Release --no-build -o "$out/nupkg"
