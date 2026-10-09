#!/usr/bin/env bash
set -euo pipefail
wallet=${1:?path to rollupx-wallet required}
work=$(mktemp -d "${TMPDIR:-/tmp}/rollupx-wallet.XXXXXX")
trap 'rm -rf "$work"' EXIT
sender=$("$wallet" keygen "$work/sender.key")
recipient=$("$wallet" keygen "$work/recipient.key")
[[ $("$wallet" address "$work/sender.key") == "$sender" ]]
signed=$("$wallet" sign "$work/sender.key" 31337 "$recipient" 42 0)
[[ ${#signed} == 348 ]]
"$wallet" verify "$signed" > "$work/verified.txt"
grep -q "sender=$sender recipient=$recipient chain_id=31337 amount=42 nonce=0 signature_valid=true" "$work/verified.txt"
if "$wallet" keygen "$work/sender.key" >/dev/null 2>&1; then
  echo "existing key was overwritten" >&2; exit 1
fi
[[ $("$wallet" address "$work/sender.key") == "$sender" ]]
prefix=00
[[ ${signed:0:2} != 00 ]] || prefix=01
if "$wallet" verify "$prefix${signed:2}" >/dev/null 2>&1; then
  echo "tampered transaction accepted" >&2; exit 1
fi
chmod 644 "$work/sender.key"
if "$wallet" address "$work/sender.key" >/dev/null 2>&1; then
  echo "world-readable secret accepted" >&2; exit 1
fi
ln -s "$work/recipient.key" "$work/symlink.key"
if "$wallet" address "$work/symlink.key" >/dev/null 2>&1; then
  echo "symlink secret accepted" >&2; exit 1
fi
echo "PASS offline key generation, signing, verification, tamper rejection and key-file protections"
