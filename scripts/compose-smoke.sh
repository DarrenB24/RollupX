#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
# Preserve named volumes. To reset test state deliberately: docker compose down -v.
trap 'docker compose down' EXIT
docker compose up --build -d --wait --wait-timeout 120
await_mesh() {
  local deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    local ready=1
    for i in 1 2 3 4 5; do
      docker compose exec -T "node$i" rollupx-cli ready 127.0.0.1:50051 >/dev/null 2>&1 || ready=0
    done
    [[ $ready == 0 ]] || return 0
    sleep 0.2
  done
  docker compose logs >&2
  return 1
}
await_mesh
docker compose kill -s SIGKILL node3
docker compose up -d node3
await_mesh
for i in 1 2 3 4 5; do
  docker compose exec -T "node$i" rollupx-cli status 127.0.0.1:50051
done
echo "PASS Docker transport connectivity and restart (no consensus tested)"
