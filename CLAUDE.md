# Working on this repository

SPEC.md is the design and plan (architecture, verification rules, milestones,
microtask checklists, progress notes). README.md has the user-facing commands.
Keep both current: tick microtasks and update the progress line under the
milestone table when work lands.

## Build and test

Everything runs in the pinned tool image (tools/versions.env). Locally the image
is usually built as `torus-tools:local`:

```sh
source tools/versions.env
docker build -f docker/Dockerfile --build-arg BASE_IMAGE="$BASE_IMAGE" -t torus-tools:local .
TORUS_IMAGE=torus-tools:local tools/dev.sh cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
TORUS_IMAGE=torus-tools:local tools/dev.sh ninja -C build
TORUS_IMAGE=torus-tools:local tools/dev.sh scripts/regress.py --no-build --seed 1
```

- `scripts/regress.py` is the gate: every test is seeded and prints a replay
  command; add each new test there (and CMake target in tb/*/CMakeLists.txt).
- Area: `scripts/synth_area.py` (also run by CI and `regress.py --synth`).
- Verilator runs at CMake configure time: a lint warning (`-Wall`, fatal) fails
  configuration, and the old binary is left in place. Check the build output
  before trusting a test result, especially in mutation tests.
- If a Verilator run fails (lint) and a later one changes how many C++ files it
  splits into, the link fails with undefined `___024root__*` symbols: delete
  that target's `V<top>.dir` under build/ and reconfigure.
- Spike builds once into build/spike-install (about 5 minutes) and is reused
  while the submodule commit is unchanged.

## Conventions

- Every block lands with its testbench and golden model (cycle-accurate where
  timing matters: links, router, arbiter). Prove each new test can fail by
  injecting a lint-clean bug once, then restore.
- RTL is lint-clean under `-Wall`; waivers only in rtl/lint/vendor.vlt (passed to
  Verilator only, never in the shared .f file lists, which Yosys also reads) or
  as narrow `lint_off` comments with a reason.
- Elaboration checks use `$fatal` (Verilator runs with `-Werror-USERFATAL`).
- Simulations start from random state (`--x-initial unique --x-initial-edge`);
  testbenches turn assertions off until reset has been applied.
- One commit per verified step, message explains why, push to main (CI gates
  regress and synth). Commit trailer: Co-Authored-By line per session rules.
- C++ mirrors of RTL types live in tb/models (noc_flit.h is checked bit-exact
  against noc_pkg by tb_flit).

## Pitfalls already found

- CV32E40X keeps only mtvec[31:7]: trap entries must be 128-byte aligned.
- CV32E40X reports data bus errors as an NMI (mcause 0x80000400 load,
  0x80000401 store) after the access retires; the Spike lockstep emulates it.
- Spike lockstep (tb/models/spike_cosim.*) syncs scratchpad and MMIO load values
  from RTL; it cannot predict them (NIU writes, uninitialized bytes).
- RVFI reports one cycle after the core counts a retirement; stopping a core can
  drop the last report (tb_soc allows exactly one).
- Router input buffers need the credit round trip plus one slot (bubble rule):
  `5 + 2 x slices`.
- On rings of length 2 no cyclic wait can form, so deadlock demonstrations need
  at least 3x3.
- Yosys (yosys-slang) flattens by default; per-unit area comes from separate
  runs per unit (scripts/synth_area.py).
