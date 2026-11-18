#!/usr/bin/env python3
"""Performance model (SPEC.md, M2 to M4): memcpy and Cannon GEMM on the
two-plane torus.

The model routes every flit a run sends the way the RTL does (the plane with
fewer hops, ties to NoC0, unless a program forces one; X first on one-way
rings, NoC0 east and south, NoC1 west and north, the I/O row inside every Y
ring) and checks a report (tb/soc/host_memcpy.cpp, host_cannon.cpp):

  - exactly: the flits through every compute tile's router outputs and its
    NIU's sent and received counts, per plane: the program's data and
    semaphore flits, the DONE increments to the I/O tile, the host's stop
    writes;
  - within a tolerance, the time of the program's main phase.

memcpy (v1): the copy phase (first DMA start to last arrival) comes from
model/noc_sim.py, the network flit by flit (cycle-exact against the RTL
network: noc_sim.py check-rtl), with each tile's DMA starting SETUP cycles
after its measured start, the semaphore increment SEM cycles after dma_wait
returns, and the waiting tile seeing it WAIT cycles after it arrives. On
shared links the copy phase is chaotic: a few cycles of difference in when
tiles start moves it by up to 20% (head-of-line blocking at the router
inputs locks flows into different phase patterns). The model therefore runs
JITTER_RUNS times, the first with the measured start times and the others
with each start moved by up to JITTER cycles, and gives the range; the
measurement must fall within it, widened by the tolerance. v0 (the even split
of shared links, no head-of-line blocking) is kept as the lower bound.

Cannon: P steps of one b x b block multiply, the time a 1x1 run of the same
kernel measured for one block (calibration), plus P-1 hand-overs: the block
shifts run under the compute, so a hand-over is the software around the
semaphores (HANDOVER) plus two semaphore trips (arrival, then buffer free) over
the longest shift path.

  model/perf_model.py memcpy --report build/reports/memcpy.json
  model/perf_model.py run-memcpy --host build/tb/soc/host_memcpy_4x4 \\
      --elf build/sw/memcpy.elf --shift=1,1 --seed=1      # run, then check
  model/perf_model.py run-cannon --host build/tb/soc/host_cannon_4x4 \\
      --cal-host build/tb/soc/host_cannon_1x1 --elf build/sw/cannon.elf --n 64
  model/perf_model.py planes --build build             # one plane against two
"""
import argparse
import json
import os
import subprocess
import sys
import random
import statistics
import tempfile
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noc_sim import NocSim  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORTS = os.path.join(ROOT, "build", "reports")

L, X, Y = 0, 1, 2
# Counter bank events (rtl/pkg/tile_pkg.sv, perf_event_e); a report lists the
# events its counters 2..9 counted
EV_NIU_TX = (18, 19)        # per plane
EV_NIU_RX = (20, 21)
EV_NIU_ERR = 22
EV_FLIT = (23, 35)          # per plane, + port (L, X, Y)
FORCED = {"auto": None, "noc0": 0, "noc1": 1}

# memcpy calibration (cycles): kernel setup before the first flit (CYCLE read,
# five NIU register writes, scratchpad read and staging), and the tail after
# the last data flit (dma_wait polling, sem_inc programming, its flit, the
# receiver's sem_wait polling and CYCLE read).
SETUP = 20
TAIL = 40
HOP = 2
ENDS = 4
# v1 memcpy endpoints (cycles), fitted on the uncontended 2x2 and 4x4 runs:
# DMA start after the kernel's first CYCLE read; dma_wait returning after the
# last flit is injected; the semaphore flit staged after that; sem_wait seeing
# it after it arrives; and the earliest a tile finishes after its own dma_wait.
V1_SETUP, V1_DONE, V1_SEM, V1_WAIT, V1_SELF = 20, 12, 20, 11, 47
JITTER, JITTER_RUNS = 3, 8
# Cannon: software of one hand-over (two dma_write, four sem_inc with their
# channel polls, four satisfied sem_waits, timestamps): about 140 instructions
# at the block multiply's CPI of about 1.35
HANDOVER = 190


class Grid:
    def __init__(self, nx, ny):
        self.nx, self.ny = nx, ny
        self.gx, self.gy = nx, ny + 1
        self.io = (0, ny)

    def plane(self, src, dst):
        """The plane every NIU picks: fewer hops, ties to NoC0 (noc_pkg::select_plane)."""
        hx, hy = (dst[0] - src[0]) % self.gx, (dst[1] - src[1]) % self.gy
        return 1 if (-hx) % self.gx + (-hy) % self.gy < hx + hy else 0

    def path(self, src, dst, plane=None):
        """Router outputs (x, y, plane, port) a flit takes from src to dst, ejection included."""
        p = self.plane(src, dst) if plane is None else plane
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

    def hops(self, src, dst, plane=None):
        return len(self.path(src, dst, plane)) - 1

    def neighbor(self, t, dx, dy):
        """Compute tile dx, dy away on the compute torus (the SDK's neighbor())."""
        return ((t[0] + dx) % self.nx, (t[1] + dy) % self.ny)


class Traffic:
    """Flits per router output and per NIU, per plane."""

    def __init__(self, g):
        self.g = g
        self.links, self.tx, self.rx = Counter(), Counter(), Counter()

    def add(self, src, dst, flits, plane=None):
        path = self.g.path(src, dst, plane)
        p = path[0][2]
        for hop in path:
            self.links[hop] += flits
        self.tx[(*src, p)] += flits
        self.rx[(*dst, p)] += flits

    def expected(self, ev, xy):
        """What counter event ev of tile xy must read."""
        for p in (0, 1):
            if ev == EV_NIU_TX[p]:
                return self.tx[(*xy, p)]
            if ev == EV_NIU_RX[p]:
                return self.rx[(*xy, p)]
            if EV_FLIT[p] <= ev < EV_FLIT[p] + 3:
                return self.links[(*xy, p, ev - EV_FLIT[p])]
        if ev == EV_NIU_ERR:
            return 0
        return None

    def per_plane(self):
        return [sum(v for k, v in self.links.items() if k[2] == p) for p in (0, 1)]


def add_launch_exit(tr, tiles):
    """DONE from every tile, and the host's stop writes (last tile first: each
    passes only tiles still running); both on the automatic plane."""
    for t in tiles:
        tr.add(t, tr.g.io, 1)
        tr.add(tr.g.io, t, 1)


def check_counters(rep, tr):
    ok = True
    for t in rep["tiles"]:
        xy, c = (t["x"], t["y"]), t["counters"]
        for k, ev in enumerate(rep["events"]):
            want = tr.expected(ev, xy)
            if want is not None and c[2 + k] != want:
                print(f"tile {xy}: counter {2 + k} (event {ev}) is {c[2 + k]}, model {want}")
                ok = False
    return ok


# ------------------------------------------------------------------ memcpy
def predict_memcpy(rep):
    g = Grid(rep["nx"], rep["ny"])
    forced = FORCED[rep.get("plane", "auto")]
    flits = rep["len"] // 16
    tiles = [(t["x"], t["y"]) for t in rep["tiles"]]
    partner = {(t["x"], t["y"]): tuple(t["partner"]) for t in rep["tiles"]}
    tr = Traffic(g)
    flows = Counter()       # (x, y, plane, port) -> data flows sharing it
    for t in tiles:
        tr.add(t, partner[t], flits + 1, forced)   # data and the semaphore increment
        for hop in g.path(t, partner[t], forced):
            flows[hop] += 1
    add_launch_exit(tr, tiles)
    worst, max_share = 0, 1
    for t in tiles:
        p = partner[t]
        share = max(flows[h] for h in g.path(t, p, forced) if h[3] != L) if t != p else 1
        max_share = max(max_share, share)
        worst = max(worst, SETUP + flits * share + ENDS + HOP * g.hops(t, p, forced) + TAIL)
    return tr, worst, max_share


def simulate_memcpy(rep, variant="base", jitter=0, seed=0):
    """Copy phase of one run of the report's memcpy in the flit-level model;
    each tile starts up to `jitter` cycles off its measured start."""
    s = NocSim.soc(rep["nx"], rep["ny"], variant=variant)
    forced = FORCED[rep.get("plane", "auto")]
    flits = rep["len"] // 16
    rng = random.Random(seed)
    senders, arrived, res = Counter(), Counter(), {}
    for t in rep["tiles"]:
        senders[tuple(t["partner"])] += 1
    for t in rep["tiles"]:
        xy, p = (t["x"], t["y"]), tuple(t["partner"])
        pl = s.plane(xy, p) if forced is None else forced
        niu = s.nius[xy]
        res[xy] = [None, None]                  # dma_wait returned, semaphore complete

        def rx(f, c, xy=xy):
            if f.tag == "sem":
                arrived[xy] += 1
                if arrived[xy] == senders[xy]:
                    res[xy][1] = c

        def done(c, xy=xy, niu=niu, p=p, pl=pl):
            res[xy][0] = c + V1_DONE
            s.at(c + V1_DONE + V1_SEM, lambda: niu.send(p, pl, "sem"))

        niu.on_rx = rx
        start = t["t"][0] + V1_SETUP + (rng.randint(-jitter, jitter) if jitter else 0)
        s.at(max(start, 0), lambda niu=niu, p=p, pl=pl, done=done: niu.dma_write(p, flits, pl, "data", done))
    while any(None in v for v in res.values()):
        s.step()
        if s.cycle > 1000 * flits:
            raise RuntimeError("memcpy model: no progress")
    t0 = min(t["t"][0] for t in rep["tiles"])
    return max(max(sem + V1_WAIT, t1 + V1_SELF) for t1, sem in res.values()) - t0


def memcpy_range(rep, variant="base"):
    """Copy phase over JITTER_RUNS runs: the measured start times, then jittered ones."""
    runs = [simulate_memcpy(rep, variant, JITTER if k else 0, k) for k in range(JITTER_RUNS)]
    return runs[0], min(runs), statistics.median(runs), max(runs)


def check_memcpy(rep, tolerance):
    tr, bound, share = predict_memcpy(rep)
    ok = check_counters(rep, tr)
    meas = rep["copy_cycles"]
    exact, lo, med, hi = memcpy_range(rep)
    err = (med - meas) / meas
    pp = tr.per_plane()
    print(f"memcpy {rep['nx']}x{rep['ny']}, {rep['len']} B per tile, shift {tuple(rep['shift'])}, plane "
          f"{rep.get('plane', 'auto')}: copy phase {meas} cycles, model {exact} at the measured starts, "
          f"{lo}..{hi} median {med:.0f} ({err:+.1%}) with +-{JITTER} cycles of start jitter, even-split bound "
          f"{bound}; {sum(tr.links.values())} router-output flits (NoC0 {pp[0]}, NoC1 {pp[1]}), "
          f"per-link counts {'exact' if ok else 'MISMATCH'}")
    if not lo * (1 - tolerance) <= meas <= hi * (1 + tolerance):
        print(f"copy phase outside the model's range widened by {tolerance:.0%}")
        ok = False
    return ok


# ------------------------------------------------------------------ Cannon
def block_cycles(cal):
    """One block multiply on the calibration run (1x1: one step)."""
    t = cal["tiles"][0]["t"]
    return t[2] - t[1]


def predict_cannon(rep, cal):
    g = Grid(rep["nx"], rep["ny"])
    p, b = rep["nx"], rep["b"]
    if cal["b"] != b or cal["nx"] != 1:
        raise SystemExit(f"calibration is a {cal['nx']}x{cal['ny']} run with b={cal['b']}, need 1x1 with b={b}")
    forced = FORCED[rep["plane"]]
    tiles = [(t["x"], t["y"]) for t in rep["tiles"]]
    tr = Traffic(g)
    flits = b * b * 4 // 16
    for t in tiles:
        west, east, north, south = (g.neighbor(t, -1, 0), g.neighbor(t, 1, 0), g.neighbor(t, 0, -1),
                                    g.neighbor(t, 0, 1))
        for _ in range(p - 1):
            tr.add(t, west, flits + 1, forced)     # A block, its arrival semaphore
            tr.add(t, north, flits + 1, forced)    # B block
            tr.add(t, east, 1, forced)             # buffer free
            tr.add(t, south, 1, forced)
    add_launch_exit(tr, tiles)
    t_mac = block_cycles(cal)
    trip = max(ENDS + HOP * max(g.hops(t, g.neighbor(t, -1, 0), forced), g.hops(t, g.neighbor(t, 0, -1), forced))
               for t in tiles)
    kernel = p * t_mac + (p - 1) * (HANDOVER + 2 * trip)
    return tr, kernel, t_mac


def check_cannon(rep, cal, tolerance):
    tr, kernel, t_mac = predict_cannon(rep, cal)
    ok = check_counters(rep, tr)
    p = rep["nx"]
    steps = [t["t"][4 * k + 2] - t["t"][4 * k + 1] for t in rep["tiles"] for k in range(p)]
    compute = sum(steps) / len(steps)
    meas = rep["kernel_cycles"]
    err, cerr = (kernel - meas) / meas, (t_mac - compute) / compute
    pp = tr.per_plane()
    print(f"cannon {p}x{p}, N={rep['n']} (b={rep['b']}), plane {rep['plane']}: kernel {meas} cycles, model {kernel} "
          f"({err:+.1%}); block multiply {compute:.0f} cycles, calibrated {t_mac} ({cerr:+.1%}); "
          f"{sum(tr.links.values())} router-output flits (NoC0 {pp[0]}, NoC1 {pp[1]}), "
          f"per-link counts {'exact' if ok else 'MISMATCH'}")
    for what, e in (("kernel time", err), ("block multiply", cerr)):
        if abs(e) > tolerance:
            print(f"{what} outside the {tolerance:.0%} tolerance")
            ok = False
    return ok


# ------------------------------------------------------------------ runs
def run_host(cmd, report):
    print("$", " ".join(cmd), flush=True)
    if subprocess.run(cmd + [f"--report={report}"]).returncode != 0:
        return None
    return json.load(open(report))


def keep(rep, name, report_dir):
    os.makedirs(report_dir, exist_ok=True)
    with open(os.path.join(report_dir, name), "w") as f:
        json.dump(rep, f, indent=1)


def shift_tag(shift):
    return shift.replace(",", "_").replace("-", "m")


def run_memcpy(host, elf, shift, length, plane, rest, d):
    return run_host([host, elf, f"--shift={shift}", f"--len={length}", f"--plane={plane}"] + rest,
                    os.path.join(d, f"memcpy_{shift_tag(shift)}_{plane}.json"))


def run_cannon(host, cal_host, elf, n, plane, rest, d):
    """The grid run, then the 1x1 calibration for its block size."""
    rep = run_host([host, elf, f"--n={n}", f"--plane={plane}"] + rest, os.path.join(d, f"cannon_{plane}.json"))
    if rep is None:
        return None, None
    cal = run_host([cal_host, elf, f"--n={rep['b']}"] + rest, os.path.join(d, f"cannon_cal_{rep['b']}.json"))
    return rep, cal


def planes(args, rest):
    """The same programs with all their traffic on NoC0 (a one-plane NoC) and on
    both planes; a markdown table on stdout and in the report directory."""
    b = args.build
    rows = []
    with tempfile.TemporaryDirectory() as d:
        for shift in args.shifts:
            r = {}
            for plane in ("noc0", "auto"):
                rep = run_memcpy(os.path.join(b, "tb/soc/host_memcpy_4x4"), os.path.join(b, "sw/memcpy.elf"), shift,
                                 16384, plane, rest, d)
                if rep is None:
                    return 1
                tr, _, _ = predict_memcpy(rep)
                hops = sum(tr.links.values())
                r[plane] = (rep["copy_cycles"], hops)
            rows.append((f"memcpy 16 KB, shift ({shift})", "copy phase", r))
        for n in args.cannon:
            r = {}
            for plane in ("noc0", "auto"):
                rep, cal = run_cannon(os.path.join(b, "tb/soc/host_cannon_4x4"), os.path.join(b, "tb/soc/host_cannon_1x1"),
                                      os.path.join(b, "sw/cannon.elf"), n, plane, rest, d)
                if rep is None or cal is None:
                    return 1
                tr, _, _ = predict_cannon(rep, cal)
                r[plane] = (rep["kernel_cycles"], sum(tr.links.values()))
            rows.append((f"Cannon N={n}", "kernel", r))
    out = ["| Program on 4x4 | Phase | One plane (NoC0) | Two planes | Speedup | Router-output flits, one / two |",
           "| --- | --- | --- | --- | --- | --- |"]
    for name, phase, r in rows:
        (c1, f1), (c2, f2) = r["noc0"], r["auto"]
        out.append(f"| {name} | {phase} | {c1} | {c2} | {c1 / c2:.2f}x | {f1} / {f2} |")
    table = "\n".join(out)
    print(table)
    os.makedirs(args.report_dir, exist_ok=True)
    with open(os.path.join(args.report_dir, "planes.md"), "w") as f:
        f.write(table + "\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("memcpy", help="check a memcpy report")
    a.add_argument("--report", required=True)
    a.add_argument("--tolerance", type=float, default=0.05)
    b = sub.add_parser("run-memcpy", help="run host_memcpy, then check its report")
    b.add_argument("--host", required=True)
    b.add_argument("--elf", required=True)
    b.add_argument("--shift", default="1,1")
    b.add_argument("--len", default="16384")
    b.add_argument("--plane", default="auto", choices=FORCED)
    b.add_argument("--tolerance", type=float, default=0.05)
    b.add_argument("--report-dir", default=REPORTS, help="where the report is kept")
    c = sub.add_parser("cannon", help="check a Cannon report against its 1x1 calibration")
    c.add_argument("--report", required=True)
    c.add_argument("--calibration", required=True)
    c.add_argument("--tolerance", type=float, default=0.15)
    e = sub.add_parser("run-cannon", help="run host_cannon on the grid and on 1x1, then check")
    e.add_argument("--host", required=True)
    e.add_argument("--cal-host", required=True, help="the 1x1 host_cannon, for the calibration")
    e.add_argument("--elf", required=True)
    e.add_argument("--n", default="64")
    e.add_argument("--plane", default="auto", choices=FORCED)
    e.add_argument("--tolerance", type=float, default=0.15)
    e.add_argument("--report-dir", default=REPORTS)
    f = sub.add_parser("planes", help="memcpy and Cannon on one plane and on two (4x4)")
    f.add_argument("--build", default=os.path.join(ROOT, "build"))
    f.add_argument("--shifts", nargs="*", default=["1,1", "-1,-1", "1,-1", "2,2"])
    f.add_argument("--cannon", nargs="*", type=int, default=[64])
    f.add_argument("--report-dir", default=REPORTS)
    args, rest = ap.parse_known_args()

    if args.cmd == "memcpy":
        return 0 if check_memcpy(json.load(open(args.report)), args.tolerance) else 1
    if args.cmd == "cannon":
        return 0 if check_cannon(json.load(open(args.report)), json.load(open(args.calibration)),
                                 args.tolerance) else 1
    if args.cmd == "planes":
        return planes(args, rest)
    with tempfile.TemporaryDirectory() as d:
        if args.cmd == "run-memcpy":
            rep = run_memcpy(args.host, args.elf, args.shift, args.len, args.plane, rest, d)
            if rep is None:
                return 1
            keep(rep, f"memcpy_{rep['nx']}x{rep['ny']}_s{shift_tag(args.shift)}_{args.plane}.json", args.report_dir)
            return 0 if check_memcpy(rep, args.tolerance) else 1
        rep, cal = run_cannon(args.host, args.cal_host, args.elf, args.n, args.plane, rest, d)
        if rep is None or cal is None:
            return 1
        keep(rep, f"cannon_{rep['nx']}x{rep['ny']}_n{rep['n']}_{args.plane}.json", args.report_dir)
        keep(cal, f"cannon_1x1_n{cal['n']}.json", args.report_dir)
        return 0 if check_cannon(rep, cal, args.tolerance) else 1


if __name__ == "__main__":
    sys.exit(main())
