#!/usr/bin/env bash
set -euo pipefail
node=${1:?path to rollupx-node required}
cli=${2:?path to rollupx-cli required}
base=${ROLLUPX_SMOKE_BASE_PORT:-24000}
work=$(mktemp -d "${TMPDIR:-/tmp}/rollupx-mesh.XXXXXX")
pids=()
cleanup() {
  local result=$?
  trap - EXIT
  for pid in "${pids[@]}"; do [[ -z $pid ]] || kill -TERM "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]}"; do [[ -z $pid ]] || wait "$pid" 2>/dev/null || true; done
  if [[ $result != 0 ]]; then
    for log in "$work"/*.log; do cat "$log" >&2; done
  fi
  rm -rf "$work"
  exit "$result"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
peers=""
for i in 1 2 3 4 5; do
  [[ -z $peers ]] || peers+=","
  peers+="n$i=127.0.0.1:$((base + i * 10 + 1))"
done
start_node() {
  local i=$1
  "$node" --id "n$i" --data-dir "$work/n$i" --peers "$peers" \
    --public-listen "127.0.0.1:$((base + i * 10))" \
    --internal-listen "127.0.0.1:$((base + i * 10 + 1))" \
    --http-listen "127.0.0.1:$((base + i * 10 + 2))" >> "$work/n$i.log" 2>&1 &
  pids[$((i - 1))]=$!
}
await_mesh() {
  local deadline=$((SECONDS + 25))
  while ((SECONDS < deadline)); do
    local ready=1
    for i in 1 2 3 4 5; do
      kill -0 "${pids[$((i - 1))]}" 2>/dev/null || { echo "n$i exited" >&2; return 1; }
      "$cli" ready "127.0.0.1:$((base + i * 10))" >/dev/null 2>&1 || ready=0
    done
    [[ $ready == 0 ]] || return 0
    sleep 0.1
  done
  echo "mesh did not become ready before deadline" >&2; return 1
}
for i in 1 2 3 4 5; do start_node "$i"; done
await_mesh
for i in 1 2 3 4 5; do
  "$cli" status "127.0.0.1:$((base + i * 10))"
  curl --fail --silent --max-time 2 "http://127.0.0.1:$((base + i * 10 + 2))/healthz" >/dev/null
done
# Real process crash/restart verifies identity-store reopening and reconnect only.
# This is NOT a consensus, transaction durability, or failover test.
kill -KILL "${pids[2]}"
wait "${pids[2]}" 2>/dev/null || true
pids[2]=""
deadline=$((SECONDS + 5))
while "$cli" ready "127.0.0.1:$((base + 10))" >/dev/null 2>&1; do
  ((SECONDS < deadline)) || { echo "dead peer never became stale" >&2; exit 1; }
  sleep 0.1
done
start_node 3
await_mesh
echo "PASS five-process transport mesh, crash detection, and restart"
