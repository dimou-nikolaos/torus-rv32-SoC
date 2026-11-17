#!/usr/bin/env python3
"""Performance model v0 (SPEC.md, M2): memcpy on the NoC0 torus.

The model routes every flit a run sends the way the RTL does (X first on
one-way rings, NoC0 east and south, the I/O row inside every Y ring) and
predicts two things a memcpy report (tb/soc/host_memcpy.cpp) must match:

  - exactly, the flits through every compute tile's router outputs (local,
    X, Y) and its NIU's sent and received counts: data and semaphore flits,
    the DONE increments to the I/O tile, and the host's stop writes;
  - within a tolerance, the copy phase (first DMA start to last arrival): DMA
    and semaphore setup, plus the flits of one block at the rate of the most
    shared link on the flow's path (round-robin arbitration splits a link
    evenly), plus the path latency (2 cycles per hop, 4 at the ends).

v0 checks the copy phase only when no link carries two flows. With shared
links the even split is optimistic: one input buffer then holds a flow that
turns and one that continues, and a waiting head blocks the flit behind it
(head-of-line blocking). Measured on 4x4 with shift (2,2): 3519 cycles, the
even split predicts 2122. v1 models the router queues flit by flit.

  model/perf_model.py memcpy --report build/reports/memcpy.json
  model/perf_model.py run-memcpy --host build/tb/soc/host_memcpy_4x4 \\
      --elf build/sw/memcpy.elf --shift 1,1 --seed=1      # run, then check
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

L, X, Y = 0, 1, 2
# Counter indices with the selects after reset (rtl/pkg/tile_pkg.sv)
C_NIU_TX, C_NIU_RX, C_NIU_ERR, C_FLIT_L, C_FLIT_X, C_FLIT_Y = 4, 5, 6, 7, 8, 9

# Calibration of v0 (cycles): kernel setup before the first flit (CYCLE read,
# five NIU register writes, scratchpad read and staging), and the tail after
# the last data flit (dma_wait polling, sem_inc programming, its flit, the
# receiver's sem_wait polling and CYCLE read).
SETUP = 20
TAIL = 40
HOP = 2
ENDS = 4


class Grid:
    def __init__(self, nx, ny):
        self.nx, self.ny = nx, ny
        self.gx, self.gy = nx, ny + 1
        self.io = (0, ny)

    def path(self, src, dst):
        """Router outputs (x, y, port) a flit takes from src to dst, ejection included."""
        (x, y), out = src, []
        while x != dst[0]:
            out.append((x, y, X))
            x = (x + 1) % self.gx
        while y != dst[1]:
            out.append((x, y, Y))
            y = (y + 1) % self.gy
        out.append((x, y, L))
        return out

    def hops(self, src, dst):
        return len(self.path(src, dst)) - 1


def predict(rep):
    g = Grid(rep["nx"], rep["ny"])
    flits = rep["len"] // 16
    tiles = [(t["x"], t["y"]) for t in rep["tiles"]]
    partner = {(t["x"], t["y"]): tuple(t["partner"]) for t in rep["tiles"]}
    links = Counter()       # (x, y, port) -> flits
    tx, rx = Counter(), Counter()
    flows = Counter()       # (x, y, port) -> data flows sharing it
    for t in tiles:
        p = partner[t]
        for hop in g.path(t, p):
            links[hop] += flits + 1          # data and the semaphore increment
            flows[hop] += 1
        for hop in g.path(t, g.io):          # DONE
            links[hop] += 1
        tx[t] += flits + 2
        rx[p] += flits + 1
    # Stop writes, last tile first: each passes only tiles still running
    for t in tiles:
        for hop in g.path(g.io, t):
            links[hop] += 1
        rx[t] += 1
    # Copy phase
    worst, max_share = 0, 1
    for t in tiles:
        p = partner[t]
        share = max(flows[h] for h in g.path(t, p) if h[2] != L) if t != p else 1
        max_share = max(max_share, share)
        worst = max(worst, SETUP + flits * share + ENDS + HOP * g.hops(t, p) + TAIL)
    return g, links, tx, rx, worst, max_share


def check(rep, tolerance):
    g, links, tx, rx, copy, share = predict(rep)
    ok = True
    for t in rep["tiles"]:
        xy = (t["x"], t["y"])
        c = t["counters"]
        want = {C_FLIT_L: links[(*xy, L)], C_FLIT_X: links[(*xy, X)], C_FLIT_Y: links[(*xy, Y)],
                C_NIU_TX: tx[xy], C_NIU_RX: rx[xy], C_NIU_ERR: 0}
        for k, v in want.items():
            if c[k] != v:
                print(f"tile {xy}: counter {k} is {c[k]}, model {v}")
                ok = False
    meas = rep["copy_cycles"]
    err = (copy - meas) / meas
    total = sum(links.values())
    print(f"memcpy {rep['nx']}x{rep['ny']}, {rep['len']} B per tile, shift {tuple(rep['shift'])}: "
          f"copy phase {meas} cycles, model {copy} ({err:+.1%}); {total} router-output flits, "
          f"per-link counts {'exact' if ok else 'MISMATCH'}")
    if share > 1:
        print(f"links carry up to {share} flows: copy phase not checked by v0 (no head-of-line blocking model)")
    elif abs(err) > tolerance:
        print(f"copy phase outside the {tolerance:.0%} tolerance")
        ok = False
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("memcpy", help="check a memcpy report")
    a.add_argument("--report", required=True)
    a.add_argument("--tolerance", type=float, default=0.10)
    b = sub.add_parser("run-memcpy", help="run host_memcpy, then check its report")
    b.add_argument("--host", required=True)
    b.add_argument("--elf", required=True)
    b.add_argument("--shift", default="1,1")
    b.add_argument("--len", default="16384")
    b.add_argument("--tolerance", type=float, default=0.10)
    b.add_argument("--report-dir", default=os.path.join(ROOT, "build", "reports"), help="where the report is kept")
    args, rest = ap.parse_known_args()

    if args.cmd == "memcpy":
        return 0 if check(json.load(open(args.report)), args.tolerance) else 1
    with tempfile.TemporaryDirectory() as d:
        report = os.path.join(d, "memcpy.json")   # kept under --report-dir once checked
        cmd = [args.host, args.elf, f"--shift={args.shift}", f"--len={args.len}", f"--report={report}"] + rest
        print("$", " ".join(cmd), flush=True)
        if subprocess.run(cmd).returncode != 0:
            return 1
        rep = json.load(open(report))
        os.makedirs(args.report_dir, exist_ok=True)
        name = f"memcpy_{rep['nx']}x{rep['ny']}_s{args.shift.replace(',', '')}.json"
        with open(os.path.join(args.report_dir, name), "w") as f:
            json.dump(rep, f, indent=1)
        return 0 if check(rep, args.tolerance) else 1


if __name__ == "__main__":
    sys.exit(main())
