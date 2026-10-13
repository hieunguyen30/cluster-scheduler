# syntax=docker/dockerfile:1
#
# Stages:
#   build        plain optimised build of all binaries (what ships)
#   test-asan    ASan+UBSan build; `docker build --target test-asan` FAILS if any test fails
#   tsan         ThreadSanitizer build, tests NOT run at build time: Docker's default seccomp
#                profile forbids personality(ADDR_NO_RANDOMIZE), which TSan needs on kernels with
#                high ASLR entropy. Run it with:
#                  docker run --rm --security-opt seccomp=unconfined <image> ctest ...
#   runtime      slim image: binaries + shared libs only (default target)
#
FROM ubuntu:24.04 AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build pkg-config git ca-certificates \
      libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY proto proto
COPY src src
COPY tests tests

FROM base AS build
RUN cmake -S . -B /build/plain -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
 && cmake --build /build/plain -j"$(nproc)"

FROM base AS test-asan
ENV KESTREL_TIME_SCALE=2 ASAN_OPTIONS=detect_leaks=1
RUN cmake -S . -B /build/asan -G Ninja -DKESTREL_SANITIZE=address \
 && cmake --build /build/asan -j"$(nproc)" \
 && ctest --test-dir /build/asan --output-on-failure -j"$(nproc)"

FROM base AS tsan
ENV KESTREL_TIME_SCALE=3 \
    TSAN_OPTIONS="halt_on_error=0 ignore_noninstrumented_modules=1 suppressions=/src/tests/tsan.supp"
RUN cmake -S . -B /build/tsan -G Ninja -DKESTREL_SANITIZE=thread \
 && cmake --build /build/tsan -j"$(nproc)"
CMD ["ctest", "--test-dir", "/build/tsan", "--output-on-failure"]

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# tini: PID 1 must reap orphaned grandchildren of job processes, or they pile up as zombies.
RUN apt-get update && apt-get install -y --no-install-recommends \
      libgrpc++1.51t64 ca-certificates tini \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --no-create-home kestrel
COPY --from=build /build/plain/src/kestrel_control /build/plain/src/kestrel_worker \
     /build/plain/src/kestrelctl /build/plain/src/kestrel_loadgen /usr/local/bin/
USER 10001
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["kestrel_control"]
