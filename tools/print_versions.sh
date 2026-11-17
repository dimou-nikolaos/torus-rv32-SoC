#!/usr/bin/env bash
# Prints every tool version; reports and CI logs embed this output.
set -uo pipefail
v() { printf '%-14s ' "$1"; shift; "$@" 2>&1 | head -1; }
v verilator   verilator --version
v yosys       yosys -V
v yosys-slang sh -c 'yosys -m slang -p "help read_slang" >/dev/null 2>&1 && echo "plugin loads (bundled with OSS CAD Suite)" || echo "MISSING"'
v sby         sby --version
v gcc         riscv-none-elf-gcc --version
v cmake       cmake --version
v ninja       ninja --version
v python      python3 --version
v pytest      python3 -m pytest --version
v spike       sh -c 'git -C "$(dirname "$0")/../third_party/spike" describe --always --dirty 2>/dev/null || echo "submodule not checked out"' "$0"
v cv32e40x    sh -c 'git -C "$(dirname "$0")/../rtl/core/cv32e40x" rev-parse --short HEAD 2>/dev/null || echo "submodule not checked out"' "$0"
