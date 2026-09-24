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

# cmocka 2.0 for the C tests: trixie ships 1.1.7, which predates the current
# API. Built as a static library for the host (/usr/local) and for armhf (the
# cross root, which the toolchain file searches), so test binaries need no
# cmocka on the board. BuildKit fetches the release and checks its SHA-256.
ARG CMOCKA_VERSION=2.0.2
ADD --checksum=sha256:39f92f366bdf3f1a02af4da75b4a5c52df6c9f7e736c7d65de13283f9f0ef416 \
    https://cmocka.org/files/2.0/cmocka-${CMOCKA_VERSION}.tar.xz /tmp/
RUN set -eu; cd /tmp; \
    tar xf cmocka-${CMOCKA_VERSION}.tar.xz; \
    common="-G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
            -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DWITH_EXAMPLES=OFF"; \
    cmake -S cmocka-${CMOCKA_VERSION} -B build-host $common \
          -DCMAKE_INSTALL_PREFIX=/usr/local; \
    cmake -S cmocka-${CMOCKA_VERSION} -B build-armhf $common \
          -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
          -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
          -DCMAKE_INSTALL_PREFIX=/usr/arm-linux-gnueabihf; \
    cmake --build build-host --target install; \
    cmake --build build-armhf --target install; \
    rm -rf /tmp/*

WORKDIR /src
