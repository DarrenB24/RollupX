FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build ca-certificates curl \
    libgrpc++-dev protobuf-compiler protobuf-compiler-grpc libprotobuf-dev \
    librocksdb-dev libssl-dev libgtest-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build --parallel 2 \
    && ctest --test-dir build --output-on-failure \
    && cmake --install build --prefix /opt/rollupx

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# Keep distro-managed shared dependencies. Image-size tuning is a later task.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates curl libgrpc++-dev libprotobuf-dev librocksdb-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home rollupx \
    && mkdir -p /var/lib/rollupx && chown rollupx:rollupx /var/lib/rollupx
COPY --from=build /opt/rollupx/bin/ /usr/local/bin/
USER 10001:10001
WORKDIR /var/lib/rollupx
EXPOSE 50051 50052 9100
STOPSIGNAL SIGTERM
ENTRYPOINT ["rollupx-node"]
