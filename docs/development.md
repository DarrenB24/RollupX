# Development and Phase 1 verification

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

* Public: `Ledger.GetClusterStatus` reports `NETWORKING_ONLY`, unset Raft role, no
  leader and zero commit/application indices. `GetLeader` returns UNAVAILABLE.
* Ledger writes, balances, receipts, batches and proofs return UNIMPLEMENTED.
  Internal Raft calls also return UNIMPLEMENTED. Do not use this milestone for funds.
* Internal: `PeerTransport.Probe` checks cluster ID, complete canonical membership,
  protocol version, and claimed sender ID; the claimed identity is not authenticated.
* `/healthz`: process and listeners running. `/readyz`: every other configured peer
  has responded recently; this is explicitly **transport readiness**, not quorum or
  transaction readiness. A 3/5 transport subset is not ready in this milestone.
* `/metrics`: actual probe counts/reachability and a consensus-enabled gauge fixed
  to zero because that feature is absent. Raft/transaction metrics are not invented.
* RPCs are limited to 1 MiB; each peer has one in-flight outbound probe with a
  configurable deadline. Membership is bounded to 31; default deployment is five.
  HTTP reads are limited to 4 KiB/250 ms and are handled by one worker.
* The RocksDB identity manifest uses synchronous WAL writes. Reusing a DB for a
  different cluster/member/topology fails. This milestone persists no ledger/log.
* SIGINT/SIGTERM stop probes and listeners and release the DB. RPC deadlines bound
  peer-worker shutdown. A fatal worker/storage/listener error exits nonzero.

Listeners currently support IPv4/DNS hostnames, not IPv6 literals or TLS. Same-host
tests do not establish geographic fault tolerance. Before sharing images publicly,
review and pin current dependency/image versions and scan for vulnerabilities.

## Verification on the initial development host

See IMPLEMENTATION_PLAN.md for exact results. Initial PATH discovery did not find
CMake/Protobuf/GoogleTest, but later inspection found cached installations under
`/opt/homebrew/Caskroom/miniforge/base`. Those enabled a foundation CMake build,
schema generation, and configuration GoogleTests. gRPC/RocksDB/Docker/Foundry remain
missing, GitHub DNS fails, and the sandbox rejects loopback bind. Full node builds
and network/storage tests therefore cannot be claimed as passed. CI is configured
to run them after a commit is pushed from an environment with Git/network access.
