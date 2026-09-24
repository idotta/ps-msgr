# SPDX-License-Identifier: Apache-2.0
#
# Build container for ps-msgr: every build (host and armhf) runs in here, in
# CI and locally. Debian trixie is the only target OS, so building on trixie
# guarantees binaries match the board's glibc.
#
#   docker build -t psmsgr-build -f docker/build.Dockerfile docker
#   docker/run.sh cmake --workflow --preset dev

FROM debian:trixie

ARG DEBIAN_FRONTEND=noninteractive

# libc6:armhf (multiarch) lets dpkg-shlibdeps resolve armhf dependencies for
# CPack; the cross toolchain itself brings /usr/arm-linux-gnueabihf for qemu.
RUN dpkg --add-architecture armhf \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        crossbuild-essential-armhf \
        libc6:armhf \
        clang \
        libclang-rt-dev \
        cmake \
        ninja-build \
        qemu-user \
        gdb-multiarch \
        file \
        git \
        ca-certificates \
        python3 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
