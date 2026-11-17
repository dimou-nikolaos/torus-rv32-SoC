#!/usr/bin/env bash
# Prints the tool image tag: a hash of everything that defines the image.
# CI pushes ghcr.io/<owner>/torus-tools:<tag>; developers pull the same tag.
set -euo pipefail
cd "$(dirname "$0")/.."
cat tools/versions.env tools/install_pinned.sh docker/Dockerfile | sha256sum | cut -c1-16
