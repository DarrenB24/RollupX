# Development and native verification

## Native build

Linux/macOS, C++20 compiler, CMake >= 3.20, gRPC C++, protoc and grpc_cpp_plugin,
RocksDB, OpenSSL >= 3, and GoogleTest are required. Dependencies come from the host;
CMake does not silently download unpinned source during configuration.

Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build curl libgrpc++-dev \
  libprotobuf-dev protobuf-compiler protobuf-compiler-grpc librocksdb-dev \
  libssl-dev libgtest-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

macOS dependencies (requires Homebrew write/network access):

```sh
brew install cmake ninja grpc protobuf rocksdb openssl@3 googletest
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)" -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

ASan/UBSan: configure a separate directory with `-DROLLUPX_SANITIZE=ON`.
TSan: use another directory with `-DROLLUPX_TSAN=ON`. The two options are mutually
exclusive. System gRPC/RocksDB are not sanitizer-instrumented by these switches.

On the tested macOS installation, mixing instrumented code with prebuilt static
RocksDB/GoogleTest archives produced container/heap overflow reports, including
in existing tests. The verified sanitizer configuration uses
`-DROLLUPX_ROCKSDB_SHARED=ON` and a matching GoogleTest source build compiled with
ASan/UBSan. No sanitizer checks are disabled. Partially instrumented containers
are a documented [Clang sanitizer limitation](https://clang.llvm.org/docs/AddressSanitizer.html#additional-checks).
Build the same GoogleTest version as the installed headers (1.18.0 in this run):

```sh
cmake -S /path/to/googletest-1.18.0 -B build-gtest-asan \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=20 -DBUILD_SHARED_LIBS=ON \
  -DBUILD_GMOCK=OFF -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_INSTALL_PREFIX="$PWD/.deps/gtest-asan"
cmake --build build-gtest-asan --parallel 2
cmake --install build-gtest-asan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DROLLUPX_SANITIZE=ON \
  -DROLLUPX_ROCKSDB_SHARED=ON -DGTest_DIR="$PWD/.deps/gtest-asan/lib/cmake/GTest" \
  -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
```

## Foundation checks when external libraries are unavailable

```sh
bash scripts/check-foundation.sh
SANITIZE=1 bash scripts/check-foundation.sh
```

This directly compiles configuration and HTTP modules with the system C++ compiler.
It is a supplementary test runner, not a replacement for GoogleTest or the gRPC
build. If sockets are forbidden, `--config-only` explicitly tests only configuration.
With CMake installed, the equivalent reduced target is
`-DROLLUPX_FOUNDATION_ONLY=ON`. It does not build a node binary. If GoogleTest is
discoverable, this reduced build also registers `config_gtests`. CTest
`-R config` runs only configuration tests; `http_checks` requires loopback sockets.

## Five real local processes

For generated-protobuf transaction validation without storage or sockets, see
[transaction-wire.md](transaction-wire.md). `-DROLLUPX_WIRE_ONLY=ON` builds the
offline core, adapter, `rollupx-wire`, and their real protobuf tests.

For the standalone transaction ledger tier (real RocksDB, no gRPC), see
[persistence.md](persistence.md). Use `-DROLLUPX_LEDGER_ONLY=ON`; its CTest suite
includes persistence/restart/crash tests and requires no listening sockets.

```sh
bash scripts/local-smoke.sh ./build/rollupx-node ./build/rollupx-cli
```

It starts five independent processes/databases, polls actual peer observations,
kills node 3, waits for its peer status to become stale, and reopens the same DB
before checking full reconnection. A failed process or deadline fails the script;
logs are printed on failure. Ports default to 24010–24052. Set
`ROLLUPX_SMOKE_BASE_PORT` to move the test range. CTest runs this script too.
These are transport recovery tests, not proof of replicated-ledger durability.

## Docker Compose

```sh
docker compose up --build -d --wait
docker compose exec node1 rollupx-cli status 127.0.0.1:50051
curl --fail http://127.0.0.1:9101/readyz
docker compose --profile monitoring up -d
# Prometheus: http://127.0.0.1:9090 ; Grafana: http://127.0.0.1:3000
docker compose down
```

Each node has a separate named volume. Public gRPC ports 50051–50055 and HTTP
ports 9101–9105 bind to host loopback. Internal gRPC port 50052 is reachable only
within the Compose network. Host port 50052 maps node2's **public** listener.
`bash scripts/compose-smoke.sh` automates the mesh/restart test and stops services
afterwards; it preserves volumes. It should run on a dedicated local test stack.
Anvil, load-generator services, and Kubernetes are deliberately pending their
implementation milestones. The current dashboard measures transport, not TPS.

## API and operational semantics

`--genesis FILE --public-peers id=host:port,...` enables the replicated ledger.
See [replicated-node.md](replicated-node.md) for submission/retry semantics,
local-read consistency, leader readiness, lifecycle, and remaining limits.

Without those options, the networking diagnostic mode still reports
`NETWORKING_ONLY`, exposes probes/status, and rejects ledger calls as
`UNIMPLEMENTED`. The default Compose setup uses that diagnostic mode.

Both modes expose `/healthz`, `/readyz` and `/metrics`. Replicated readiness is
leader write-readiness; networking readiness is full-mesh probe connectivity.
Transport probes do not establish a consensus quorum. Public/internal listeners
are separate, with bounded messages, queues and RPC deadlines. A database cannot
be silently reused across modes or with an incompatible durable identity.

The replicated five-process crash test is registered in full-build CTest:

```sh
bash scripts/replicated-smoke.sh ./build/rollupx-node ./build/rollupx-cli ./build/rollupx-wallet
```

Listeners currently support IPv4/DNS hostnames, not IPv6 literals or TLS. Same-host
tests do not establish geographic fault tolerance. Before sharing images publicly,
review and pin current dependency/image versions and scan for vulnerabilities.

## Verification

See IMPLEMENTATION_PLAN.md for exact results. Native gRPC/RocksDB recovery,
network tests and the Compose transport demo passed on Ubuntu CI for `1a14d01`.
Homebrew dependencies now permit local native builds and the replicated-node
suite. Local socket tests require execution outside the restricted sandbox.
Docker/Foundry deployment work remains separate from these native checks.
