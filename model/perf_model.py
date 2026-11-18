#!/usr/bin/env python3
"""Performance model v0 (SPEC.md, M2 and M3): memcpy on the two-plane torus.

The model routes every flit a run sends the way the RTL does (the plane with
fewer hops, ties to NoC0; X first on one-way rings, NoC0 east and south, NoC1
west and north, the I/O row inside every Y ring) and predicts two things a
memcpy report (tb/soc/host_memcpy.cpp) must match:

  - exactly, the flits through every compute tile's router outputs (X and Y,
    per plane) and its NIU's sent and received counts per plane: data and
    semaphore flits, the DONE increments to the I/O tile, and the host's stop
    writes;
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
# Counter bank events (rtl/pkg/tile_pkg.sv, perf_event_e); a report lists the
# events its counters 2..9 counted
EV_NIU_TX = (18, 19)        # per plane
EV_NIU_RX = (20, 21)
EV_NIU_ERR = 22
EV_FLIT = (23, 35)          # per plane, + port (L, X, Y)

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

    def plane(self, src, dst):
        """The plane every NIU picks: fewer hops, ties to NoC0 (noc_pkg::select_plane)."""
        hx, hy = (dst[0] - src[0]) % self.gx, (dst[1] - src[1]) % self.gy
        return 1 if (-hx) % self.gx + (-hy) % self.gy < hx + hy else 0

    def path(self, src, dst):
        """Router outputs (x, y, plane, port) a flit takes from src to dst, ejection included."""
        p = self.plane(src, dst)
        step = 1 if p == 0 else -1
        (x, y), out = src, []
        while x != dst[0]:
            out.append((x, y, p, X))
            x = (x + step) % self.gx
        while y != dst[1]:
            out.append((x, y, p, Y))
            y = (y + step) % self.gy
        out.append((x, y, p, L))
        return out

    def hops(self, src, dst):
        return len(self.path(src, dst)) - 1


def predict(rep):
    g = Grid(rep["nx"], rep["ny"])
    flits = rep["len"] // 16
    tiles = [(t["x"], t["y"]) for t in rep["tiles"]]
    partner = {(t["x"], t["y"]): tuple(t["partner"]) for t in rep["tiles"]}
    links = Counter()       # (x, y, plane, port) -> flits
    tx, rx = Counter(), Counter()   # (x, y, plane) -> flits
    flows = Counter()       # (x, y, plane, port) -> data flows sharing it
    for t in tiles:
        p = partner[t]
        for hop in g.path(t, p):
            links[hop] += flits + 1          # data and the semaphore increment
            flows[hop] += 1
        for hop in g.path(t, g.io):          # DONE
            links[hop] += 1
        tx[(*t, g.plane(t, p))] += flits + 1
        tx[(*t, g.plane(t, g.io))] += 1
        rx[(*p, g.plane(t, p))] += flits + 1
    # Stop writes, last tile first: each passes only tiles still running
    for t in tiles:
        for hop in g.path(g.io, t):
            links[hop] += 1
        rx[(*t, g.plane(g.io, t))] += 1
    # Copy phase
    worst, max_share = 0, 1
    for t in tiles:
        p = partner[t]
        share = max(flows[h] for h in g.path(t, p) if h[3] != L) if t != p else 1
        max_share = max(max_share, share)
        worst = max(worst, SETUP + flits * share + ENDS + HOP * g.hops(t, p) + TAIL)
    return g, links, tx, rx, worst, max_share


def expected_event(ev, xy, links, tx, rx):
    """What counter event ev of tile xy must read, per the model."""
    for p in (0, 1):
        if ev == EV_NIU_TX[p]:
            return tx[(*xy, p)]
        if ev == EV_NIU_RX[p]:
            return rx[(*xy, p)]
        if EV_FLIT[p] <= ev < EV_FLIT[p] + 3:
            return links[(*xy, p, ev - EV_FLIT[p])]
    if ev == EV_NIU_ERR:
        return 0
    return None


def check(rep, tolerance):
    g, links, tx, rx, copy, share = predict(rep)
    ok = True
    for t in rep["tiles"]:
        xy = (t["x"], t["y"])
        c = t["counters"]
        for k, ev in enumerate(rep["events"]):
            want = expected_event(ev, xy, links, tx, rx)
            if want is not None and c[2 + k] != want:
                print(f"tile {xy}: counter {2 + k} (event {ev}) is {c[2 + k]}, model {want}")
                ok = False
    meas = rep["copy_cycles"]
    err = (copy - meas) / meas
    total = sum(links.values())
    per_plane = [sum(v for k, v in links.items() if k[2] == p) for p in (0, 1)]
    print(f"memcpy {rep['nx']}x{rep['ny']}, {rep['len']} B per tile, shift {tuple(rep['shift'])}: "
          f"copy phase {meas} cycles, model {copy} ({err:+.1%}); {total} router-output flits "
          f"(NoC0 {per_plane[0]}, NoC1 {per_plane[1]}), per-link counts {'exact' if ok else 'MISMATCH'}")
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
        tag = args.shift.replace(",", "_").replace("-", "m")
        name = f"memcpy_{rep['nx']}x{rep['ny']}_s{tag}.json"
        with open(os.path.join(args.report_dir, name), "w") as f:
            json.dump(rep, f, indent=1)
        return 0 if check(rep, args.tolerance) else 1


if __name__ == "__main__":
    sys.exit(main())
