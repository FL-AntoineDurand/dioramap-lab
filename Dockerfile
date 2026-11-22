# dioramap-processor — runtime image.
#
# Two-stage build:
#   1. `builder` — Ubuntu 26.04 with the toolchain; builds dioramap-processor.
#   2. `runtime` — slim Ubuntu 26.04 with only the runtime shared libs.
#
# Build:  docker build -t dioramap-processor .
# Run:    docker run --rm -p 127.0.0.1:7801:7801 \
#           -v dioramap-cache:/var/cache/dioramap dioramap-processor
#
# The container listens on all interfaces *inside* the container; publish the
# port on 127.0.0.1 as above, or set DIORAMAP_TOKEN to require a shared token
# (clients connect with ?token=...). Pipelines listed in core/CMakeLists.txt
# (EMBEDDED_PIPELINES) are baked into the binary; mount a directory and pass
# --pipelines-dir to use others.

# ─────────────────────────── Stage 1: build ──────────────────────────
FROM ubuntu:26.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        libwebsockets-dev \
        libcurl4-openssl-dev \
        libproj-dev \
        libjpeg-dev \
        libjansson-dev \
        libsqlite3-dev \
        libssl-dev \
        libxxhash-dev \
        ca-certificates \
        git \
        nodejs \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# third_party/ is not in git, so dependencies are fetched inside the build.
COPY . /src/

# copc-lib + laz-perf back LAZ/COPC decompression (pinned SHAs in the script).
RUN ./scripts/fetch-third-party.sh \
    && find /src/third_party -name .git -type d -prune -exec rm -rf {} +

RUN cmake -S core -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDIORAMAP_BUILD_TESTS=OFF \
    && cmake --build build --target dioramap_processor -j

# ───────────────────────── Stage 2: runtime ──────────────────────────
FROM ubuntu:26.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# NOTE: t64 SONAME package renames on 26.04 — libwebsockets19t64,
# libcurl4t64, libssl3t64 (were …19/…4/…3 on 24.04). libproj25,
# libjpeg-turbo8, libjansson4, libsqlite3-0, libxxhash0 unchanged.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libwebsockets19t64 \
        libcurl4t64 \
        libproj25 \
        libjpeg-turbo8 \
        libjansson4 \
        libsqlite3-0 \
        libssl3t64 \
        libxxhash0 \
        libstdc++6 \
        ca-certificates \
        # netcat for the HEALTHCHECK probe
        netcat-openbsd \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd -r dioramap && useradd -r -g dioramap -d /var/cache/dioramap dioramap

# Binary only — embedded pipelines live in rodata (see header).
COPY --from=builder /src/build/dioramap-processor /opt/dioramap/bin/dioramap-processor

# Cache root. Mount a volume here to keep the cache across restarts.
RUN mkdir -p /var/cache/dioramap && chown -R dioramap:dioramap /var/cache/dioramap

USER dioramap
WORKDIR /opt/dioramap

ENV SERVE_PORT=7801 \
    DIORAMAP_CACHE_DIR=/var/cache/dioramap

EXPOSE 7801

HEALTHCHECK --interval=15s --timeout=3s --start-period=5s --retries=3 \
    CMD nc -z localhost ${SERVE_PORT} || exit 1

# Shell form so SERVE_PORT flows through. The processor falls back to
# embedded pipelines when --pipelines-dir is omitted.
CMD /opt/dioramap/bin/dioramap-processor serve \
        --bind "*" \
        --port "$SERVE_PORT" \
        --cache-dir "$DIORAMAP_CACHE_DIR"
