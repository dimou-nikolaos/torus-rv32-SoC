# Torus RV32 SoC: project setup

Nov 10, 2026 · @Nikolaos Dimou

## Goals and scope

Build a small Wormhole-style dataflow SoC: a 2D torus of RV32IM tiles with local scratchpads and DMA, simulated in Verilator and synthesized in Yosys, plus the software to split a problem across it.

In scope:

- A uniform, parameterized grid of compute tiles (start 4x4, target 8x8), fed through one I/O tile in an extra I/O row on the same NoC. No DRAM tiles: every tile owns its scratchpad.
- Two unidirectional NoC planes (NoC0 east and south, NoC1 west and north), torus wraparound, deadlock-free, with pipelined credit-based routers.
- A kernel SDK for the tiles and a C++ host runtime that loads, launches and waits.
- Workloads sized to fit on chip: ping, DMA copy, Cannon GEMM, 2D stencil, reduction.
- Optional, as parameters: two buffer columns that fold onto the left and right die edges, and extra I/O tiles in the I/O row (later, further row blocks).
- Phase 2: one compute engine per tile (Zve32x or a custom int8 matrix unit).

Out of scope for now: external DRAM, caches and coherence, virtual memory, interrupts beyond a doorbell, multi-chip links, and place-and-route (a late, optional experiment). Problems must fit in on-chip SRAM or be streamed in through the I/O tile.

## Build principles

Verification sits above the RTL: the harness defines what done means, and RTL is accepted only when it passes the harness.

1. Verification is the harness, strict and thorough. Every block lands together with its testbench, scoreboard and golden model. Random tests are seeded and replayable, and the regression gates every merge.
2. Golden models for everything where applicable: Spike for the cores, a transaction-level NoC model, an NIU and DMA model, a C++ reference for every kernel, and the performance model for cycle counts. A model may abstract only where no precision is lost for the property being checked; anything timing-sensitive (router pipeline, credits, arbitration) is checked cycle-accurately.
3. The router is designed from the start for the best ratio of latency, throughput and scalability. It is pipelined, so link and grid size can grow without redesign, and it supports hierarchical flows (cluster-level routing) without changing the interface.
4. Credit-based flow control on every link and plane. Credits are sized to cover the link round trip, so adding pipeline stages or register slices keeps full throughput instead of stalling. The NoC full-throughput rule under Verification checks this.
5. Demonstrable deliverables. Every milestone runs the complete software-to-hardware flow end to end: compile, load, launch, check results. Loading goes through the I/O tile from M2 on; M0 and M1 use a testbench backdoor. First versions are simple (scalar cores, no engine) and are refined and optimized later behind the same interfaces.
6. Performance counters everywhere: per tile (busy, semaphore stalls, bank conflicts, DMA bytes), per router port (flits, credit stalls, arbitration losses), per link (utilization) and in the I/O tile. The host reads them over the NoC, and every run compares them against the performance model to locate stalls and bottlenecks.

## Toolchain and environment

Tools come from OSS CAD Suite, pinned to one dated release, plus the few things it does not ship. One pinned container image runs them everywhere, locally and in CI, so numbers are reproducible.

| Tool | Source | Role | Notes |
| --- | --- | --- | --- |
| Verilator | OSS CAD Suite | Cycle-based simulation | FST tracing, `--threads` for 8x8 grids |
| Yosys + slang frontend | OSS CAD Suite | Synthesis and area checks | Confirm `yosys -m slang` works in the chosen release; sv2v as fallback |
| SymbiYosys | OSS CAD Suite | Formal checks | Optional router properties |
| Surfer or GTKWave | OSS CAD Suite | Waveforms | Run on the host against FST files in the workspace |
| RISC-V GCC | xPack riscv-none-elf, pinned prebuilt release, in the image | Kernel compiler | `-march=rv32im_zicsr -mabi=ilp32`, newlib-nano, later `rv32im_zve32x` |
| Spike | Repo submodule, built by the project | Golden model per core | Linked as a library; scratchpad and MMIO sync through its memory callbacks (no patch needed so far), later custom instructions |
| Python 3 + pytest | Image | Test driver and analysis | Regressions, plots, counter comparison |
| CMake + Ninja | Image | Build | One target per testbench, one per kernel |
| OpenROAD (M6) | Added later | Place-and-route experiment | Logical vs folded tile placement |

Setup:

1. `tools/versions.env` pins the OSS CAD Suite release date, the RISC-V GCC release and the Python package versions.
2. `docker/Dockerfile` builds the image from `versions.env`. CI builds it only when versions.env changes and pushes it to GitHub's container registry, tagged with a hash of that file. CI, developers and co-authors pull that image and never build it locally; it also works as a VS Code devcontainer.
3. Everything runs in the image: `tools/dev.sh <command>` locally, the same image in CI. There is no native install path, so there is one set of tools to keep pinned and verified. Hosts without Docker run the image with Podman or Apptainer; a multi-arch (arm64) image is the route if native-speed Apple Silicon is ever needed.
4. Spike lives in `third_party/spike` as a submodule built by the project, so its patches are versioned with the RTL that needs them.

A GitHub Actions job runs the smoke regression on every push, and every report records the tool versions it ran with.

## Repository layout

One repository, split so RTL, tile software and host software can each be built and tested on their own.

```
torus-rv32-SoC/
  SPEC.md         this document
  rtl/
    pkg/          noc_pkg.sv, tile_pkg.sv (widths, coords, packet types)
    noc/          router.sv (pipelined), credit_port.sv, link_slice.sv
    tile/         tile_top.sv, niu.sv, dma.sv, spm_banks.sv, mailbox.sv
    core/         cv32e40x (git submodule) + wrapper with RVFI tap
    special/      io_tile.sv, io_axi_bridge.sv, io_cdc_fifo.sv, io_fe_jtag_sba.sv, buffer_tile.sv (optional)
    perf/         perf_counter.sv, counter_bank.sv (NoC-readable)
    soc/          soc_top.sv (grid generator, parameters NX, NY, I/O row, buffer columns)
  tb/
    common/       testbench helpers: seeds and options, ELF loader
    models/       golden models: spike_cosim (lockstep), spm_model, later noc_tlm, niu_model, kernel references
    unit/         block testbenches (C++ driver + scoreboard): spm, later router, link, NIU
    soc/          tb_tile (M0), later tb_soc.cpp, axi_bfm.cpp (DPI)
  sw/
    sdk/          crt0.S, link.ld, tile.h (DMA, semaphores, coords)
    kernels/      hello, regtest, later ping, memcpy, cannon_gemm, stencil2d, reduce
    isa/          riscv-tests environment for a tile (rv32ui, rv32um)
    runtime/      libtorus (C++ host API) + examples
  model/          perf_model.py (roofline + capacity check)
  syn/            yosys scripts, area reports per module
  fpga/           optional pblock constraints in folded order
  scripts/        regress.py, synth_area.py, later plot_noc.py, compare_counters.py
  cmake/          Verilator testbench and tile-program helpers
  docker/         Dockerfile built from tools/versions.env
  tools/          versions.env, install_pinned.sh, dev.sh (run in the image), image_tag.sh
  third_party/    spike, riscv-tests (submodules; Spike built by the project)
  docs/           detailed notes that outgrow SPEC.md (protocol tables, register maps)
```

## Baseline architecture

Start with a full 4x4 grid of compute tiles on one clock, plus one extra I/O row. Everything is parameterized by `NX` and `NY`, so 8x8 is a parameter change, and the optional parts are parameters too.

The router grid is `GX` x `GY` with `GX = NX` (plus 2 with buffer columns) and `GY = NY + 1`. Compute tiles sit at x in `[0, NX)`, y in `[0, NY)`. The I/O row is logical row `y = NY`: the I/O tile at `(0, NY)`, and router-only stops (router, no NIU) at the other positions, reserved for extra I/O tiles or other row blocks later. The I/O row is part of the Y rings, so every compute tile has a full NX x NY torus of compute peers, and a Y wraparound costs one extra pass-through hop. Tile coordinates on the NoC are always router coordinates. The SDK's `compute_grid()` and `neighbor()` skip non-compute positions, so kernels see a plain NX x NY torus.

| Parameter | Baseline | Notes |
| --- | --- | --- |
| Grid | 4x4 compute, target 8x8; router grid NX x (NY+1) | Uniform compute tiles, no DRAM tiles; the extra row is the I/O row |
| I/O tile | One, at logical (0, NY) in the I/O row | AXI slave front-end, async FIFO, bridge to NIU; driven by a C++ AXI model in Verilator and by QDMA on FPGA (see I/O tile) |
| Core | RV32IM, CV32E40X | Decided. RV32IM configuration (no A, C or B), RVFI for Spike lockstep, CV-X-IF reserved for M5 |
| Scratchpad | 64 KB, 4 banks x 128 bit | Single-cycle; banks interleaved on address bits `[5:4]`; seven-port arbiter per bank with a starvation bound (see Scratchpad bank arbiter) |
| DMA | 2 channels per tile | Channel 0 for reads (READ\_REQ), channel 1 for writes, so double buffering works |
| Router | One 3x3 router per plane, pipelined, dimension-ordered, single-flit packets | Ports: local, X ring, Y ring (see Router). Credit-based flow control, credits sized to the link round trip |
| NoC | 2 unidirectional planes | NoC0 east and south, NoC1 west and north, as in Wormhole |
| Link | 204-bit flit per plane and direction, plus valid, the 2-bit lookahead port and the credit return | Optional register slices, depth 0 to 2 |
| Optional: buffer columns | Two columns half an X ring apart: logical `x = NX/2` and `x = NX + 1`, so `GX = NX + 2` | NIU plus large SRAM, no core. Being half a ring apart, they fold onto the physical left and right edges. Compute columns become non-contiguous in router coordinates; the SDK hides this |
| Optional: extra I/O tiles | Further positions in the I/O row | The I/O row folds onto a die edge, where pads are. A second row block half a Y ring away would fold onto the opposite edge |

Compute tile (the engine and its links arrive in M5):

```
                NoC0 in (W, N)      NoC1 in (E, S)
                      |                   |
             +--------v--------+ +--------v--------+
             |  router NoC0    | |  router NoC1    |
             +--------+--------+ +--------+--------+
                      |  inject / eject   |
             +--------v-------------------v--------+
  NIU regs <-+ NIU: DMA x2, TX-DMA, TX-reply,       |
 (core only) |  RX0, RX1, READ_REQ queue, atomic    |
             +---+---------+---------+---------+----+
                 | TX-DMA  | TX-rep  | RX0     | RX1
+----------+  I  +-v---------v---------v---------v-+    +- - - - - - - -+
| CV32E40X +---->+ bank arbiter x4 (7 ports)       +<---+ engine (M5)   |
| (RV32IM) +---->+ 4 banks x 128 bit, 64 KB        |    | CV-X-IF ctrl  |
+----+-----+  D  +---------------------------------+    +- - - - - - - -+
     |
     +--> tile control, counter bank, console (MMIO)
```

The core never touches the network directly: it writes NIU registers and reads its own scratchpad, while the NIU moves data in and out of the same banks.

The fold is never in the RTL. The same netlist is placed in folded order by the ASIC floorplan, or by pblocks on FPGA, and on FPGA only when timing closure needs it. A folded ring of length L puts logical positions `r` and `r + L/2` on the two edges for any rotation `r`. In Y, the I/O row is placed on a die edge and the compute rows fold onto each other like the columns, giving the row order `NY, 0, NY-1, 1, …` (`8 0 7 1 6 2 5 3 4` at 8x8); with buffer columns, the X rotation puts both of them on the left and right edges. If tiles are later grouped into clusters, the cluster is the unit of the fold. `scripts/gen_fold_svg.py` draws the fold for the README (`docs/img/torus_fold*.svg`) and checks the two-pitch link bound; regenerate it whenever the grid parameters change.

### Router

Each plane has its own router per grid position: one 3x3 module, instantiated twice with direction parameters (NoC0: in from W and N, out to E and S; NoC1: in from E and S, out to W and N). The planes never exchange flits, so nothing is shared between them, and the module is verified once.

| Input | Allowed outputs (X-first dimension-ordered routing) |
| --- | --- |
| X ring | X ring (continue), Y ring (turn, bubble rule applies), local |
| Y ring | Y ring (continue), local |
| Local | X ring or Y ring (injection, bubble rule applies), or local (loopback) |

Together with local to local (the NIU loopback), that is 8 of the 9 crossbar paths; only Y to X is illegal, and the router asserts it never occurs. Router-only stops in the I/O row use the same module with the local port tied off. Every port is a generic credit port (flit, valid, credit return), so a cluster-level router or an express link can attach later without changing the interface. Two pipeline stages per hop with lookahead routing (see Open decisions): each router computes the output port the flit will take at the next router, so allocation never waits for route computation.

### Scratchpad bank arbiter

Each of the 4 banks has its own arbiter with seven master ports, all present from M0 onward even while only the core is connected, so M1 and M5 add masters without changing the arbiter:

| Port | Master | Width | Access |
| --- | --- | --- | --- |
| 0 | Engine (M5) | 128 bit | Read and write |
| 1 | NIU RX0 (ejection, NoC0) | 128 bit | Write, read-modify-write for ATOMIC\_INC |
| 2 | NIU RX1 (ejection, NoC1) | 128 bit | Write, read-modify-write for ATOMIC\_INC |
| 3 | NIU TX-DMA (DMA WRITE source) | 128 bit | Read |
| 4 | NIU TX-reply (READ\_REQ replies) | 128 bit | Read |
| 5 | Core data (OBI) | 32 bit | Read and write, byte enables |
| 6 | Core instruction (OBI) | 32 bit | Read |

Separate RX ports per plane plus separate TX ports for DMA writes and replies let the NIU eject on both planes, stream its own writes and serve remote reads at the same time, each at one flit per cycle. Sequential streams spread over the banks (16-byte interleave), so concurrent streams collide only briefly before they fall out of phase.

Arbitration is priority with a starvation bound:

- Default order: engine, then the NIU ports (round-robin among RX0, RX1, TX-DMA and TX-reply), then core data, then core instruction.
- Every port has an age counter that counts consecutive denied cycles on a pending request. When it reaches `STARVE_LIMIT` (parameter, default 4), the port is promoted above all default priorities; several promoted ports are served round-robin.
- Guarantee: any request is granted within a fixed bound: `STARVE_LIMIT` cycles plus one round-robin turn over the other promoted ports, with an atomic counting as two cycles. The arbiter model derives the exact bound from the parameters and checks it, a simulation assertion enforces it, and the counter bank records conflicts and promotions per port.
- A read-modify-write for ATOMIC\_INC locks its bank for the two cycles of the operation, so no other master writes in between.

This bound is what makes NIU ejection always drain: writes and atomics wait only on the local bank arbiter, never on the network (see NIU structure).

### Compute engine attachment (M5)

The engine connects to the core through CV-X-IF for control only, and to the scratchpad through its own wide port for data. CV-X-IF can route coprocessor loads and stores through the core's LSU, but that path is 32-bit and one access at a time, which would starve any engine worth adding. The spec allows bypassing it (Vicuna does), and this tile does.

| Path | Carries | Interface |
| --- | --- | --- |
| Control | Custom instructions: start with buffer addresses and sizes, status and result readback | CV-X-IF issue, register, commit and result interfaces; the CV-X-IF memory interface stays unused |
| Data | Operand and result streams | Port 0 of each scratchpad bank arbiter, 128 bit |

Consistency is a software contract, the same as for DMA buffers: the core does not touch a buffer the engine owns until it reads the engine's done status (or a semaphore the engine increments). No hardware interlock in the first version; add one only if the contract proves error-prone. The engine's own golden model checks results bit-exact, and Spike gets a model of the custom instructions so lockstep co-simulation keeps working.

### I/O tile

The I/O tile is an NIU, a bridge, a front-end and a small register block. The register block holds the DONE counters and host-side semaphores that kernels increment with ATOMIC\_INC; the host reads it directly over AXI. The AXI slave port is the real top-level boundary of the design in every flow: simulation, FPGA and ASIC studies.

| Part | Function |
| --- | --- |
| Front-end | Selected by parameter: AXI slave (default), JTAG debug module SBA, SPI slave |
| Clock crossing | Async FIFO between the front-end clock and the fabric clock; the only CDC point in the design |
| Bridge | Maps a global address (tile x, tile y, local address) to NoC packets: AXI bursts split into 128-bit WRITE flits (AXI byte strobes become `be`, so the host may write unaligned), reads become READ\_REQ round trips with at most `IO_RD_OUTSTANDING` in flight, with backpressure to the front-end |
| NIU | The same NIU as in compute tiles |

How each flow drives it:

| Flow | Host side | Notes |
| --- | --- | --- |
| Verilator | C++ AXI bus functional model called through DPI | The regression verifies the exact RTL that the FPGA host talks to |
| FPGA | PCIe with QDMA, AXI memory-mapped | QDMA clock (typically 250 MHz) crosses the async FIFO; 512-bit data is split into flits |
| FPGA bring-up | JTAG debug module, system bus access | A few hundred KB/s: fine for load, run, read back; not for streaming |
| ASIC study | AXI ports left as top-level pins | Area and timing reported for fabric + I/O tile, excluding PCIe controller and PHY |
| Tapeout (if ever) | JTAG SBA or SPI front-end | AXI needs far more pins than a shuttle-class chip has |

Verification: the AXI model and bridge get their own testbench (bursts, byte strobes, backpressure, read ordering) against a golden memory model, and the CDC FIFO is checked with randomized clock ratios. One small regression test loads and runs hello-world through the JTAG SBA front-end, so that path stays verified while every other test uses the AXI model.

## Tile memory map and NIU registers

Every tile sees the same local map, and any remote agent addresses a tile as (x, y, local address). Code, data, mailbox and semaphores all live in the scratchpad, so the host loads and configures a tile with ordinary NoC writes.

| Range | Size | Contents |
| --- | --- | --- |
| `0x0000_0000`–`0x0000_EFFF` | 60 KB | Code, data, stack, circular buffers (boot at `0x0`) |
| `0x0000_F000`–`0x0000_FEFF` | 3.75 KB | Semaphores and kernel-defined sync words |
| `0x0000_FF00`–`0x0000_FFFF` | 256 B | Mailbox: runtime args written by the host |
| `0x1000_0000`–`0x1000_00FF` | 256 B | NIU and DMA registers (local core only) |
| `0x1000_1000`–`0x1000_10FF` | 256 B | Tile control |
| `0x1000_2000`–`0x1000_2FFF` | 4 KB | Performance counter bank: live view, snapshot view at `+0x800`, `PERF_CMD` at `+0xFF4` |
| `0x1000_3000`–`0x1000_30FF` | 256 B | Console and exit (simulation only, from M0) |

NIU registers, per DMA channel (start with 2 channels; channel n starts at 0x1000\_0000 + 0x20 × n):

| Offset | Name | Meaning |
| --- | --- | --- |
| `0x00` | `SRC` | Local scratchpad address, 16-byte aligned |
| `0x04` | `DST_ADDR` | Remote local address, 16-byte aligned (word-aligned for ATOMIC\_INC) |
| `0x08` | `DST_XY` | Remote tile, x in `[7:0]`, y in `[15:8]`; only the low 4 bits of each field reach the flit, the upper bits must be zero |
| `0x0C` | `LEN` | Bytes, multiple of 16 |
| `0x10` | `CMD` | Write starts: op (WRITE, READ\_REQ, ATOMIC\_INC), plane (AUTO, or forced NoC0/NoC1 for tests only), arg |
| `0x14` | `STATUS` | Busy bit, completed-command counter, error bit (misaligned, coordinate outside the grid, nonzero upper bits, READ\_REQ on channel 1) |

DMA moves whole flits: every DMA flit has all 16 byte enables set. Sub-16-byte writes come only from the I/O bridge (AXI byte strobes). A command with an error does not start; it sets the error bit and counts as completed so `dma_wait` cannot hang.

Tile control registers: `COORD` (read-only own x, y), `CTRL` (bit 0 = run; the host clears it to hold the core in reset), `CYCLE` (free-running counter), `DOORBELL` (set by an incoming ATOMIC\_INC to a watched address; lets the core use `wfi` later instead of polling).

Counter bank: 64-bit counters that count only while `CTRL.run` is set. Counter *i* is at `+8i` (live; reading the low word latches the high word, so low-then-high is one consistent value) and at `+0x800 + 8i` (snapshot). `+0xFF0` returns the number of counters. `PERF_CMD` (`+0xFF4`, write-only) bit 0 copies every counter into the snapshot view in the same cycle, and bit 1 clears every live counter. Both bits in one write capture the totals and restart from zero. The counters are otherwise read-only, from the local core and remotely.

Semaphores are plain 32-bit words in the scratchpad. ATOMIC\_INC is executed by the receiving NIU as a read-modify-write, so no core is involved and no lock is needed. Atomics from both RX ports go through one atomic unit in the NIU, so increments arriving on both planes to the same word in the same cycle are serialized, and the bank lock keeps other masters out during the read-modify-write. Software contract: the core does not write a semaphore word while remote tiles may increment it (resetting it is fine once all increments for the round have landed).

Incoming NoC traffic is decoded by local address: the scratchpad range goes to the bank arbiter, tile control and the counter bank accept remote accesses (so the host can start cores and read counters), and the NIU registers are reachable only from the tile's own core.

### NIU structure

| Part | Function |
| --- | --- |
| DMA channels | Channel 0 issues READ\_REQ, WRITE and ATOMIC\_INC; channel 1 issues WRITE and ATOMIC\_INC only, which bounds a tile to one outstanding READ\_REQ. Each channel runs one command at a time |
| TX-DMA | Reads the scratchpad (arbiter port 3) for DMA WRITE flits; also issues READ\_REQ and ATOMIC\_INC flits |
| TX-reply | Reply engine: reads the scratchpad, tile control or counter bank (arbiter port 4) and turns each queued READ\_REQ into `len / 16` WRITE flits back to the requester |
| Injection mux | One per plane, in front of that plane's router local input. Round-robin between TX-DMA and TX-reply when both target the same plane; when they target different planes both inject in the same cycle |
| RX0, RX1 | One ejection path per plane, each with its own arbiter port (1 and 2). Each processes its flits strictly in arrival order |
| READ\_REQ queue | Holds accepted READ\_REQs until TX-reply serves them |
| Atomic unit | Shared by RX0 and RX1, executes ATOMIC\_INC |

Ejection never waits on injection. This is how the NoC stays free of protocol deadlock with requests and replies on the same planes, without separate message classes:

- WRITE and ATOMIC\_INC drain into the scratchpad, tile control or the counter bank. They wait only on the local bank arbiter, whose starvation bound guarantees progress.
- A READ\_REQ is accepted into the READ\_REQ queue and leaves the RX path at once. The queue is sized so it can never be full when a READ\_REQ arrives:
  - Every NIU node limits its own outstanding READ\_REQs: one per compute tile (one read channel, one READ\_REQ per `dma_read`, the channel stays busy until all replies have landed), and `IO_RD_OUTSTANDING` for the I/O bridge.
  - The queue depth is at least the sum of these limits over all NIU nodes in the grid: `NX * NY + IO_RD_OUTSTANDING` in the baseline, plus the buffer-column NIUs if enabled. For 8x8 with `IO_RD_OUTSTANDING = 8` that is 72 entries of about 64 bits each.
  - An elaboration-time assertion fails the build if the depth is too small for the grid parameters, the same way as the credit round-trip check.
- Replies are posted WRITEs to the requester, which drains them by the first rule, so the dependency chain ends there.
- Replies have their own TX path, so a long local DMA never delays service of remote reads beyond the round-robin share of the injection mux, and a reply stream never blocks the tile's own DMA.

The verification watchdog and a dedicated test (every node issues its maximum READ\_REQs at one target while that target's injection is blocked) confirm the queue never overflows and RX never stalls on it.

## NoC packet format

Start with single-flit packets: every flit carries a full header plus 16 bytes of data. That removes wormhole state and per-packet VC allocation from the router, and keeps the torus bubble rule a simple credit check. A DMA of `LEN` bytes simply becomes `LEN / 16` independent flits.

Flow control is credit-based per link and plane: the receiver returns one credit per freed buffer slot, and a sender with zero credits holds the flit.

Bubble rule, which keeps the torus deadlock-free without virtual channels. Each plane has X rings (one per row) and Y rings (one per column). A flit enters a ring in two cases, and both count as an injection into that ring:

- Local injection: the NIU injects into the X ring, or straight into the Y ring when `dx = 0`.
- Dimension turn: under dimension-ordered routing a flit leaves its X ring and enters the Y ring of its destination column.

An entering flit may advance only if the downstream input buffer on the ring it enters has at least two free slots (credits ≥ 2). A flit continuing along the ring it is already on needs one credit, as on any link, and is never held by the bubble rule. Ejection to the NIU is not a ring hop. With single-flit packets this leaves at least one free slot on every ring, so a ring can always move. Input buffers are at least as deep as the credit round trip, which is always ≥ 2, so the rule is always satisfiable.

Plane selection is a fixed function of (source, destination), never of load. With `dx = (dst_x - src_x) mod GX` and `dy = (dst_y - src_y) mod GY`, NoC0 takes `dx + dy` hops and NoC1 takes `((GX - dx) mod GX) + ((GY - dy) mod GY)`. The DMA picks the plane with fewer hops; ties go to NoC0. Since each plane runs in only one direction per dimension, a destination that is east and north (or west and south) costs the long way around in one dimension on either plane. The NoC model and the performance model use this hop count, and the mean hop count is higher than on a bidirectional torus.

| Field | Bits | Meaning |
| --- | --- | --- |
| `dst_x`, `dst_y` | 4 + 4 | Destination tile (up to 16x16) |
| `src_x`, `src_y` | 4 + 4 | Source tile, used for replies and debug |
| `op` | 3 | WRITE, READ\_REQ, ATOMIC\_INC; MULTICAST from M4; remaining codes reserved (for example remote load/store) |
| `vc` | 1 | Reserved: kept for a dateline-VC fallback, unused with bubble flow control |
| `addr` | 32 | Local address at the destination |
| `be` | 16 | Byte enables for the 16-byte payload |
| `tag` | 8 | Requester's DMA channel and sequence, for completion counting; READ\_REQ replies carry the tag of their request |
| `data` | 128 | Payload |

204 bits per link and plane (8 + 8 + 3 + 1 + 32 + 16 + 8 + 128). Coordinates are 4 bits, so the router grid, including the I/O row and buffer columns, is capped at 16x16; 8x8 with both options is 10x9. Op semantics:

- WRITE: write `data` under `be` at `addr`. Posted, no response.
- READ\_REQ: `data[31:0]` holds the return address at the requester, `data[63:32]` the length in bytes (multiple of 16); the reply goes to (`src_x`, `src_y`). The destination NIU answers with `len / 16` WRITE flits carrying the request's `tag`, so reads are just writes in the other direction. The source range may be the scratchpad, tile control or the counter bank, never the NIU registers.
- ATOMIC\_INC: add `data[31:0]` to the word at `addr`, optionally ring the doorbell.

### Ordering and completion

The guarantees software may rely on, and nothing more:

1. Path ordering. Flits from one source to one destination always take the same plane and the same path, so they arrive in injection order. This covers all ops: WRITE, READ\_REQ and ATOMIC\_INC. Forcing a plane in `CMD` (tests only) voids this guarantee.
2. In-order receive. Each RX port retires flits strictly in arrival order, whatever their target. A flit is retired only once its target accepted it: the bank arbiter granted the write, the atomic completed, or the register was written. A READ\_REQ is retired when it enters the READ\_REQ queue, and its reply reads the scratchpad after every earlier flit on that RX port has retired.
3. Injection order. A channel injects its flits in address order, and the reply engine serves each READ\_REQ in address order. Across channels and the reply engine, flits interleave with no defined order.
4. `dma_wait` completion. For WRITE and ATOMIC\_INC, `dma_wait` returns once every flit has been injected, not delivered. For a read it returns once every reply flit has been written into the local scratchpad, which the NIU counts per channel by tag.
5. No ordering across destinations or planes. A write to tile C followed by a semaphore increment at tile B gives B no guarantee about C.

Patterns that follow from these:

- Producer to consumer: `dma_write(ch, …, C, …); dma_wait(ch); sem_inc(C, sem, 1);`. By rules 1 to 4 the consumer sees the data before the semaphore changes. The `dma_wait` is needed so the semaphore flit is injected after the last data flit.
- Remote fence: `noc_fence(C)` issues a 16-byte READ\_REQ to C and waits for the reply. When it returns, every earlier write from this tile to C is visible at C. Use it when another tile must be told about writes to C (rule 5).
- Launch: the host's ELF and mailbox writes and the final `CTRL.run` write all travel from the I/O tile to the same tile, and the bridge injects writes in AXI arrival order, so by rules 1 and 2 the core leaves reset only after its image is in the scratchpad.

Upgrade path once it works: multi-flit packets (64 B per header) to cut header overhead, and the M4 rectangle multicast (destinations x0 to x1, y0 to y1; its encoding is an open decision and may widen the header).

## Software stack: kernel SDK and host runtime

Two small libraries: `tile.h` for code running on the tiles, `libtorus` for the C++ host, which drives the I/O tile (the AXI model in Verilator, QDMA on FPGA).

Kernel SDK (`sw/sdk`): `crt0.S` sets the stack and calls `kernel_main(const args_t*)`; `link.ld` follows the memory map above. `tile.h` provides:

```c
typedef struct { uint8_t x, y; } xy_t;
xy_t  my_xy(void);                                        /* router coordinates */
xy_t  io_xy(void);                                        /* the I/O tile */
void  dma_write(int ch, uint32_t src, xy_t dst, uint32_t dst_addr, uint32_t len);
void  dma_read (xy_t src, uint32_t src_addr, uint32_t dst, uint32_t len);  /* channel 0 */
void  dma_wait (int ch);
void  sem_inc  (xy_t dst, uint32_t sem_addr, uint32_t n);
void  sem_wait (volatile uint32_t *sem, uint32_t value);   /* poll, later wfi */
void  noc_fence(xy_t dst);                                /* earlier writes to dst are visible */
xy_t  neighbor (int dx, int dy);                          /* torus wrap over compute tiles only */
```

All coordinates are router coordinates. `neighbor()` wraps over the NX x NY compute grid and skips the I/O row and buffer columns, so a kernel never needs to know they exist. The ordering rules a kernel may rely on are listed under Ordering and completion; `sem_inc` and `noc_fence` follow the patterns given there.

Host runtime (`sw/runtime`):

```cpp
torus::Device dev("build/Vsoc", {.nx = 4, .ny = 4});
auto grid = dev.compute_grid();
auto prog = dev.load_kernel("cannon_gemm.elf", grid);   // multicast once available
for (auto t : grid) {
    auto blk = block_of(t, N);                            // host-side partitioning
    dev.write_spm(t, A_ADDR, host_a.block(blk));          // through the I/O tile
    dev.write_spm(t, B_ADDR, host_b.block(blk));
    prog.set_args(t, {A_ADDR, B_ADDR, C_ADDR, N, blk});
}
dev.launch(prog);
dev.wait(prog);                                           // done counter in the I/O tile
for (auto t : grid) dev.read_spm(t, C_ADDR, host_c.block(block_of(t, N)));
auto perf = dev.read_counters();                          // compared against the model
```

Launch protocol:

1. Host clears `CTRL.run` on every target tile and clears their counters (`PERF_CMD.clear`).
2. Host writes the ELF segments into each tile scratchpad.
3. Host writes per-tile args into the mailbox.
4. Host sets `CTRL.run`; cores boot at `0x0`.
5. Each kernel ends with `dma_wait` on its channels and then `sem_inc(io_xy(), DONE, 1)`; DONE lives in the I/O tile register block, and the host waits until it equals the tile count.

Partitioning lives in the host code at first (block indices in the args). Pull it into a small library once two or three kernels share the same block-cyclic or 2D-block layouts.

## Verification strategy

Verify bottom-up, and make the NoC trustworthy before any kernel runs on it: most hard bugs in this design will be routing, flow-control and deadlock bugs, not core bugs.

| Level | Testbench | Golden model | Pass criteria |
| --- | --- | --- | --- |
| Link and credits | Sender and receiver with random backpressure and slice depths 0 to 2 | Cycle-accurate credit model | No overflow, no lost credit, full throughput when the receiver never stalls |
| Router | Single router, random flits on all ports of both planes | Cycle-accurate pipeline model | Every flit delivered once on the correct port, latency per stage as specified |
| NoC | Bare grid with traffic generators (uniform, transpose, hotspot, neighbor, READ\_REQ storms) | Transaction-level NoC model | Exactly-once delivery, per (src, dst) ordering, watchdog never fires, READ\_REQ queues never overflow |
| NIU and DMA | One tile, register-level C++ driver | NIU and DMA model | Every op and length, byte enables, completion counters, error bit, in-order retire across targets |
| Bank arbiter | Random requests on all seven ports, atomics included | Arbiter model | Grants match the model, no request waits beyond the starvation bound, atomics never interleaved |
| Core | Spike lockstep per tile on the retire trace | Spike | Zero mismatches over the kernel suite |
| System | Kernels launched through the I/O tile | C++ kernel references + performance model | Bit-exact output; counters within tolerance of the model |

Three points need care:

- Deadlock watchdog: if no flit moves anywhere for N cycles while flits are in flight, dump router occupancy and fail. Run long random torus traffic with it on before trusting the bubble scheme.
- Spike runs with `rv32im_zicsr_zifencei_zicclsm` (CV32E40X supports misaligned accesses to main memory). CV32E40X reports data bus errors as an NMI after the faulting access retires; Spike has no such NMI, so the lockstep performs the NMI entry when RVFI reports it, and checks the vector.
- Spike co-simulation sees values Spike cannot predict: scratchpad words changed underneath it (DMA, remote writes, atomics) and MMIO reads (`CYCLE`, NIU `STATUS`, `DOORBELL`, the counter bank). Sync the RTL value into Spike on each retired load from either, the same trick as a coherence-visibility hook in a multicore co-sim. Only loads are synced; stores and register writebacks are still compared.
- Seeds: every random test takes a seed on the command line and prints it, so failures replay exactly.
- Power-up state: simulations start with random flop values (`--x-initial unique`) and model initial edges (`--x-initial-edge`), so reset bugs show up as seed-dependent failures instead of hiding behind zero initialization.

### NoC full-throughput rule

With no contention and a receiver that never stalls, every link, router port and NIU port moves one flit per cycle, back to back. A bubble is a cycle in which a sender holds a valid flit and has a credit, yet the link stays idle. Bubbles are bugs, not tuning.

Design requirement: each input buffer is at least as deep as the credit round trip (forward link latency, slice depth included, plus the stage that frees the slot, plus the credit return latency), which is `4 + 2 × slices` cycles, plus one slot: a flit entering a ring needs two credits, so an injection or turn stream can never use the last one. Router input buffers therefore hold `5 + 2 × slices` flits. An elaboration-time assertion on the parameters fails the build if a buffer is too shallow.

| Check | Stimulus | Pass |
| --- | --- | --- |
| Link | One sender streams N flits | N flits arrive in exactly N + link latency cycles |
| Router pass-through | Stream on each input-to-output pair | One flit per cycle on the output, no gaps |
| Parallel, no conflicts | All inputs to distinct outputs at once, per plane | Every output at 100% |
| Multi-hop | Stream across the grid diameter | End-to-end one flit per cycle after pipeline fill |
| Neighbor permutation | Every tile streams to its east neighbor (then each other direction) on the torus | Every link at 100% |
| Recovery | Receiver stalls k cycles, then releases | Full rate within one credit round trip |
| NIU | DMA injection, reply streaming and ejection on both planes at once, core and engine idle | One flit per cycle sustained on TX-DMA, TX-reply, RX0 and RX1 (TX-DMA and TX-reply on different planes), bank arbiter included |
| Torus ring | Ring traffic plus pending local injections and X-to-Y turns | Flits continuing on a ring are never held by the bubble rule; only local injections and turns wait for two credits |

Every check runs at slice depths 0, 1 and 2 and on both planes.

Instrumentation: every sender has a bubble counter (valid flit, credit available, nothing sent), backed by a simulation assertion. A flit entering a ring with exactly one credit is held by the bubble rule, not by a bug; those cycles go to a separate bubble-rule-hold counter and do not count as bubbles. It must read zero in every test of the regression, not only the throughput tests. Contention tests such as hotspot traffic are exempt from the 100% target but must still show zero bubbles. Link-utilization counters are compared against 100% in the throughput checks.

Optional: SymbiYosys properties on the router (no flit created or lost, credit counters bounded) once the RTL settles.

## Milestones

Seven milestones, each ending in a runnable test rather than a feature list. The NoC is a torus from M1: with one-way planes a mesh cannot reach every destination (NoC0 alone cannot go west or north, and even both planes cannot reach north-east or south-west without the wraparound links). M1 builds the torus on NoC0, M3 adds NoC1. M6 is optional.

| Milestone | Scope | Exit test |
| --- | --- | --- |
| M0 | Environment and first tile | Hello-world and ISA tests on one tile in Spike lockstep, counters read out, area report, all in CI |
| M1 | 2x2 torus on NoC0: pipelined credit-based router, bubble rule, NIU | Ping-pong kernel on a 2x2 torus (backdoor load), router throughput suite green at slice depths 0 to 2, long random traffic without deadlock |
| M2 | I/O row, I/O tile and launch | memcpy on a 4x4 torus (NoC0) loaded and launched from C++ through the I/O tile, bit-exact, performance model within tolerance |
| M3 | Second plane | Cannon GEMM on a 4x4 torus with both planes, bit-exact, counters within 15% of the model |
| M4 | Scale to 8x8 | Full kernel suite at 8x8, scaling efficiency and model error recorded |
| M5 | Compute engine | GEMM on the engine bit-exact, speedup and perf/area against scalar recorded |
| M6 (optional) | Place and route | Logical against folded floorplan comparison recorded |

The exit tests summarize the microtasks below, which are the authoritative list.

Progress (2026-11-17): M0 and M1 done. M1 results: per-hop latency 2 cycles plus link slices; every link, router port and NIU port at one flit per cycle; no deadlock in a 2M-cycle saturating soak, while builds without the bubble rule deadlock within thousands of cycles; ping-pong on the 2x2 torus at 352 cycles per 64-byte round trip, dominated by software (DMA setup and polling). Tile area (Yosys generic, in context): about 400k transistors of logic plus 64 KB SRAM; the counter bank (33 x 64-bit live + snapshot, about 160k transistors standalone) is the largest unit and is up for review. Next: M2.

Keep every exit test in the regression once it passes, so later milestones cannot silently break earlier ones.

## M0 plan: environment and first tile

Goal: one tile (core, scratchpad, tile control) runs a C program in Verilator, in lockstep with Spike, and synthesizes in Yosys, all from the pinned container and in CI. The full flow (compile, load, run, check, read counters) works end to end from the first milestone; only the load path is temporary.

| # | Work package | Output | Done when |
| --- | --- | --- | --- |
| 1 | Container | versions.env + Dockerfile on a pinned OSS CAD Suite release, plus RISC-V GCC and Python; Spike as a submodule | CI job builds the image and prints all tool versions |
| 2 | Core integration | CV32E40X as a submodule, configured RV32IM, thin wrapper exposing instruction and data ports | Verilator lint is clean and Yosys elaborates the wrapper |
| 3 | Minimal tile | Wrapper + scratchpad (4 banks x 128 bit, seven-port arbiter with starvation bound, only the core's instruction and data ports connected) + tile control (`COORD`, `CTRL`, `CYCLE`) + console and exit MMIO | Scratchpad unit test passes against its model |
| 4 | SDK v0 | `crt0.S`, `link.ld` per the memory map, `tile.h` stubs, `printf` over console MMIO | Hello-world and rv32ui/rv32um ISA tests build |
| 5 | Testbench | C++ Verilator TB: backdoor ELF load, releases `CTRL.run`, runs to exit, optional FST, seed and plusargs | Exit code reflects pass or fail |
| 6 | Spike lockstep | Spike stepped on the core's RVFI retire trace: PC, instruction, register writeback, traps; MMIO load values synced from RTL | Zero mismatches on ISA tests and hello-world |
| 7 | Counters v0 | Counter bank with cycles, retired instructions, fetch stalls, data stalls; TB reads it at exit | Counters printed per run and checked for consistency (retired ≤ cycles) |
| 8 | Synthesis | Yosys generic flow for the tile, area per module | Report generated in CI and stored per commit |
| 9 | Regression | `regress.py`: runs everything with seeds, JUnit output | One command runs all of M0 locally and in CI |

M0 deliberately leaves out the NoC, NIU and I/O tile. The ELF is loaded through a testbench backdoor, which M2 replaces with loading through the I/O tile; everything else in the flow stays the same.

Risks to check first:

- CV32E40X SystemVerilog constructs through yosys-slang; fall back to sv2v for synthesis only if needed.
- Aligning Spike and RVFI on traps and CSR side effects, so lockstep mismatches mean real bugs.
- Scratchpad bank arbitration is almost trivial at M0 (core instruction and data only), so the arbiter and its model must already have all seven ports and the starvation bound to avoid rework in M1 and M5. Instruction fetch and data access colliding on one bank is the first real conflict case and is tested from M0.

M0 exit checklist:

- [x] Image built in CI from pinned versions and pullable from the registry
- [x] Tile passes Verilator lint and Yosys elaboration
- [x] Hello-world prints and exits cleanly
- [x] ISA tests pass with zero Spike mismatches
- [x] Counters read out on every run
- [x] Area report per module produced in CI
- [x] `regress.py` green locally and in CI

## Microtasks per milestone

Each microtask is small (about a day or two) and ends in something tangible: a test that passes, a program that runs, or a number that gets recorded. The format is task → result. A microtask is done only when its result is in the regression or in a stored report.

### M0: environment and first tile

- [x] `tools/versions.env` with the OSS CAD Suite date, GCC release and Python pins → file committed
- [x] Dockerfile from OSS CAD Suite + RISC-V GCC + Python → image builds, versions logged
- [x] Slang frontend check → `yosys -m slang` reads a small SystemVerilog test, or the sv2v fallback is decided
- [x] Spike submodule built by CMake → `spike --help` runs from the build tree
- [x] CI job builds the image on versions.env changes and pushes it to the registry → image pullable by its hash tag
- [x] CV32E40X submodule configured RV32IM → core passes Verilator lint
- [x] Core wrapper with instruction and data ports (32-bit OBI to 128-bit bank adapter) → wrapper passes lint and Yosys elaboration
- [x] Scratchpad bank module + C++ model → random read/write unit test passes
- [x] Bank arbiter with seven ports and starvation bound (core instruction and data connected only) → arbiter test passes against its model for all port combinations, bound never exceeded
- [x] Tile control registers (`COORD`, `CTRL`, `CYCLE`) → register test passes
- [x] Console and exit MMIO → testbench prints a character stream and returns the exit code
- [x] `crt0.S` + `link.ld` → `hello.elf` builds, map file matches the memory map
- [x] Testbench backdoor ELF loader → dumped scratchpad matches the ELF
- [x] Hello-world → prints and exits 0
- [x] rv32ui and rv32um ISA tests ported → all pass
- [x] RVFI tap + Spike lockstep → zero mismatches on hello-world
- [x] Lockstep on the ISA tests → zero mismatches
- [x] Counter bank v0 → counters printed on every run
- [x] Yosys flow for the tile → area report per module
- [x] `regress.py` with seeds and JUnit → one command green locally and in CI

### M1: 2x2 torus on NoC0, pipelined credit-based router

- [x] `noc_pkg` with flit, coordinates and ops → packs to 204 bits, unit test
- [x] Credit link sender and receiver + cycle model → random backpressure test: no overflow, no lost credit
- [x] Link register slice, depth 0 to 2 → full throughput at every depth
- [x] Credit round-trip parameter check → elaboration fails when a buffer is too shallow
- [x] Bubble counter on every sender + assertion → zero bubbles in all existing tests (links, routers; the NIU senders get theirs with the NIU)
- [x] Torus routing on a unidirectional plane, X first, with the lookahead port on the link → exhaustive routing test
- [x] 3x3 per-plane router module (direction parameters, local port optional) → lint clean, instantiated for both planes
- [x] Bubble rule on ring entry → unit test: local injections and X-to-Y turns blocked when fewer than two credits remain, while flits continuing on a ring never wait; bubble-rule holds counted separately from bubbles
- [x] Router input buffers + round-robin switch allocator → single-router random test matches the cycle model
- [x] Two-stage router pipeline → measured per-hop latency equals the spec
- [x] Router counters (flits, credit stalls, arbitration losses) → counters equal scoreboard counts
- [x] Bare 2x2 torus with traffic generators → exactly-once delivery, ordering, watchdog silent
- [x] Long random torus traffic with the watchdog, also on a 4x4 bare torus → no deadlock over the full run
- [x] Transaction-level NoC model → RTL matches it on all traffic patterns
- [x] Single-router throughput suite (pass-through, parallel, recovery) at slice depths 0 to 2 → one flit per cycle per output, results stored
- [x] Multi-hop and neighbor-permutation streams on the 2x2 and 4x4 bare torus → every link at 100%
- [x] NIU transmit path (DMA WRITE) on the TX-DMA arbiter port and per-plane injection mux → a tile sends `LEN` bytes in loopback
- [x] NIU receive path into the scratchpad on the RX arbiter ports → remote writes land with correct byte enables
- [x] In-order retire across targets → a write stream followed by a tile-control write retires in order under bank contention
- [x] NIU status, error bit and completion counters → `dma_wait` works from software, bad commands set the error bit and never hang
- [x] NIU injection and ejection stream → one flit per cycle sustained on TX-DMA and RX
- [x] `tile.h` `dma_write` and `dma_wait` → SDK test passes
- [x] 2x2 SoC top on NoC0 (backdoor load) → ping-pong kernel passes, round-trip latency recorded
- [x] Router and NIU in the Yosys report → area recorded

### M2: I/O tile and launch

- [ ] C++ AXI bus functional model through DPI → burst write and read test against a memory model
- [ ] Async CDC FIFO → randomized clock-ratio test passes, no loss or duplication
- [ ] I/O row in the grid generator (router-only stops, I/O tile at (0, NY)) → grid elaborates, `compute_grid()` excludes the row
- [ ] AXI-to-NoC bridge with burst splitting, byte strobes, `IO_RD_OUTSTANDING` and backpressure → host write lands in any tile's scratchpad
- [ ] I/O tile assembled (AXI front-end, FIFO, bridge, NIU) → I/O tile testbench passes
- [ ] C++ remote read and write API → unit test passes
- [ ] `CTRL.run` over the NoC → host releases a core remotely
- [ ] ELF loading through the I/O tile → backdoor removed, hello-world runs
- [ ] Mailbox and `set_args` → kernel reads its arguments
- [ ] READ\_REQ in the NIU, TX-reply engine on its own arbiter port, tag-based completion → remote read returns correct data, and served at full rate while the tile's own DMA streams on the other plane
- [ ] READ\_REQ queue sizing assertion → elaboration fails when the queue is too shallow for the grid
- [ ] READ\_REQ storm (every node at its limit, one target, target injection blocked) → no queue overflow, RX never stalls, watchdog silent
- [ ] `noc_fence` → writes before the fence are visible at the destination when it returns
- [ ] ATOMIC\_INC and semaphores → all tiles increment one counter, final value exact
- [ ] DONE counter + `dev.wait` → host waits for all tiles
- [ ] `libtorus` v0 (`Device`, `load_kernel`, `launch`, `wait`) → example program runs
- [ ] Counter readout over the NoC → `dev.read_counters` returns all banks
- [ ] memcpy on a 4x4 torus (NoC0) launched from C++ → bit-exact, counters stored
- [ ] Performance model v0 for memcpy → prediction within tolerance of the counters

### M3: second plane

- [ ] NoC1 plane (west and north) → both planes pass the traffic tests, I/O row included in the Y rings
- [ ] Automatic plane selection in the DMA (fixed per source and destination) → measured hop counts match the prediction, per (src, dst) ordering holds
- [ ] NoC model updated for torus and two planes → RTL matches
- [ ] `neighbor()` in `tile.h` → wraparound over the compute grid passes, I/O row skipped
- [ ] Cannon GEMM, scalar, 4x4 → bit-exact
- [ ] Cannon counters vs the performance model → within 15%
- [ ] Same kernels on one plane and on two planes → comparison table recorded

### M4: scale to 8x8

- [ ] 8x8 build with Verilator threads → simulation speed recorded
- [ ] Rectangle multicast op → kernel load time reduced, test passes
- [ ] 2D stencil kernel → bit-exact
- [ ] Reduction kernel → bit-exact
- [ ] Full kernel suite at 8x8 → all pass
- [ ] Scaling 4x4 to 8x8 → efficiency measured
- [ ] Yosys area, generic and sky130 → report stored
- [ ] Model calibration → error table per kernel
- [ ] Buffer columns decision → recorded in open decisions

### M5: compute engine

- [ ] Engine choice made with the model → decision recorded
- [ ] Engine golden model in C++ → test vectors generated
- [ ] Engine RTL standalone with its scratchpad port → matches the golden model
- [ ] CV-X-IF control path and custom instructions → issue and result test passes
- [ ] Spike model of the custom instructions → lockstep passes
- [ ] Engine arbiter port live → bank-conflict and starvation-promotion counters recorded
- [ ] GEMM kernel on the engine → bit-exact, speedup measured
- [ ] Perf/area vs scalar → table recorded

### M6: place and route (optional)

- [ ] OpenROAD flow for one tile → placed and routed, timing report
- [ ] Grid floorplan in logical order → longest wire and timing recorded
- [ ] Grid floorplan in folded order → comparison recorded
- [ ] FPGA pblocks in folded order → timing comparison recorded

## Open decisions

| Decision | Options | Status | Decide by |
| --- | --- | --- | --- |
| Core | CV32E40X, Ibex, own in-order core | Decided: CV32E40X (RV32IM, RVFI, CV-X-IF for M5) | Done |
| Scratchpad organization | 4 banks x 128-bit, 2 banks dual-port | Decided: 4 banks x 128-bit, seven-port arbiter (engine, NIU RX0, RX1, TX-DMA, TX-reply, core data, core instruction) with a starvation bound | Done |
| I/O tile placement | In the compute grid, extra row, extra column | Decided: extra I/O row at logical `y = NY`, part of the Y rings; further row blocks later | Done |
| Request/reply deadlock | Separate message classes, NIU always sinks requests | Decided: READ\_REQ queue sized for the worst case, ejection never waits on injection | Done |
| Router port structure | One 3x3 router per plane, one combined router for both planes | Decided: one 3x3 router per plane; separate NIU TX paths for DMA writes and READ\_REQ replies | Done |
| Router pipeline depth | 1, 2 or 3 stages per hop | Decided: 2 stages (buffer write, switch allocation and lookahead route computation; then switch traversal into the output register), routes computed one hop ahead | Done |
| Torus deadlock avoidance | Dateline VCs, bubble flow control | Leaning: bubble on top of credits; the packet's `vc` bit stays reserved as a fallback | M3 |
| Multicast encoding | Rectangle in the header, separate multicast flit | Open: decide with the M4 kernel-load measurements | M4 |
| Buffer columns | None, two columns half an X ring apart | Leaning: none; enable only if a workload's model shows it needs a shared working set | M4 |
| Synthesis target | Generic Yosys cells, sky130 liberty | Leaning: generic for area trends, sky130 when timing matters | M4 |
| Phase-2 engine | Zve32x, custom int8 matrix unit | Open: Zve32x for standard tooling, custom unit for performance per area | M5 |

- [x] Confirm CV32E40X builds in Verilator and Yosys with the chosen frontend (M0): yes, yosys-slang, no sv2v
- [x] Fix flit width and coordinate width: 204-bit flit, 4-bit coordinates, router grid up to 16x16 including the I/O row and buffer columns
- [ ] Pick the first GEMM size and block size so it fits 60 KB per tile with double buffering
