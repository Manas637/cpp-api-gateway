# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# Build stage
#
# The gateway's Redis rate limiter uses boost::asio::cancel_after with
# Boost.Redis' async_exec, which is only supported from Boost 1.90 onwards
# (Boost.Redis gained reliable per-operation cancellation in 1.90). The
# project is developed against Boost 1.92, so this image uses Debian sid,
# which currently packages libboost1.92-dev. Boost.Redis is header-only and
# is compiled into the binary through <boost/redis/src.hpp>, so no compiled
# Boost libraries are required.
# ---------------------------------------------------------------------------
FROM debian:sid AS build

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        libboost1.92-dev \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Only the sources needed to configure and build the gateway are copied.
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests

# Only the gateway executable is built; test targets are configured but
# never compiled. Release applies NDEBUG, as with any normal distribution
# build.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target api_gateway -j"$(nproc)"

# ---------------------------------------------------------------------------
# Runtime stage
# ---------------------------------------------------------------------------
FROM debian:sid AS runtime

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        libssl3t64 \
    && rm -rf /var/lib/apt/lists/*

# Run as an unprivileged system user.
RUN useradd --system --create-home --uid 10001 gateway

COPY --from=build /src/build/api_gateway /usr/local/bin/api_gateway

# The gateway is configured entirely through environment variables
# (see .env.example) and reads no file-based configuration.
USER gateway

EXPOSE 8080

# Deliver SIGTERM to the process (do not wrap it in a shell) so the
# gateway's graceful-shutdown handler runs.
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/api_gateway"]
