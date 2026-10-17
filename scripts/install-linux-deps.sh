#!/usr/bin/env bash
set -euo pipefail
# Ubuntu 24.04 / Linux Mint 22.x. Run as root (CI uses sudo).
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build python3 clang-format-18 \
  qt6-base-dev qt6-base-dev-tools qt6-qpa-plugins \
  libocct-foundation-dev libocct-modeling-data-dev libocct-modeling-algorithms-dev \
  libocct-visualization-dev libocct-data-exchange-dev libtbb-dev libfontconfig-dev \
  xvfb xauth libgl1-mesa-dri \
  dpkg-dev file ca-certificates
