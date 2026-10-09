# Benchmark protocol

No throughput, latency, failover, or recovery result has been measured yet.
Networking smoke tests are not transaction benchmarks.

The Phase 4 C++ load generator will sign funded-account workloads, submit with
bounded concurrency, follow leader hints, and poll receipts until **durably
applied**. A timeout is not evidence of rejection; resolve IDs before calculating
final counts. Measure committed successes per wall-clock interval, not admissions.
Latency begins before submission and ends at durable receipt confirmation.
Report retries, invalid/rejected transactions, unresolved IDs, warmup, and drain
separately. Do not omit failed requests to improve apparent percentiles.

Sweep batch sizes 1 (batching disabled), 50, 100, 250, 500, and 1000 at fixed load
and client concurrency; repeat each configuration. Report sample count, arithmetic
mean, p50/p95/p99 (document percentile method), successful TPS, queue depth,
replication lag, disk writes, CPU, RSS, and failures. Capture election time from
fault injection to observed stable leadership, and recovery to applied-root
convergence. Failure schedules and artificial delay are explicit run inputs.

Versioned JSON plus per-run and per-transaction CSV artifacts must include commit
SHA, OS/kernel, CPU/core count, RAM, disks/filesystem/fsync settings, dependency
versions, compiler/build flags, node placement, RTT, transaction bytes, account
distribution, mix, rate, duration, warmup/drain limits, batch size/timeout, and seed.
Different payload sizes initially require separately defined non-semantic padding;
padding must never change the signed encoding or be silently benchmarked as work.

Goals: 10,000 committed TPS, p95 below 100 ms, leadership recovery below 1 s,
restart recovery below 3 s. These are targets, not promises or prefilled results.
