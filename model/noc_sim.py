#!/usr/bin/env python3
"""Performance model v1 (SPEC.md, M4): the two-plane torus flit by flit.

Every router is the cycle-level behaviour of rtl/noc/noc_router.sv (and of
tb/models/router_model.h, which tb_router checks against the RTL):

  cycle 1  the flit is at the head of its input queue; the output's round-robin
           grants it if the downstream queue has the credits it needs (two for
           a flit entering a ring: injection into X or Y, the X-to-Y turn; one
           otherwise)
  cycle 2  the flit is in the output register; at the next edge it is in the
           downstream queue, and the freed slot's credit is in a register that
           reaches the upstream counter at the edge after

Link slices add a cycle each way. NIUs inject one flit per cycle per plane
(staging FIFO in front, credits towards the router's local queue) and drain
their ejection queues one flit per cycle per plane (the bank arbiter's
starvation bound keeps ejection draining; bank conflicts are not modelled).

Head-of-line variants (SPEC.md, Open issues):

  base  one FIFO per input (the RTL today)
  B     one buffer per input, searchable: each output takes the oldest flit
        bound for it; one read per input per cycle (a separable allocator:
        outputs grant round-robin, an input with several grants takes its
        oldest flit, the other outputs idle that cycle)
  C     the X input split into a continue queue (flits staying on X) and a
        leave queue (turning to Y or ejecting), each DEPTH deep with its own
        credits; the bubble rule guards the continue queues (the X ring
        cycle); a flit that leaves X at the next router needs one credit

and, combinable with base and C, two other ways to keep the rings
deadlock-free:

  cbs   critical bubble scheme: one marked free slot per ring; entering flits
        need one free slot that is not the marked one, a continuing flit that
        takes the marked slot moves the mark upstream (a flag on its credit)
  vcs   dateline VCs instead of the bubble rule: a flit enters each ring on
        VC0 and moves to VC1 on that ring's wraparound link; every move needs
        one credit

The results and the decision (no router change in M4) are in SPEC.md, Open
issues. check-rtl runs tb_noc's stream mode and compares every flit.
"""
import argparse
import json
import os
import random
import subprocess
import sys
import tempfile
from collections import deque

L, X, Y = 0, 1, 2
XL = 3          # option C: the X input's leave queue
XV1, YV1 = 4, 5  # dateline VCs: the X (continue) and Y queues of VC1


def credit_rtt(slices):
    return 4 + 2 * slices


class Flit:
    __slots__ = ("dx", "dy", "plane", "tag", "on_inj", "t_inj")

    def __init__(self, dx, dy, plane, tag, on_inj=None):
        self.dx, self.dy, self.plane, self.tag, self.on_inj = dx, dy, plane, tag, on_inj
        self.t_inj = None


def route(x, y, dx, dy):
    if x != dx:
        return X
    if y != dy:
        return Y
    return L


class Router:
    __slots__ = ("x", "y", "p", "nx", "ny", "q", "qport", "credits", "crit", "out", "cred_q", "cred_crit", "rr", "nq",
                 "busy")

    def __init__(self, x, y, p, gx, gy, nq, depth):
        step = 1 if p == 0 else -1
        self.x, self.y, self.p = x, y, p
        self.nx, self.ny = (x + step) % gx, (y + step) % gy    # coordinates one hop on along X and Y
        self.nq = nq
        self.q = [deque() for _ in range(nq)]
        self.qport = [L, X, Y, X, X, Y][:nq]
        # credits[o][k]: free slots in queue k of the router (or NIU) behind output o
        self.credits = [[0] * 6 for _ in range(3)]
        self.credits[L][0] = depth
        for k in (X, XL, XV1):
            self.credits[X][k] = depth
        for k in (Y, YV1):
            self.credits[Y][k] = depth
        # crit[o][k]: CBS, the ring's critical bubble is one of those free slots
        self.crit = [[False] * len(c) for c in self.credits]
        self.out = [None, None, None]       # (flit, la at the next router, downstream queue)
        self.cred_q = [False] * nq          # credit register per input queue
        self.cred_crit = [False] * nq       # ... and whether the slot it frees is the critical bubble
        self.rr = [0, 0, 0]
        self.busy = False


class Niu:
    """One tile's network side: per plane an injection port and an ejection queue."""

    def __init__(self, sim, x, y, depth, stage):
        self.sim, self.x, self.y = sim, x, y
        self.stage_depth = stage
        self.stage = deque()                # (flit) staged by the DMA, any plane
        self.dma = deque()                  # active transfers: [flits left, dst, plane, tag, on_done]
        self.credits = [depth, depth]
        self.out = [None, None]             # injection register per plane: (flit, la)
        self.ej = [deque(), deque()]
        self.cred_q = [False, False]
        self.on_rx = None                   # callback(flit, cycle)
        self.ch_rr = 0
        self.tx_count = [0, 0]
        self.rx_count = [0, 0]

    def dma_write(self, dst, flits, plane, tag, on_done=None):
        """Queue a DMA WRITE of `flits` flits (channels interleave flit by flit);
        on_done(cycle) when its last flit is injected."""
        self.dma.append([flits, dst, plane, tag, on_done])

    def send(self, dst, plane, tag, on_done=None):
        self.dma_write(dst, 1, plane, tag, on_done)


class NocSim:
    """A router grid gx x gy: compute tiles everywhere, or (io_row) in rows
    0..gy-2 with the I/O row gy-1 holding the I/O tile at (0, gy-1) and
    router-only stops. planes lists the ring directions present (0: NoC0 east
    and south, 1: NoC1 west and north); flits carry the direction."""

    def __init__(self, gx, gy, io_row=True, planes=(0, 1), variant="base", slices=0, stage=4, ej_stall=0.0,
                 seed=1, bubble=True, cbs=False, vcs=False):
        self.gx, self.gy, self.io_row, self.planes = gx, gy, io_row, tuple(planes)
        self.nx, self.ny = gx, gy - 1 if io_row else gy
        self.variant = variant
        self.bubble = bubble
        self.cbs = cbs
        # Dateline VCs instead of the bubble rule: a flit enters each ring on
        # VC0 and moves to VC1 when it crosses that ring's wraparound link
        self.vcs = vcs
        self.slices = slices
        self.depth = credit_rtt(slices) + 1
        nq = 6 if vcs else 4 if variant == "C" else 3
        self.routers = {}
        for p in self.planes:
            for y in range(gy):
                for x in range(gx):
                    self.routers[(x, y, p)] = Router(x, y, p, gx, gy, nq, self.depth)
            if cbs:
                # one critical bubble per ring, in the queue behind router 0's output
                for y in range(gy):
                    self.routers[(0, y, p)].crit[X][X] = True
                for x in range(gx):
                    self.routers[(x, 0, p)].crit[Y][Y] = True
        self.nius = {}
        for y in range(gy):
            for x in range(gx):
                if not io_row or y < gy - 1 or x == 0:
                    self.nius[(x, y)] = Niu(self, x, y, self.depth, stage)
        self.cycle = 0
        self.timers = []                    # (cycle, fn)
        self.links = {}                     # (x, y, plane, port) -> flits sent
        self.inflight = []                  # with slices: (arrive cycle, deliveries, credits)
        self.pending = 0                    # flits injected and not yet ejected
        self.ej_stall = ej_stall            # probability an ejection queue does not drain in a cycle
        self.rng = random.Random(seed)
        self.events = None                  # a dict to count router stall events (base, C) into

    @classmethod
    def soc(cls, nx, ny, **kw):
        """The SoC's network: nx x ny compute tiles, the I/O row, both planes."""
        return cls(nx, ny + 1, **kw)

    # ------------------------------------------------------------- helpers
    def plane(self, src, dst):
        """noc_pkg::select_plane: fewer hops, ties to NoC0 (the only plane if one)."""
        if len(self.planes) == 1:
            return self.planes[0]
        hx, hy = (dst[0] - src[0]) % self.gx, (dst[1] - src[1]) % self.gy
        return 1 if (-hx) % self.gx + (-hy) % self.gy < hx + hy else 0

    def at(self, cycle, fn):
        self.timers.append((cycle, fn))

    def _queue_of(self, port, la, vc=0):
        if self.variant == "C" and port == X and la != X:
            return XL
        if vc:
            return XV1 if port == X else YV1
        return port

    def _eligible(self, r, qport, o, dq):
        """Whether a flit from an input on port qport may go out on o into
        downstream queue dq now. A flit entering a ring queue (injection, the
        X-to-Y turn) must leave a free slot behind it: two credits (bubble
        rule), or with CBS one that is not the ring's critical bubble. The leave
        queues of option C are not part of the ring cycle."""
        free = r.credits[o][dq]
        if self.vcs or o == L or not self.bubble or not ((o == X and qport == L) or (o == Y and qport != Y)) or dq == XL:
            return free >= 1
        if self.cbs:
            return free >= 1 + r.crit[o][dq]
        return free >= 2

    def _next(self, r, o, f, k=0):
        """(la at the next router, its queue) for flit f from queue k leaving r on output o."""
        if o == X:
            la = route(r.nx, r.y, f.dx, f.dy)
            vc = self.vcs and (k == XV1 or r.nx == (0 if r.p == 0 else self.gx - 1))
            return la, self._queue_of(X, la, vc)
        if o == Y:
            vc = self.vcs and (k == YV1 or r.ny == (0 if r.p == 0 else self.gy - 1))
            return route(r.x, r.ny, f.dx, f.dy), self._queue_of(Y, None, vc)
        return L, 0

    # ------------------------------------------------------------- allocation
    def _allocate(self, r):
        """Grants: list of (queue, index in queue, output, la next, downstream queue)."""
        grants = []
        q, credits, variant = r.q, r.credits, self.variant
        if variant == "B":
            # every output: per queue the oldest flit bound for it
            offers = {}                     # o -> {queue: (idx, la, dq)}
            for k in range(r.nq):
                seen = 0
                for idx, (f, la) in enumerate(q[k]):
                    if seen >> la & 1:
                        continue
                    seen |= 1 << la
                    nla, dq = self._next(r, la, f, k)
                    if self._eligible(r, r.qport[k], la, dq):
                        offers.setdefault(la, {})[k] = (idx, nla, dq)
                    if seen == 7:
                        break
            taken = {}
            for o in (L, X, Y):
                if o not in offers:
                    continue
                for s in range(1, r.nq + 1):
                    k = (r.rr[o] + s) % r.nq
                    if k in offers[o]:
                        idx, nla, dq = offers[o][k]
                        if k not in taken or idx < taken[k][1]:
                            taken[k] = (o, idx, nla, dq)
                        break
            for k, (o, idx, nla, dq) in taken.items():
                grants.append((k, idx, o, nla, dq))
            return grants
        heads = []
        ev = self.events
        for k in range(r.nq):
            if q[k]:
                f, la = q[k][0]
                nla, dq = self._next(r, la, f, k)
                if self._eligible(r, r.qport[k], la, dq):
                    heads.append((k, la, nla, dq))
                elif ev is not None:
                    key = (r.x, r.y, r.p)
                    if credits[la][dq] == 0:
                        ev[key + ("cstall", la)] = ev.get(key + ("cstall", la), 0) + 1
                    else:
                        ev[key + ("bhold", la)] = ev.get(key + ("bhold", la), 0) + 1
        if not heads:
            return grants
        for o in (L, X, Y):
            best, bd = None, None
            for k, la, nla, dq in heads:
                if la != o:
                    continue
                d = (k - r.rr[o] - 1) % r.nq
                if bd is None or d < bd:
                    best, bd = (k, 0, o, nla, dq), d
            if best:
                grants.append(best)
        if ev is not None:
            won = {g[0] for g in grants}
            for k, la, nla, dq in heads:
                if k not in won:
                    key = (r.x, r.y, r.p, "arbloss", r.qport[k])
                    ev[key] = ev.get(key, 0) + 1
        return grants

    # ------------------------------------------------------------- one cycle
    def step(self):
        c = self.cycle
        if self.timers:
            due = [t for t in self.timers if t[0] <= c]
            if due:
                self.timers = [t for t in self.timers if t[0] > c]
                for _, fn in due:
                    fn()
        routers, nius = self.routers, self.nius
        # 1. decisions from the current state
        rgrants = []
        for r in routers.values():
            if r.busy:
                rgrants.append((r, self._allocate(r)))
        nsends = [n for n in nius.values() if n.stage and n.credits[n.stage[0].plane] > 0]
        # 2. what the registers deliver at this edge; link slices delay the
        # flits and credits between routers, not the local ports
        deliveries = []                     # (router, queue, flit, la) or (niu, plane, flit, None)
        credit_rx = []                      # (router or niu, output, queue, critical)
        ring_d, ring_c = (deliveries, credit_rx) if not self.slices else ([], [])
        for r in routers.values():
            for o in (L, X, Y):
                v = r.out[o]
                if v is None:
                    continue
                f, la, dq = v
                if o == L:
                    deliveries.append((nius[(r.x, r.y)], r.p, f, None))
                elif o == X:
                    ring_d.append((routers[(r.nx, r.y, r.p)], dq, f, la))
                else:
                    ring_d.append((routers[(r.x, r.ny, r.p)], dq, f, la))
            for k in range(r.nq):
                if r.cred_q[k]:
                    port = r.qport[k]
                    if port == L:
                        credit_rx.append((nius[(r.x, r.y)], r.p, None, False))
                    elif port == X:
                        up = routers[((r.x - (1 if r.p == 0 else -1)) % self.gx, r.y, r.p)]
                        ring_c.append((up, X, k, r.cred_crit[k]))
                    else:
                        up = routers[(r.x, (r.y - (1 if r.p == 0 else -1)) % self.gy, r.p)]
                        ring_c.append((up, Y, k, r.cred_crit[k]))
        for n in nius.values():
            for p in self.planes:
                if n.out[p] is not None:
                    f, la = n.out[p]
                    deliveries.append((routers[(n.x, n.y, p)], L, f, la))
                if n.cred_q[p]:
                    credit_rx.append((routers[(n.x, n.y, p)], L, 0, False))
        if self.slices:
            self.inflight.append((c + self.slices, ring_d, ring_c))
            while self.inflight and self.inflight[0][0] <= c:
                _, d, cr = self.inflight.pop(0)
                deliveries += d
                credit_rx += cr
        # 3. the edge
        for r in routers.values():
            r.out = [None, None, None]
            r.cred_q = [False] * r.nq
            r.cred_crit = [False] * r.nq
        for r, grants in rgrants:
            for k, idx, o, nla, dq in grants:
                if idx == 0:
                    f, _ = r.q[k].popleft()
                else:
                    f, _ = r.q[k][idx]
                    del r.q[k][idx]
                r.out[o] = (f, nla, dq)
                if r.crit[o][dq] and r.credits[o][dq] == 1:
                    # a flit continuing on its ring takes the critical bubble:
                    # the slot it frees here becomes the critical one
                    r.crit[o][dq] = False
                    r.cred_crit[k] = True
                r.credits[o][dq] -= 1
                r.cred_q[k] = True
                r.rr[o] = k
                key = (r.x, r.y, r.p, o)
                self.links[key] = self.links.get(key, 0) + 1
        for n in nius.values():
            n.out = [None, None]
            for p in self.planes:
                if n.cred_q[p]:
                    n.cred_q[p] = False
                if n.ej[p] and not (self.ej_stall and self.rng.random() < self.ej_stall):
                    f = n.ej[p].popleft()
                    n.cred_q[p] = True
                    n.rx_count[p] += 1
                    self.pending -= 1
                    if n.on_rx:
                        n.on_rx(f, c)
        for n in nsends:
            f = n.stage.popleft()
            n.out[f.plane] = (f, route(n.x, n.y, f.dx, f.dy))
            n.credits[f.plane] -= 1
            n.tx_count[f.plane] += 1
            f.t_inj = c
            self.pending += 1
            if f.on_inj:
                self.at(c + 1, lambda f=f, c=c: f.on_inj(c))
        for tgt, k, f, la in deliveries:
            if la is None:
                tgt.ej[k].append(f)
            else:
                tgt.q[k].append((f, la))
        for tgt, o, k, crit in credit_rx:
            if k is None:
                tgt.credits[o] += 1
            else:
                tgt.credits[o][k] += 1
                if crit:
                    tgt.crit[o][k] = True
        # DMA: one flit per cycle into staging, channels interleaved
        for n in nius.values():
            if n.dma and len(n.stage) < n.stage_depth:
                n.ch_rr = (n.ch_rr + 1) % len(n.dma)
                t = n.dma[n.ch_rr]
                t[0] -= 1
                n.stage.append(Flit(t[1][0], t[1][1], t[2], t[3], t[4] if t[0] == 0 else None))
                if t[0] == 0:
                    del n.dma[n.ch_rr]
        for r in routers.values():
            r.busy = any(r.q) or any(v is not None for v in r.out)
        self.cycle = c + 1


def streams(gx, gy, io_row, planes, shift, n, variant="base", slices=0, jitter=0, seed=1, vcs=False):
    """tb_noc's stream mode: every compute endpoint sends n flits to the one
    shift away on the compute torus, all from cycle 0 (or each from a random
    cycle up to `jitter`), ejection never stalls.
    Returns {(src, seq): (dst, plane, sent, ejected)} with nodes as y * gx + x."""
    sim = NocSim(gx, gy, io_row, planes, variant, slices, vcs=vcs)
    rng = random.Random(seed)
    out = {}
    for (x, y), niu in sim.nius.items():
        if y >= sim.ny:
            continue
        d = ((x + shift[0]) % sim.nx, (y + shift[1]) % sim.ny)
        p = sim.plane((x, y), d)
        niu.on_rx = lambda f, c: out.__setitem__(f.tag, (f.dx + gx * f.dy, f.plane, f.t_inj, c))
        niu.stage_depth = 1 << 30
        flits = [Flit(d[0], d[1], p, (y * gx + x, k)) for k in range(n)]
        if jitter:
            sim.at(rng.randint(0, jitter), lambda niu=niu, flits=flits: niu.stage.extend(flits))
        else:
            niu.stage.extend(flits)
    total = n * sum(1 for (x, y) in sim.nius if y < sim.ny)
    while len(out) < total:
        sim.step()
        if sim.cycle > 100 * total:
            raise RuntimeError("no progress")
    return out, sim


def soak(gx, gy, io_row, planes, variant, cycles, ej_stall, seed=1, bubble=True, cbs=False, vcs=False,
         watchdog=2000):
    """Every endpoint always has a flit to a random endpoint, ejection stalls
    with probability ej_stall. Returns (delivered, deadlocked)."""
    sim = NocSim(gx, gy, io_row, planes, variant, ej_stall=ej_stall, seed=seed, bubble=bubble, cbs=cbs,
                 vcs=vcs)
    rng = random.Random(seed)
    eps = list(sim.nius)
    got = [0]
    last = [0]

    def rx(f, c):
        got[0] += 1
        last[0] = c

    for niu in sim.nius.values():
        niu.on_rx = rx
    while sim.cycle < cycles:
        for (x, y), niu in sim.nius.items():
            if len(niu.stage) < 2:
                d = eps[rng.randrange(len(eps))]
                niu.stage.append(Flit(d[0], d[1], sim.plane((x, y), d), None))
        sim.step()
        if sim.cycle - last[0] > watchdog:
            return got[0], True
    return got[0], False


def check_rtl(tb, shift, n, seed):
    """Run tb_noc's stream mode and compare every flit's send and ejection cycle."""
    with tempfile.TemporaryDirectory() as d:
        report = os.path.join(d, "streams.json")
        cmd = [tb, f"--seed={seed}", f"--streams={shift[0]},{shift[1]},{n}", f"--report={report}"]
        print("$", " ".join(cmd), flush=True)
        if subprocess.run(cmd).returncode != 0:
            return False
        r = json.load(open(report))
    planes = (0, 1) if r["planes"] == 2 else (r["dir"],)
    rtl, seq = {}, {}
    for _, src, dst, pl, sent, ej in r["flits"]:
        k = seq.get(src, 0)
        seq[src] = k + 1
        rtl[(src, k)] = (dst, planes[pl], sent, ej)
    ok = True
    for variant in ("base",):
        out, _ = streams(r["gx"], r["gy"], bool(r["io_row"]), planes, r["shift"], r["n"], variant, r["slice"])
        bad = sorted((k for k in rtl if rtl[k] != out.get(k)), key=lambda k: rtl[k][2])
        end = max(v[3] for v in rtl.values()) + 1
        print(f"{r['gx']}x{r['gy']}{' io_row' if r['io_row'] else ''} planes {planes} slice {r['slice']} "
              f"shift {tuple(r['shift'])} x {r['n']}: RTL {end} cycles, model {max(v[3] for v in out.values()) + 1}, "
              f"{len(rtl)} flits, {len(bad)} differ in send or ejection cycle")
        for k in bad[:5]:
            print(f"  flit {k[1]} from node {k[0]}: RTL (dst, plane, sent, ejected) {rtl[k]}, model {out.get(k)}")
        ok &= not bad
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("check-rtl", help="cycle-exact check against tb_noc's stream mode")
    a.add_argument("--tb", required=True, help="a tb_noc binary")
    a.add_argument("--shift", default="2,2")
    a.add_argument("--n", type=int, default=256)
    a.add_argument("--seed", default="1")
    args = ap.parse_args()
    if args.cmd == "check-rtl":
        return 0 if check_rtl(args.tb, tuple(int(v) for v in args.shift.split(",")), args.n, args.seed) else 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
