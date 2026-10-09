#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build-foundation"
flags=(-std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread)
if [[ ${SANITIZE:-0} == 1 ]]; then
  flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
"${CXX:-c++}" "${flags[@]}" -I "$root/include" \
  "$root/src/node/config.cpp" "$root/src/metrics/http_server.cpp" \
  "$root/tests/unit/foundation_checks.cpp" -o "$root/build-foundation/foundation_checks"
"$root/build-foundation/foundation_checks" "$@"
