# Torus RV32 SoC

A 2D torus of RV32IM tiles (CV32E40X) with scratchpads, DMA and a credit-based
two-plane NoC, simulated in Verilator and synthesized in Yosys. The design,
milestones and verification plan are in [SPEC.md](SPEC.md).

Status: M4 done. M3: both NoC planes (NoC0 east/south, NoC1 west/north) with an I/O row, every
flit on the plane with fewer hops; programs are loaded and launched from C++ (libtorus) through
the I/O tile's AXI port; memcpy and Cannon GEMM on 4x4 checked against the performance model.
M4 (8x8) done: memcpy, Cannon, stencil and all-reduce run bit-exact on 8x8 (multithreaded
model), the performance model simulates the network flit by flit (cycle-exact against the RTL).
Next: place and route (M5), FPGA (M6), compute engine (M7).

## Topology

<p align="center">
  <img src="docs/img/torus_fold.svg" width="520"
       alt="An 8x8 torus with its I/O row folding once per dimension and interleaving into its physical layout">
</p>

The NoC is a 2D torus of NX x NY compute tiles plus an I/O row at `y = NY`,
which holds the I/O tile at `(0, NY)` and router-only stops. Laid out in logical
order, every wraparound link would span the whole die. Each ring is instead
folded once and its two layers interleaved. The I/O row is first rotated to the
top die edge, where the pads are, and stays there while the compute rows fold
onto each other exactly like the columns. At 8x8 that gives the physical column
order `0 7 1 6 2 5 3 4` and row order `8 0 7 1 6 2 5 3 4`, so every link,
wraparound included, spans at most two tile pitches. The fold is a placement
decision only: the RTL is the same, and the floorplan (ASIC) or pblocks (FPGA)
apply it. With the optional buffer columns (`x = NX/2` and `x = NX + 1`) both
land on the left and right edges:
[docs/img/torus_fold_buffers.svg](docs/img/torus_fold_buffers.svg).

Regenerate the animations after a topology change (the script checks the
two-pitch link bound):

```sh
python3 scripts/gen_fold_svg.py --nx 8 --ny 8 -o docs/img/torus_fold.svg
python3 scripts/gen_fold_svg.py --nx 8 --ny 8 --buffer-cols -o docs/img/torus_fold_buffers.svg
python3 scripts/gen_fold_svg.py --nx 8 --ny 8 --frame 14.5 -o final.svg   # one static frame
```

## Quick start

Everything runs in the pinned tool image (tools/versions.env); Docker is the
only host requirement.

```sh
git clone --recursive <repo> && cd torus-rv32-SoC
tools/dev.sh scripts/regress.py          # build everything, run the regression
```

`tools/dev.sh <cmd>` runs a command in the image with the repository at /work.
It pulls `ghcr.io/<owner>/torus-tools:$(tools/image_tag.sh)`; CI builds and
pushes that image whenever versions.env, the Dockerfile or the installer
change. Without the registry image it falls back to a local build:

```sh
source tools/versions.env
docker build -f docker/Dockerfile --build-arg BASE_IMAGE="$BASE_IMAGE" -t torus-tools:local .
```

On a Linux host without Docker, Podman or Apptainer run the same image
(`apptainer exec docker://ghcr.io/<owner>/torus-tools:<tag> ...`). There is no
native install path: one image keeps every run on identical tools.

The first build compiles Spike from `third_party/spike` (about 5 minutes); later
builds reuse it while the submodule commit is unchanged.

## Common tasks

```sh
tools/dev.sh cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
tools/dev.sh ninja -C build

# Run a program on every tile of a 1x1, 2x2 or 4x4 grid (plus the I/O row),
# loaded through the I/O tile, Spike in lockstep on every core
tools/dev.sh build/tb/soc/tb_soc_1x1 build/sw/hello.elf
tools/dev.sh build/tb/soc/tb_soc_1x1 build/sw/isa/rv32ui-add.elf --rvfi-trace
tools/dev.sh build/tb/soc/tb_soc_4x4 build/sw/hello.elf --axi-half=10   # slower AXI clock

# Scratchpad size: 32 KB per tile by default; -DTORUS_SPM_KB=64 builds every
# model with 64 KB (the regression also builds the 4x4 SoC at 64 KB:
# tb_soc_4x4_spm64, host_memcpy_4x4_spm64, host_cannon_4x4_spm64)

# Waveforms: compiled in only on request (a separate build directory keeps the
# fast models), then --trace=FILE writes an FST
tools/dev.sh cmake -S . -B build-trace -G Ninja -DCMAKE_BUILD_TYPE=Release -DTORUS_TRACE=ON
tools/dev.sh ninja -C build-trace tb_soc_2x2
tools/dev.sh build-trace/tb/soc/tb_soc_2x2 build-trace/sw/nocops.elf --trace=build/nocops.fst

# memcpy from C++ (libtorus), checked against the performance model
tools/dev.sh model/perf_model.py run-memcpy --host build/tb/soc/host_memcpy_4x4 \
    --elf build/sw/memcpy.elf --shift=1,1        # report in build/reports/
# (--shift=-1,-1 runs on NoC1; host_memcpy --events=E0,..,E7 prints other counter events)

# Cannon GEMM (int32, N=64 on 4x4), checked bit-exact and against the model
# calibrated on a 1x1 run; --plane=noc0 puts all kernel traffic on one plane
tools/dev.sh model/perf_model.py run-cannon --host build/tb/soc/host_cannon_4x4 \
    --cal-host build/tb/soc/host_cannon_1x1 --elf build/sw/cannon.elf --n 64
tools/dev.sh model/perf_model.py planes      # one plane against two: build/reports/planes.md

# Stencil (calibrated on 1x1) and all-reduce (calibrated on 2x2), any grid incl. 8x8
tools/dev.sh model/perf_model.py run-kernel --kernel stencil --host build/tb/soc/host_stencil_8x8 \
    --cal-host build/tb/soc/host_stencil_1x1 --elf build/sw/stencil.elf
tools/dev.sh model/perf_model.py run-kernel --kernel reduce --host build/tb/soc/host_reduce_4x4 \
    --cal-host build/tb/soc/host_reduce_2x2 --elf build/sw/reduce.elf --len 1024

# Performance model v1: the network flit by flit, checked cycle-exact against the RTL
tools/dev.sh model/noc_sim.py check-rtl --tb build/tb/unit/tb_noc_4x5_p01_s0_io --shift=2,2

# Kernel load through the resident loader's software tree (any host program)
tools/dev.sh build/tb/soc/host_cannon_8x8 build/sw/cannon.elf --n=128 --load=tree

# Regression: replay a run or one test from the printed seeds
tools/dev.sh scripts/regress.py --seed 1234
tools/dev.sh scripts/regress.py --long --no-build     # adds the 8x8 kernel suite, Cannon N=128, planes
tools/dev.sh scripts/regress.py -k isa --no-build
tools/dev.sh build/tb/unit/tb_spm_32 --seed=42

# Area report (Yosys generic cells, SRAM as macros)
tools/dev.sh scripts/regress.py -k synth --synth --no-build

# Place and route a compute tile in sky130hd (OpenROAD-flow-scripts, about an
# hour): area, timing and power report in build/pnr/tile_<variant>_report.txt
tools/dev.sh scripts/pnr.py tile --period 20 --floorplan cross
tools/dev.sh scripts/pnr.py tile --period 15 --variant p15 --to floorplan

# The 4x4 grid with the tile as a hard macro, in logical or folded order (links
# through the tiles' feedthroughs), to global routing: link lengths and timing
# in build/pnr/grid_<order>_s0_report.txt, layout with links in the .png.
# One grid run at a time: they need up to 22 GB.
tools/dev.sh scripts/pnr.py tile --period 20 --variant ft --floorplan cross --to cts
tools/dev.sh scripts/pnr.py tile --variant ft --floorplan cross --abstract --set ABSTRACT_SOURCE=4_1_cts
tools/dev.sh scripts/pnr.py grid --order folded
```

Waveforms are FST files; open them on the host with Surfer or GTKWave.

## Layout

| Path | Contents |
| --- | --- |
| `rtl/` | SystemVerilog: `pkg`, `core` (CV32E40X submodule + wrapper), `tile`, `noc`, `perf`, `special` (I/O tile), `soc` (grid generator), file lists (`*.f`) |
| `tb/` | Verilator testbenches: `unit` (block tests), `soc` (SoC host, tb_soc, host programs), `models` (C++ golden models, Spike lockstep, AXI model), `common` |
| `sw/` | `sdk` (crt0, linker script, tile.h, exit sequence), `kernels`, `isa` (riscv-tests environment), `runtime` (libtorus host library) |
| `model/` | `perf_model.py` (performance model) |
| `scripts/` | `regress.py`, `synth_area.py`, `pnr.py` (place and route), `plot_layout.py` (layout images), `gen_fold_svg.py` (topology animation) |
| `pnr/` | Place-and-route designs (ORFS config, constraints, pin sides) and the shared power grid and source reading |
| `tools/`, `docker/` | Pinned versions, image, native setup, `dev.sh` |
| `third_party/` | Spike and riscv-tests submodules |

## Notes for kernel writers

- A program ends by returning from `kernel_main` (or calling `exit`): the exit
  sequence writes the exit word at `0x7FFC` and increments DONE in the I/O
  tile, which is what the host waits for. Args from the host are in the
  mailbox at `0x7F00` (`kernel_main`'s argument).
- The program region is `0x0000`–`0x67FF` at both scratchpad sizes: code,
  data, `.bss`, then the stack (2 KB, `__stack_size` in sw/sdk/link.ld), so the
  program occupies `[0, tile_program_end())`. Buffers the host places go above
  it, or at `0x8000` and up on 64 KB scratchpads (`tile_spm_bytes()`); check
  them with `tile_buffer_ok`. Sync words live at `0x7400`–`0x7EFF`.
- `io_xy()`, `grid_nx()`, `grid_ny()` come from the tile's `GRID` register; the
  I/O row (`y = NY`) is not a compute row.

- Build for `rv32im_zicsr`; the core also implements Zca, but code never uses it.
- `mtvec` keeps only bits [31:7] on CV32E40X: trap entries must be 128-byte aligned (crt0 does this).
- Data bus errors (for example an unmapped MMIO address) arrive as an NMI, `mcause = 0x80000400` (load) or `0x80000401` (store), after the faulting access retires.
