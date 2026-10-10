#!/usr/bin/env bash
set -euo pipefail
wallet=${1:?path to rollupx-wallet required}
wire=${2:?path to rollupx-wire required}
work=$(mktemp -d "${TMPDIR:-/tmp}/rollupx-wire.XXXXXX")
trap 'rm -rf "$work"' EXIT
"$wallet" keygen "$work/sender.key" > /dev/null
recipient=$("$wallet" keygen "$work/recipient.key")
signed=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 42 0)
encoded=$("$wire" encode "$signed")
[[ $("$wire" decode "$encoded") == "$signed" ]]
[[ $("$wire" decode "0x$encoded") == "$signed" ]]
# Protobuf field 4 = amount, appended with a conflicting value (43).
if "$wire" decode "${encoded}202b" > "$work/output" 2>/dev/null; then
  echo "conflicting unsigned amount accepted" >&2; exit 1
fi
[[ ! -s "$work/output" ]]
for malformed in '' '0' 'zz' "${encoded}00"; do
  if "$wire" decode "$malformed" > /dev/null 2>&1; then
    echo "malformed wire bytes accepted" >&2; exit 1
  fi
done
echo "PASS signed wallet/protobuf round-trip and malformed request rejection"
