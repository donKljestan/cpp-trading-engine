#!/usr/bin/env bash
# Prepares a fresh Ubuntu 24.04 instance to build and run the recorder.
# Safe to re-run.

set -euo pipefail

echo "installing packages ..."
sudo apt-get update -qq
sudo apt-get install -y build-essential cmake libssl-dev chrony

# Exchange timestamps are only comparable against a disciplined clock. The engine
# measures its own offset anyway, but logs and captures are far easier to reason about
# when the wall clock is right.
echo "enabling time sync ..."
sudo systemctl enable --now chrony
sleep 2
chronyc tracking || true

mkdir -p "$HOME/data"

echo
echo "toolchain:"
g++ --version | head -1
cmake --version | head -1
openssl version

echo
echo "done. next: ./build.sh release && ./build/release/lat_probe 30"
