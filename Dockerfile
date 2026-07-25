# CHRONOS daemon image. Multi-stage: heavy toolchain in the build stage,
# a slim runtime with just the binary + dashboard in the final image.
#
#   docker build -t chronos .
#   docker run -p 8080:8080 chronos            # daemon + dashboard
#   docker run -p 8080:8080 chronos --demo     # with a live demo workload

FROM gcc:13 AS build
RUN apt-get update && apt-get install -y --no-install-recommends cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY include/ include/
COPY src/ src/
COPY apps/ apps/
COPY tests/ tests/
RUN cmake --preset release -G Ninja \
    && cmake --build --preset release --target chronosd chronos_sim chronos_bench

FROM debian:bookworm-slim AS runtime
RUN useradd --system --uid 1001 chronos
WORKDIR /opt/chronos
COPY --from=build /src/build/release/chronosd /src/build/release/chronos-sim \
                  /src/build/release/chronos-bench /usr/local/bin/
COPY dashboard/ dashboard/
USER chronos
EXPOSE 8080
ENTRYPOINT ["chronosd", "--port=8080", "--static-dir=/opt/chronos/dashboard"]
CMD []
