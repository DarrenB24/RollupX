#!/usr/bin/env bash
set -euo pipefail

# ubuntu-24.04 runners also include third-party feeds. These dependencies all
# come from Ubuntu; an unrelated feed outage must not block the build.
ubuntu_sources=/etc/apt/sources.list.d/ubuntu.sources
if [[ ! -s "$ubuntu_sources" ]]; then
  echo "Missing Ubuntu package sources: $ubuntu_sources" >&2
  exit 1
fi
apt_options=(-o "Dir::Etc::sourcelist=$ubuntu_sources" -o 'Dir::Etc::sourceparts=-' -o Acquire::Retries=3)
sudo apt-get "${apt_options[@]}" update
sudo apt-get "${apt_options[@]}" install -y "$@"
