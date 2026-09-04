# syntax=docker/dockerfile:1
#
# Multi-stage: vcpkg/CMake build in a full Debian image, slim runtime with just the
# resulting binary — vcpkg's Linux triplets link everything statically, so there are no
# shared libs to carry across. Same base distro/version in both stages regardless, so
# glibc ABI matches across the COPY --from boundary.
#
# Builds for whatever platform invokes `docker build` (no --platform flag here): arm64
# natively on Apple Silicon for local dev, amd64 in CI (Step 3) for the deploy image.
# Never cross-build the amd64 image on the Mac — that's what CI is for.

FROM debian:bookworm AS build
ARG TARGETARCH
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake curl zip unzip tar git pkg-config \
      autoconf autoconf-archive automake libtool python3 ca-certificates \
      bison flex perl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# See scripts/fetch-vcpkg.sh for why this doesn't just COPY the host's vcpkg/ submodule
# checkout (also used by CI, so the fetch logic is single-sourced there, not duplicated here).
COPY scripts/fetch-vcpkg.sh scripts/fetch-vcpkg.sh
RUN ./scripts/fetch-vcpkg.sh /src/vcpkg

COPY . .

RUN case "$TARGETARCH" in \
      amd64) echo x64-linux > /tmp/vcpkg_triplet ;; \
      arm64) echo arm64-linux > /tmp/vcpkg_triplet ;; \
      *) echo "unsupported TARGETARCH=$TARGETARCH" >&2; exit 1 ;; \
    esac

# Cache mount: vcpkg's compiled-package cache would otherwise live only in this throwaway
# build layer and be recompiled from scratch (~10 min, incl. Drogon) on every Dockerfile
# change. This persists it across builds — the CI equivalent of Step 3's vcpkg binary cache.
RUN --mount=type=cache,target=/root/.cache/vcpkg/archives \
    cmake -S . -B build \
      -DCMAKE_TOOLCHAIN_FILE=/src/vcpkg/scripts/buildsystems/vcpkg.cmake \
      -DVCPKG_TARGET_TRIPLET="$(cat /tmp/vcpkg_triplet)" \
      -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

FROM debian:bookworm-slim AS runtime
# vcpkg's community Linux triplets (arm64-linux, x64-linux) default to STATIC linking —
# verified 2026-09-05: the built binary's only dynamic deps are libstdc++/libm/libgcc_s/libc,
# all standard and already present below. No vcpkg .so/.a files need copying at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/receipt_scanner_server /usr/local/bin/receipt_scanner_server
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/receipt_scanner_server"]
