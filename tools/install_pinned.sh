#!/usr/bin/env bash
# Installs the pinned OSS CAD Suite, RISC-V GCC, Python venv and sky130 PDK into a prefix
# (OpenROAD and the ORFS flow come from tools/build_openroad.sh in another stage).
# Used by docker/Dockerfile.
#   usage: install_pinned.sh <versions.env> <prefix>
set -euo pipefail

versions="$1"
prefix="$2"
# shellcheck disable=SC1090
source "$versions"

mkdir -p "$prefix"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fetch() { # url sha256 dest
    curl -fL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c -
}

# OSS CAD Suite
date_compact="${OSS_CAD_SUITE_DATE//-/}"
fetch "https://github.com/YosysHQ/oss-cad-suite-build/releases/download/${OSS_CAD_SUITE_DATE}/oss-cad-suite-linux-x64-${date_compact}.tgz" \
      "$OSS_CAD_SUITE_SHA256" "$tmp/oss.tgz"
rm -rf "$prefix/oss-cad-suite"
tar -xzf "$tmp/oss.tgz" -C "$prefix"

# RISC-V GCC
fetch "https://github.com/xpack-dev-tools/riscv-none-elf-gcc-xpack/releases/download/v${RISCV_GCC_VERSION}/xpack-riscv-none-elf-gcc-${RISCV_GCC_VERSION}-linux-x64.tar.gz" \
      "$RISCV_GCC_SHA256" "$tmp/gcc.tgz"
rm -rf "$prefix/riscv-gcc"
mkdir -p "$prefix/riscv-gcc"
tar -xzf "$tmp/gcc.tgz" -C "$prefix/riscv-gcc" --strip-components=1

# Python venv
rm -rf "$prefix/venv"
python3 -m venv "$prefix/venv"
"$prefix/venv/bin/pip" install --no-cache-dir --upgrade pip
"$prefix/venv/bin/pip" install --no-cache-dir \
    "pytest==${PY_PYTEST}" "pyelftools==${PY_PYELFTOOLS}" "numpy==${PY_NUMPY}" \
    "matplotlib==${PY_MATPLOTLIB}" "pyyaml==${PY_PYYAML}" "ciel==${PY_CIEL}"

# sky130 PDK: standard cells and SRAM macros
rm -rf "$prefix/pdk"
"$prefix/venv/bin/ciel" enable --pdk-family sky130 --pdk-root "$prefix/pdk" \
    -l sky130_fd_sc_hd -l sky130_sram_macros \
    "$SKY130_OPEN_PDKS"

# Environment script. The venv goes before OSS CAD Suite, which ships its own python3.
cat > "$prefix/env.sh" <<ENV
export TORUS_TOOLS="$prefix"
export PDK_ROOT="$prefix/pdk"
export PDK=sky130A
export ORFS_FLOW="$prefix/orfs/flow"
export PATH="$prefix/venv/bin:$prefix/riscv-gcc/bin:$prefix/oss-cad-suite/bin:$prefix/openroad/bin:\$PATH"
ENV
echo "Installed pinned tools into $prefix; source $prefix/env.sh"
