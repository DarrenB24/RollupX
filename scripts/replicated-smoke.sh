#!/usr/bin/env bash
set -euo pipefail
node=${1:?path to rollupx-node required}
cli=${2:?path to rollupx-cli required}
wallet=${3:?path to rollupx-wallet required}
base=${ROLLUPX_REPLICATED_BASE_PORT:-25000}
work=$(mktemp -d "${TMPDIR:-/tmp}/rollupx-replicated.XXXXXX")
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
sender=$("$wallet" keygen "$work/sender.key")
recipient=$("$wallet" keygen "$work/recipient.key")
printf 'chain 31337\naccount %s 100\naccount %s 0\n' "$sender" "$recipient" > "$work/genesis.txt"
peers=""; public_peers=""
for i in 1 2 3 4 5; do
  [[ -z $peers ]] || { peers+=","; public_peers+=","; }
  peers+="n$i=127.0.0.1:$((base+i*10+1))"
  public_peers+="n$i=127.0.0.1:$((base+i*10))"
done
endpoint() { printf '127.0.0.1:%s' "$((base+$1*10))"; }
start_node() {
  local i=$1
  "$node" --id "n$i" --data-dir "$work/n$i" --peers "$peers" \
    --genesis "$work/genesis.txt" --public-peers "$public_peers" \
    --public-listen "$(endpoint "$i")" \
    --internal-listen "127.0.0.1:$((base+i*10+1))" \
    --http-listen "127.0.0.1:$((base+i*10+2))" >> "$work/n$i.log" 2>&1 &
  pids[$((i-1))]=$!
}
crash_node() {
  local i=$1
  kill -KILL "${pids[$((i-1))]}"
  wait "${pids[$((i-1))]}" 2>/dev/null || true
  pids[$((i-1))]=""
}
await_leader() {
  local deadline=$((SECONDS+25))
  while ((SECONDS < deadline)); do
    for i in 1 2 3 4 5; do
      [[ -n ${pids[$((i-1))]} ]] || continue
      kill -0 "${pids[$((i-1))]}" 2>/dev/null || { echo "n$i exited" >&2; return 1; }
      if "$cli" ready "$(endpoint "$i")" >/dev/null 2>&1; then leader=$i; return 0; fi
    done
    sleep 0.1
  done
  echo "leader readiness timeout" >&2; return 1
}
await_receipts() {
  local transaction=$1 expected=$2 deadline=$((SECONDS+25))
  while ((SECONDS < deadline)); do
    local ready=1
    for i in 1 2 3 4 5; do
      [[ -n ${pids[$((i-1))]} ]] || continue
      local receipt
      receipt=$("$cli" receipt "$(endpoint "$i")" "${transaction:0:64}" 2>/dev/null) || ready=0
      [[ $receipt == "$expected" ]] || ready=0
    done
    [[ $ready == 0 ]] || return 0
    sleep 0.1
  done
  echo "replicated receipt timeout" >&2; return 1
}
check_accounts() {
  local balance=$1 nonce=$2
  for i in 1 2 3 4 5; do
    [[ -n ${pids[$((i-1))]} ]] || continue
    [[ $("$cli" balance "$(endpoint "$i")" "$sender") == "balance=$balance "* ]]
    [[ $("$cli" balance "$(endpoint "$i")" "$recipient") == "balance=$((100-balance)) "* ]]
    [[ $("$cli" nonce "$(endpoint "$i")" "$sender") == "nonce=$nonce "* ]]
  done
}
for i in 1 2 3 4 5; do start_node "$i"; done
await_leader
transaction=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 10 0)
first=$("$cli" submit "$(endpoint "$leader")" "$transaction")
[[ $first == *'state=APPLIED'* ]]
await_receipts "$transaction" "$first"
for i in 1 2 3 4 5; do
  [[ $("$cli" submit "$(endpoint "$i")" "$transaction") == "$first" ]]
done
check_accounts 90 1
# Lose the leader and one follower; the remaining majority must commit.
old_leader=$leader
other=$((leader%5+1))
crash_node "$old_leader"
crash_node "$other"
await_leader
second_tx=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 20 1)
second=$("$cli" submit "$(endpoint "$leader")" "$second_tx")
[[ $second == *'state=APPLIED'* ]]
await_receipts "$second_tx" "$second"
check_accounts 70 2
start_node "$old_leader"
start_node "$other"
await_receipts "$second_tx" "$second"
check_accounts 70 2
# Kill every process after durable application; reopen all five databases.
for i in 1 2 3 4 5; do crash_node "$i"; done
for i in 1 2 3 4 5; do start_node "$i"; done
await_leader
await_receipts "$transaction" "$first"
await_receipts "$second_tx" "$second"
[[ $("$cli" submit "$(endpoint "$leader")" "$second_tx") == "$second" ]]
check_accounts 70 2
echo "PASS five-process signed transfers, idempotent retries, leader crash, majority progress, catch-up and full crash recovery"
