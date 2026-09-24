#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Builds the wheel, installs it into a scratch directory and runs the tests
# against it and the built library, then ruff. Runs in the build container
# after the release preset (or the build directory given):
#
#   docker/run.sh cmake --workflow --preset release
#   docker/run.sh bindings/python/check.sh [build/<preset>]
set -eu

here=$(cd "$(dirname "$0")" && pwd -P)
build=$(cd "${1:-$here/../../build/release}" && pwd -P)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cp -R "$here/pyproject.toml" "$here/README.md" "$here/src" "$tmp/"
python3 -m pip wheel --quiet --no-deps --no-build-isolation --wheel-dir "$tmp/dist" "$tmp"
python3 -m pip install --quiet --no-deps --no-index --target "$tmp/site" "$tmp"/dist/*.whl

cd "$here"
PSMSGR_LIBRARY="$build/libpsmsgr.so.1" PYTHONPATH="$tmp/site" python3 -m pytest -ra tests
ruff check .
ruff format --check .
