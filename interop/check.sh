#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Runs the interop suite (README.md): builds the C agent (tests/interop_helper),
# installs the Python binding's wheel into a scratch directory for the Python agent,
# publishes the C# agent with Native AOT, and runs pytest, then ruff and dotnet format.
# Runs in the build container after the release preset (or the build directory given):
#
#   docker/run.sh cmake --workflow --preset release
#   docker/run.sh interop/check.sh [build/<preset>]
#
# The C# agent goes to <build>/interop/agent_cs.
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
repo=$(cd "$here/.." && pwd -P)
build=$(cd "${1:-$repo/build/release}" && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

case $(uname -m) in
    x86_64) rid=linux-x64 ;;
    aarch64) rid=linux-arm64 ;;
    *) echo "check.sh: no Native AOT runtime identifier for $(uname -m)" >&2; exit 1 ;;
esac

export NUGET_PACKAGES="${NUGET_PACKAGES:-$repo/build/nuget}"
export PSMSGR_LIBRARY="$build/libpsmsgr.so.1"

cmake --build "$build" --target interop_helper
export PSMSGR_INTEROP_C="$build/tests/interop_helper"

cp -R "$repo/bindings/python/pyproject.toml" "$repo/bindings/python/README.md" "$repo/bindings/python/src" "$tmp/"
python3 -m pip wheel --quiet --no-deps --no-build-isolation --wheel-dir "$tmp/dist" "$tmp"
python3 -m pip install --quiet --no-deps --no-index --target "$tmp/site" "$tmp"/dist/*.whl
export PYTHONPATH="$tmp/site"

dotnet publish "$here/agent_cs" -c Release -r "$rid" -o "$build/interop/agent_cs"
export PSMSGR_INTEROP_CS="$build/interop/agent_cs/PsMsgr.InteropAgent"

cd "$here"
python3 -m pytest -ra .
ruff check .
ruff format --check .
dotnet format agent_cs --verify-no-changes --no-restore
