#!/usr/bin/env bash
# Builds the pinned OpenROAD and installs the pinned OpenROAD-flow-scripts flow
# directory (sky130 platforms only) into a prefix. Used by docker/Dockerfile in
# its own stage, whose result is copied into the tool image.
#   usage: build_openroad.sh <versions.env> <prefix>
set -euo pipefail

versions="$1"
prefix="$2"
# shellcheck disable=SC1090
source "$versions"

jobs="${OPENROAD_JOBS:-$(nproc)}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# bazelisk; it fetches the Bazel version OpenROAD pins in .bazelversion.
curl -fL --retry 3 -o /usr/local/bin/bazelisk \
    "https://github.com/bazelbuild/bazelisk/releases/download/v${BAZELISK_VERSION}/bazelisk-linux-amd64"
echo "${BAZELISK_SHA256}  /usr/local/bin/bazelisk" | sha256sum -c -
chmod +x /usr/local/bin/bazelisk

checkout() { # url commit dir
    git init -q "$3"
    git -C "$3" remote add origin "$1"   # submodule URLs are relative to it
    git -C "$3" fetch -q --depth 1 origin "$2"
    git -C "$3" checkout -q FETCH_HEAD
}

# ORFS; its OpenROAD submodule pointer must match the pin.
checkout https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts.git "$ORFS_COMMIT" "$tmp/orfs"
or_commit=$(git -C "$tmp/orfs" rev-parse HEAD:tools/OpenROAD)
[ "$or_commit" = "$OPENROAD_COMMIT" ] || { echo "ORFS pins OpenROAD ${or_commit}, versions.env ${OPENROAD_COMMIT}" >&2; exit 1; }

rm -rf "$prefix/orfs"
mkdir -p "$prefix/orfs/flow/platforms"
(cd "$tmp/orfs/flow" && cp -a Makefile scripts util "$prefix/orfs/flow/" \
    && cp -a platforms/common platforms/sky130hd "$prefix/orfs/flow/platforms/")
echo "$ORFS_COMMIT" > "$prefix/orfs/COMMIT"

# OpenROAD, command line only (no GUI).
checkout https://github.com/The-OpenROAD-Project/OpenROAD.git "$OPENROAD_COMMIT" "$tmp/openroad"
git -C "$tmp/openroad" submodule update -q --init --recursive --depth 1
rm -rf "$prefix/openroad"
(cd "$tmp/openroad" && ./etc/Build.sh -no-gui -threads="$jobs" -prefix="$prefix/openroad")
"$prefix/openroad/bin/openroad" -version
