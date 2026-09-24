#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Run a command in the build container, with the repository mounted at /src
# and the working directory mapped to the same place inside the repo.
# Builds the image on first use. Files are created as the calling user.
#
#   docker/run.sh cmake --workflow --preset dev
#
# PSMSGR_BUILD_IMAGE overrides the image tag (default: psmsgr-build).
# PSMSGR_REBUILD_IMAGE=1 forces a rebuild of the image.
# PSMSGR_TORTURE_SECONDS, if set, is passed through to the container.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd -P)
image=${PSMSGR_BUILD_IMAGE:-psmsgr-build}

if [ "${PSMSGR_REBUILD_IMAGE:-0}" = 1 ] || ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -t "$image" -f "$repo/docker/build.Dockerfile" "$repo/docker"
fi

here=$(pwd -P)
case "$here/" in
    "$repo"/*) workdir="/src${here#"$repo"}" ;;
    *)         workdir=/src ;;
esac

tty=
if [ -t 0 ] && [ -t 1 ]; then tty=-it; fi

# shellcheck disable=SC2086  # $tty is intentionally empty or one word
exec docker run --rm $tty \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --env PSMSGR_TORTURE_SECONDS \
    --volume "$repo:/src" \
    --workdir "$workdir" \
    "$image" "$@"
