#!/usr/bin/env bash
set -euo pipefail
ledger=${1:?path to rollupx-ledger required}
wallet=${2:?path to rollupx-wallet required}
work=$(mktemp -d "${TMPDIR:-/tmp}/rollupx-ledger.XXXXXX")
trap 'rm -rf "$work"' EXIT
sender=$("$wallet" keygen "$work/sender.key")
recipient=$("$wallet" keygen "$work/recipient.key")
printf 'chain 31337\naccount %s 100\naccount %s 0\n' "$sender" "$recipient" > "$work/genesis.txt"
if "$ledger" --standalone "$work/db" "$work/genesis.txt" status >/dev/null 2>&1; then
  echo "query unexpectedly initialized a missing ledger" >&2; exit 1
fi
"$ledger" --standalone "$work/db" "$work/genesis.txt" init
transaction=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 10 0)
first=$("$ledger" --standalone "$work/db" "$work/genesis.txt" submit "$transaction")
retry=$("$ledger" --standalone "$work/db" "$work/genesis.txt" submit "$transaction")
[[ $first == "$retry" && $first == *'result=applied log_index=1 offset=0 durable=true'* ]]
receipt=$("$ledger" --standalone "$work/db" "$work/genesis.txt" receipt "${transaction:0:64}")
[[ $receipt == "$first" ]]
balance=$("$ledger" --standalone "$work/db" "$work/genesis.txt" balance "$sender")
[[ $balance == *'balance=90 nonce=1 applied_index=1'* ]]
batch=$("$ledger" --standalone "$work/db" "$work/genesis.txt" batch 1)
[[ $batch == *"$transaction"* ]]
bad_balance=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 1000 1)
rejected=$("$ledger" --standalone "$work/db" "$work/genesis.txt" submit "$bad_balance")
[[ $rejected == *'result=insufficient_balance log_index=2 offset=0 durable=true'* ]]
balance=$("$ledger" --standalone "$work/db" "$work/genesis.txt" balance "$sender")
[[ $balance == *'balance=90 nonce=1 applied_index=2'* ]]
echo "PASS standalone ledger submission, reopen, durable retries, query and rejection"
