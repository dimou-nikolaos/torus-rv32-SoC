#!/usr/bin/env python3
"""Generate an animated SVG of the torus folding into its physical layout.

Topology as in SPEC.md (Baseline architecture):
  - NX x NY compute tiles at x in [0, NX), y in [0, NY);
  - the I/O row at y = NY: the I/O tile at (0, NY), router-only stops elsewhere;
  - optional buffer columns at x = NX/2 and x = NX + 1 (router grid NX + 2 wide).

Each ring is folded once and interleaved, which keeps every link, wraparound
included, at most two tile pitches long. In y the I/O row is rotated to the top
edge and stays there while the compute rows fold onto each other (physical
order NY, 0, NY-1, 1, ...); in x the whole ring folds, starting from a rotation
that puts the buffer columns, if any, on the two edges. The animation shows the
logical grid, the ring rotation, the two folds, the layer slide and the
stretch to the die.

Only CSS keyframes are used (no JavaScript), so the SVG plays when embedded in
a GitHub README as an image. --frame T writes a static frame at time T seconds
instead, for checking the geometry with an SVG rasterizer.

  python3 scripts/gen_fold_svg.py --nx 8 --ny 8 -o docs/img/torus_fold.svg
  python3 scripts/gen_fold_svg.py --nx 8 --ny 8 --buffer-cols -o docs/img/torus_fold_buffers.svg
"""
import argparse
import math

LIGHT = dict(bg="#ffffff", text="#1f1f1f", sub="#6b6b6b")
DARK = dict(bg="#0d1117", text="#e6edf3", sub="#9da7b3")

# Fill / border per tile class (light, dark)
CLASSES = {
    "q0": (("#eeedfe", "#534ab7"), ("#2b2a4a", "#afa9ec")),  # stays, stays
    "q1": (("#e1f5ee", "#0f6e56"), ("#11332a", "#5dcaa5")),  # folds in x
    "q2": (("#e6f1fb", "#185fa5"), ("#132b44", "#85b7eb")),  # folds in y
    "q3": (("#fbeaf0", "#993556"), ("#3d1a26", "#ed93b1")),  # folds in both
    "io": (("#faeeda", "#854f0b"), ("#3d2a0c", "#ef9f27")),  # I/O tile
    "rt": (("#f6f6f4", "#a3a29c"), ("#1a1d22", "#5c5f66")),  # router-only stop
    "buf": (("#f1efe8", "#5f5e5a"), ("#2c2c2a", "#b4b2a9")),  # buffer tile
}


def ease(k):
    k = max(0.0, min(1.0, k))
    return 2 * k * k if k < 0.5 else 1 - (-2 * k + 2) ** 2 / 2


class Ring:
    """One dimension: ring of length n, rotated so position r comes first.

    The first `head` positions after the rotation (the I/O row) stay where they
    are; the remaining block of n - head positions is folded in half onto
    itself and interleaved behind them.
    """

    def __init__(self, n, r, head=0):
        self.n, self.r, self.head = n, r, head
        self.m = n - head               # folding block
        self.c = (self.m - 1) / 2       # fold line inside the block

    def k(self, i):
        """Ring position counted from the rotation start."""
        return (i - self.r) % self.n

    def folds(self, i):
        k = self.k(i)
        return k >= self.head and k - self.head > self.c

    def slot(self, i):
        k = self.k(i)
        if k < self.head:
            return k
        j = k - self.head
        return self.head + (2 * j if j <= self.c else 2 * (self.m - 1 - j) + 1)

    def physical_order(self):
        slot = {self.slot(i): i for i in range(self.n)}
        return [slot[s] for s in range(self.n)]

    def place(self, i, rot, fold, slide, stretch):
        """Position (in pitches), scale and opacity of index i."""
        n = self.n
        d = (-self.r) % n                   # every index moves d steps forward
        if rot < 1.0:
            u = i + rot * d
            alpha = 1.0
            if u > n - 1:                   # wrapping: fade out past the end,
                over = u - (n - 1)          # fade in before the start
                if over < 0.5:
                    alpha = 1 - 2 * over
                else:
                    u -= n
                    alpha = min(1.0, 2 * (over - 0.5))
            return u, 1.0, max(0.0, alpha)
        k = self.k(i)
        if k < self.head:                 # the I/O row: stays on its edge
            return float(k), 1.0, 1.0
        j = k - self.head
        u, scale = float(j), 1.0
        if j > self.c:
            th = math.pi * fold
            u = self.c + (j - self.c) * math.cos(th)
            scale = max(0.04, abs(math.cos(th)))
        u = (u + 0.5 * (j > self.c) * slide) * (1 + stretch)
        return self.head + u, scale, 1.0


def build(nx, ny, buffer_cols, pitch, samples, frame=None):
    gx = nx + 2 if buffer_cols else nx
    gy = ny + 1
    buf_cols = {nx // 2, nx + 1} if buffer_cols else set()
    io = (0, ny)
    # Columns: the whole ring folds; with buffer columns the rotation starts at
    # x = NX + 1, so both buffer columns (half a ring apart) land on the edges.
    # Rows: the rotation brings the I/O row to the top, where it stays; the
    # compute rows fold onto each other exactly like the columns.
    rx = Ring(gx, nx + 1 if buffer_cols else 0)
    ry = Ring(gy, ny, head=1)

    edges = "I/O row and buffer columns" if buffer_cols else "I/O row"
    phases = [
        ("Logical grid", 1.5),
        (f"Rotate the rings: {edges} first", 2.0),
        ("Fold left/right", 2.5),
        ("Fold top/bottom", 2.5),
        ("Slide layers apart", 1.5),
        ("Stretch to the die", 2.0),
        ("Physical layout", 2.5),
    ]
    total = sum(d for _, d in phases)

    def progress(t):
        vals, s = [], t
        for _, dur in phases:
            vals.append(ease(s / dur))
            s -= dur
        _, rot, fx, fy, slide, stretch, _ = vals
        return rot, fx, fy, slide, stretch

    tile = pitch * 0.78
    margin = pitch * 0.8
    top = 56
    bottom = 46
    width = (gx - 1) * pitch + 2 * margin
    height = (gy - 1) * pitch + 2 * margin + top + bottom

    def px(u):
        return margin + u * pitch

    def py(u):
        return top + margin + u * pitch

    css = []
    static = {}
    for dim, ring, conv, name in (("x", rx, px, "ax"), ("y", ry, py, "ay")):
        for i in range(ring.n):
            def at(t):
                rot, fx, fy, slide, stretch = progress(t)
                u, sc, al = ring.place(i, rot, fx if dim == "x" else fy, slide, stretch)
                pos = conv(u)
                tf = (f"translate({pos:.1f}px,0) scale({sc:.3f},1)" if dim == "x"
                      else f"translate(0,{pos:.1f}px) scale(1,{sc:.3f})")
                return tf, al
            if frame is not None:
                static[(name, i)] = at(frame)
                continue
            frames = []
            for s in range(samples + 1):
                tf, al = at(total * s / samples)
                frames.append(f"{100 * s / samples:.2f}%{{transform:{tf};opacity:{al:.2f}}}")
            css.append(f"@keyframes {name}{i}{{{''.join(frames)}}}")
            css.append(f".{name}{i}{{animation:{name}{i} {total}s linear infinite alternate}}")

    # Stage labels: each fades in after the previous one has faded out.
    starts, s = [], 0.0
    for _, d in phases:
        starts.append(s)
        s += d
    fade = 1.0  # percent
    for k, (_, d) in enumerate(phases):
        a = 100 * starts[k] / total
        b = 100 * (starts[k] + d) / total
        first, last = k == 0, k == len(phases) - 1
        lo = 1 if first else 0
        hi = 1 if last else 0
        kf = [f"0%{{opacity:{lo}}}", f"{a:.2f}%{{opacity:{lo}}}",
              f"{min(100, a + fade):.2f}%{{opacity:1}}", f"{max(0, b - fade):.2f}%{{opacity:1}}",
              f"{b:.2f}%{{opacity:{hi}}}", f"100%{{opacity:{hi}}}"]
        if frame is None:
            css.append(f"@keyframes st{k}{{{''.join(kf)}}}")
            css.append(f".st{k}{{opacity:0;animation:st{k} {total}s linear infinite alternate}}")
    cur = 0
    if frame is not None:
        cur = max(k for k in range(len(phases)) if starts[k] <= frame or k == 0)
        for k in range(len(phases)):
            css.append(f".st{k}{{opacity:{1 if k == cur else 0}}}")

    def theme(p, dark):
        rules = [f".bg{{fill:{p['bg']}}}", f".title{{fill:{p['text']}}}",
                 f".stage,.order{{fill:{p['sub']}}}"]
        for cls, pair in CLASSES.items():
            fill, border = pair[1 if dark else 0]
            rules.append(f".{cls} rect{{fill:{fill};stroke:{border}}}")
            rules.append(f".{cls} text{{fill:{border}}}")
        return "".join(rules)

    css.append(theme(LIGHT, False))
    css.append("@media (prefers-color-scheme: dark){" + theme(DARK, True) + "}")
    css.append("@media (prefers-reduced-motion: reduce){*{animation-play-state:paused!important}}")
    css.append(".rt rect{stroke-dasharray:3 2}")
    if frame is not None:
        css = []  # static frames use plain attributes

    def tile_class(x, y):
        if (x, y) == io:
            return "io", "IO"
        if y == ny:
            return "rt", ""
        if x in buf_cols:
            return "buf", "B"
        return f"q{int(rx.folds(x)) + 2 * int(ry.folds(y))}", ""

    # Draw order: tiles that end up on top of the fold stack last
    order = sorted(((x, y) for y in range(gy) for x in range(gx)),
                   key=lambda p: rx.folds(p[0]) + ry.folds(p[1]))

    out = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height:.0f}" '
        f'width="{width:.0f}" height="{height:.0f}" role="img" '
        f'aria-label="A {nx} by {ny} torus with its I/O row folding once per dimension '
        f'and interleaving into its physical layout">',
        f"<style>{''.join(css)}</style>",
        f'<rect class="bg" width="{width:.0f}" height="{height:.0f}" rx="8"'
        f'{"" if frame is None else " fill=" + chr(34) + LIGHT["bg"] + chr(34)}/>',
        f'<text class="title" x="{margin - tile / 2:.0f}" y="26" font-family="system-ui,sans-serif" '
        f'{"" if frame is None else "fill=" + chr(34) + LIGHT["text"] + chr(34) + " "}'
        f'font-size="15" font-weight="600">Folded torus: {nx}x{ny} compute tiles + I/O row'
        f'{" + buffer columns" if buffer_cols else ""}</text>',
    ]
    for k, (label, _) in enumerate(phases):
        if frame is not None and k != cur:
            continue  # static frames carry only the current stage
        out.append(f'<text class="stage st{k}" x="{margin - tile / 2:.0f}" y="46" '
                   f'font-family="system-ui,sans-serif" font-size="13">{k + 1}. {label}</text>')
    h = tile / 2
    for x, y in order:
        cls, tag = tile_class(x, y)
        ax = f'class="ax{x}"' if frame is None else f'transform="{static[("ax", x)][0].replace("px", "")}" opacity="{static[("ax", x)][1]:.2f}"'
        ay = f'class="ay{y}"' if frame is None else f'transform="{static[("ay", y)][0].replace("px", "")}" opacity="{static[("ay", y)][1]:.2f}"'
        if frame is None:
            ax += f' transform="translate({px(x):.1f},0)"'
            ay += f' transform="translate(0,{py(y):.1f})"'
        fill, border = CLASSES[cls][0]
        paint = f' fill="{fill}" stroke="{border}"' if frame is not None else ""
        txt = f' fill="{border}"' if frame is not None else ""
        dash = ' stroke-dasharray="3 2"' if cls == "rt" else ""
        out.append(f'<g {ax}><g {ay}><g class="{cls}">'
                   f'<rect x="{-h:.1f}" y="{-h:.1f}" width="{tile:.1f}" height="{tile:.1f}" rx="4" '
                   f'fill-opacity="0.85" stroke-width="1"{paint}{dash}/>')
        if tag:
            out.append(f'<text x="0" y="-3" text-anchor="middle" font-family="system-ui,sans-serif" '
                       f'font-size="10" font-weight="600"{txt}>{tag}</text>'
                       f'<text x="0" y="10" text-anchor="middle" font-family="ui-monospace,monospace" '
                       f'font-size="10"{txt}>{x},{y}</text>')
        else:
            out.append(f'<text x="0" y="4" text-anchor="middle" font-family="ui-monospace,monospace" '
                       f'font-size="10"{txt}>{x},{y}</text>')
        out.append("</g></g></g>")
    fx = " ".join(map(str, rx.physical_order()))
    fy = " ".join(map(str, ry.physical_order()))
    out.append(f'<text class="order" x="{margin - tile / 2:.0f}" y="{height - 26:.0f}" '
               f'font-family="ui-monospace,monospace" font-size="11">physical x order: {fx}</text>')
    out.append(f'<text class="order" x="{margin - tile / 2:.0f}" y="{height - 10:.0f}" '
               f'font-family="ui-monospace,monospace" font-size="11">physical y order: {fy}</text>')
    out.append("</svg>")
    return "\n".join(out), rx, ry


def check_links(ring):
    """Every ring link, wraparound included, spans at most two physical slots."""
    slot = {i: s for s, i in enumerate(ring.physical_order())}
    worst = max(abs(slot[i] - slot[(i + 1) % ring.n]) for i in range(ring.n))
    assert worst <= 2, f"ring of {ring.n}: a link spans {worst} slots"
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nx", type=int, default=8, help="compute columns (even)")
    ap.add_argument("--ny", type=int, default=8, help="compute rows")
    ap.add_argument("--buffer-cols", action="store_true",
                    help="add buffer columns at x = NX/2 and x = NX + 1")
    ap.add_argument("--pitch", type=float, default=52.0)
    ap.add_argument("--samples", type=int, default=140,
                    help="keyframes per pass (higher = smoother flips, bigger file)")
    ap.add_argument("--frame", type=float, default=None,
                    help="write a static frame at this time (seconds) instead of the animation")
    ap.add_argument("-o", "--out", default="docs/img/torus_fold.svg")
    a = ap.parse_args()
    if a.buffer_cols and a.nx % 2:
        ap.error("buffer columns need an even NX (they sit half a ring apart)")
    svg, rx, ry = build(a.nx, a.ny, a.buffer_cols, a.pitch, a.samples, a.frame)
    check_links(rx)
    check_links(ry)
    with open(a.out, "w") as f:
        f.write(svg)
    print(f"wrote {a.out} ({len(svg) / 1024:.0f} KB); physical x order {rx.physical_order()}, "
          f"y order {ry.physical_order()}")


if __name__ == "__main__":
    main()
