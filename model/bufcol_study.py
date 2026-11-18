#!/usr/bin/env python3
"""Buffer columns, bandwidth study (SPEC.md, Open decisions; M4). 8x8 compute
tiles plus two buffer columns at x = NX/2 and NX + 1 (GX = NX + 2); a buffer
tile has no core, so it only answers READ_REQs, in order, from its TX-reply
engine (one flit per cycle), or with two engines, one per plane. Every compute
tile fetches the same amount: from the nearest buffer tile in its row, or
interleaved over all buffer tiles. Prints the aggregate bandwidth.

  model/bufcol_study.py
"""
import collections
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from noc_sim import NocSim, Flit  # noqa: E402
NX, NY = 8, 8
GX, GY = NX + 2, NY + 1
BUF_X = (NX // 2, NX + 1)
compute = [(x, y) for y in range(NY) for x in range(GX) if x not in BUF_X]
bufs = [(x, y) for y in range(NY) for x in BUF_X]

def ring_dist(a, b, n):
    d = (b - a) % n
    return min(d, n - d)

def run(mode, per_client_flits, engines=1):
    sim = NocSim(GX, GY, True, (0, 1))
    # requests per buffer tile, served in order (TX-reply), one engine per plane if engines == 2
    queues = collections.defaultdict(list)
    for c in compute:
        if mode == "row":
            b = min((bx for bx in BUF_X), key=lambda bx: ring_dist(c[0], bx, GX))
            queues[(b, c[1])].append((c, per_client_flits))
        else:
            for b in bufs:
                queues[b].append((c, per_client_flits // len(bufs)))
    done = {}
    total = sum(n for q in queues.values() for _, n in q)
    got = [0]
    def rx(f, c):
        got[0] += 1
        done[f.tag] = c
    for n in sim.nius.values():
        n.on_rx = rx
        n.per_plane = engines == 2
        n.stage_depth = 8
    # each engine: a deque of (client, flits) served in order; one flit per cycle into the NIU's injection
    engines_state = []
    for b, q in queues.items():
        if engines == 1:
            engines_state.append([b, q, None])
        else:   # split requests by plane
            for p in (0, 1):
                engines_state.append([b, [r for r in q if sim.plane(b, r[0]) == p], p])
    cur = {id(e): [0, 0] for e in engines_state}
    while got[0] < total:
        for e in engines_state:
            b, q, p = e
            st = cur[id(e)]
            niu = sim.nius[b]
            if st[0] < len(q) and len([f for f in niu.stage if (p is None or f.plane == p)]) < 4:
                c, n = q[st[0]]
                niu.stage.append(Flit(c[0], c[1], sim.plane(b, c), c))
                st[1] += 1
                if st[1] == n:
                    st[0] += 1; st[1] = 0
        sim.step()
        if sim.cycle > 400000: raise SystemExit("stuck")
    return sim.cycle, total

for mode in ("row", "interleaved"):
    for eng in (1, 2):
        cyc, total = run(mode, 256, eng)
        print(f"{mode:12s} engines/buffer tile {eng}: {total} flits ({total*16//1024} KB) in {cyc} cycles = {total/cyc:.2f} flits/cycle aggregate, {total/cyc*16/64:.2f} B/cycle per compute tile", flush=True)
