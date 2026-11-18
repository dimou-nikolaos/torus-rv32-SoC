#!/usr/bin/env python3
"""Draws a placed (or routed) design as a PNG, every instance colored by group.

Reads the DEF of a PnR run (die area, component placements) and the LEF files
for the cell sizes (macros and standard cells). Groups are regular expressions
on instance names, first match wins; a group may use a capture to split into
one color per value (one per tile, one per bank).

  scripts/plot_layout.py --def build/pnr/results/sky130hd/tile/base/6_final.def \\
      --lef <lefs...> --style tile --title "tile, 20 ns" --out build/pnr/tile_base.png

Styles: tile (macros by bank, standard cells by unit), grid (one color per tile,
I/O row and top-level cells apart).
"""
import argparse
import json
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.lines  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.collections import PatchCollection  # noqa: E402
from matplotlib.patches import Patch, Rectangle  # noqa: E402

# (label, regex on the instance name) per style. Names use / or . between levels.
STYLES = {
    "tile": [
        ("SRAM bank {0}", r"g_bank\[(\d)\].*u_ram"),
        ("core", r"^u_core"),
        ("NIU", r"^u_niu"),
        ("router NoC{0}", r"g_router\[(\d)\]|g_plane\[(\d)\]|u_router(\d)"),
        ("scratchpad logic", r"^u_spm"),
        ("counter bank", r"^u_perf"),
        ("tile control, console", r"^(u_ctrl|u_console|u_ibus|u_dbus)"),
    ],
    "grid": [
        ("tile ({1},{0})", r"g_y\[(\d+)\][./]g_x\[(\d+)\][./]g_tile"),
        ("I/O tile", r"g_io_tile"),
        ("I/O row stops", r"g_io"),
        ("link slices", r"g_link"),
    ],
}
OTHER = "other (buffers, clock tree, ties)"
SKIP_MASTERS = re.compile(r"(tapvpwrvgnd|fill|decap|PHY_|TAP_)", re.I)


# Valid bit of each output link of a tile (the MSB of a 207-bit link_t; plane 0
# is the upper half of the two-plane port): one line per link in the overlay
LINK_PINS = {"x_out_o[413]": ("NoC0 east", "#1f77b4"), "y_out_o[413]": ("NoC0 south", "#2ca02c"),
             "x_out_o[206]": ("NoC1 west", "#d62728"), "y_out_o[206]": ("NoC1 north", "#9467bd")}


def lef_pins(paths, macro):
    """Pin centers of one macro, in its own (R0) coordinates."""
    pins = {}
    for p in paths:
        text = open(p, errors="replace").read()
        m = re.search(rf"MACRO {re.escape(macro)}\b(.*?)END {re.escape(macro)}\b", text, re.S)
        if not m:
            continue
        for pm in re.finditer(r"PIN (\S+)\n(.*?)END \1", m.group(1), re.S):
            r = re.search(r"RECT\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)\s+([-\d.]+)", pm.group(2))
            if r:
                x0, y0, x1, y1 = (float(v) for v in r.groups())
                pins[pm.group(1)] = ((x0 + x1) / 2, (y0 + y1) / 2)
    return pins


def place_point(px, py, w, h, x, y, orient):
    """A point (px, py) of a w x h macro, placed at (x, y) with a DEF orientation."""
    o = {"N": "R0", "S": "R180", "FN": "MY", "FS": "MX"}.get(orient, orient)
    if o in ("MY", "R180"):
        px = w - px
    if o in ("MX", "R180"):
        py = h - py
    return x + px, y + py


def link_lines(def_text, comps, sizes, pins, macro):
    """(x0, y0, x1, y1, label, color) segments of each tile output link, from
    its pin through buffers and repeaters (followed A -> X) to the sinks."""
    where = {name: (master, x, y, o) for name, master, x, y, o in comps}
    sec = re.search(r"^NETS \d+ ;(.*?)^END NETS", def_text, re.S | re.M)
    nets, net_of = [], {}
    for net in (sec.group(1) if sec else "").split(";"):
        conns = [(i.replace("\\", ""), pn.replace("\\", ""))
                 for i, pn in re.findall(r"\(\s*(\S+)\s+(\S+)\s*\)", net)]
        if not conns:
            continue
        for c in conns:
            net_of[c] = len(nets)
        nets.append(conns)
    w, h = sizes[macro]

    def point(inst, pin):
        m, x, y, o = where[inst]
        if m == macro and pin in pins:
            return place_point(*pins[pin], w, h, x, y, o)
        cw, ch = sizes.get(m, (0, 0))
        return x + cw / 2, y + ch / 2

    out = []
    for (inst, pin), n in net_of.items():
        if pin not in LINK_PINS or inst not in where or where[inst][0] != macro:
            continue
        label, color = LINK_PINS[pin]
        chain = f"{inst}/{pin}"
        todo, seen = [(n, point(inst, pin), 0.0)], set()
        while todo:
            n, p0, acc = todo.pop()
            if n in seen:
                continue
            seen.add(n)
            for i, pn in nets[n]:
                if (i, pn) == (inst, pin) or i == "PIN" or i not in where or pn in ("X", "Y") and n == net_of.get((i, pn)):
                    continue
                p1 = point(i, pn)
                a1 = acc + abs(p1[0] - p0[0]) + abs(p1[1] - p0[1])
                out.append((*p0, *p1, label, color, chain, a1))
                master = where[i][0]
                if re.search(r"__(buf|clkbuf|dlygate|dlymetal)", master) and (i, "X") in net_of:
                    todo.append((net_of[(i, "X")], p1, a1))
                ft = re.match(r"([xy]ft)_i(\[\d+\])$", pn)
                if master == macro and ft and (i, f"{ft.group(1)}_o{ft.group(2)}") in net_of:
                    # through the tile's feedthrough: continue from the other edge
                    q = (i, f"{ft.group(1)}_o{ft.group(2)}")
                    p2 = point(*q)
                    a2 = a1 + abs(p2[0] - p1[0]) + abs(p2[1] - p1[1])
                    out.append((*p1, *p2, label, color, chain, a2))
                    todo.append((net_of[q], p2, a2))
    return out


def lef_sizes(paths):
    sizes = {}
    for p in paths:
        cur = None
        for line in open(p, errors="replace"):
            t = line.split()
            if not t:
                continue
            if t[0] == "MACRO":
                cur = t[1]
            elif t[0] == "SIZE" and cur:
                sizes[cur] = (float(t[1]), float(t[3]))
            elif t[0] == "END" and len(t) > 1 and t[1] == cur:
                cur = None
    return sizes


def read_def(path):
    dbu, die, comps = 1000.0, None, []
    text = open(path, errors="replace").read()
    read_def.text = text
    m = re.search(r"UNITS DISTANCE MICRONS (\d+)", text)
    if m:
        dbu = float(m.group(1))
    m = re.search(r"DIEAREA\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)", text)
    if m:
        die = [float(v) / dbu for v in m.groups()]
    sec = re.search(r"^COMPONENTS \d+ ;(.*?)^END COMPONENTS", text, re.S | re.M)
    for c in (sec.group(1) if sec else "").split(";"):
        m = re.search(r"-\s+(\S+)\s+(\S+).*?\+\s+(?:PLACED|FIXED|COVER)\s+\(\s*(-?\d+)\s+(-?\d+)\s*\)\s+(\S+)", c, re.S)
        if m:
            name, master, x, y, orient = m.groups()
            comps.append((name.replace("\\", ""), master, float(x) / dbu, float(y) / dbu, orient))
    return die, comps


def classify(name, master, rules):
    for label, rx in rules:
        if label.startswith("SRAM") and not master.startswith("sky130_sram"):
            continue
        m = re.search(rx, name)
        if m:
            vals = [g for g in m.groups() if g is not None] if m.groups() else []
            try:
                return label.format(*vals)
            except IndexError:
                return label.split(" {")[0]
    return OTHER


def densest(rects, die, n=24):
    x0, y0, x1, y1 = die if die else (0, 0, 1, 1)
    bw, bh = (x1 - x0) / n, (y1 - y0) / n
    count = {}
    for r in rects:
        k = (int((r.get_x() - x0) / bw), int((r.get_y() - y0) / bh))
        count[k] = count.get(k, 0) + 1
    (i, j), _ = max(count.items(), key=lambda kv: kv[1])
    return x0 + (i + 0.5) * bw, y0 + (j + 0.5) * bh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--def", dest="def_file", required=True)
    ap.add_argument("--lef", nargs="+", required=True)
    ap.add_argument("--style", choices=sorted(STYLES), required=True)
    ap.add_argument("--title", default="")
    ap.add_argument("--note", default="", help="text box (area, timing)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-labels", action="store_true", help="legend only, no labels on the layout")
    ap.add_argument("--links", action="store_true", help="grid: draw each tile output link (its valid bit)")
    ap.add_argument("--links-json", help="with --links: write link lengths (Manhattan, pin to farthest sink through repeaters and feedthroughs)")
    args = ap.parse_args()

    sizes = lef_sizes(args.lef)
    die, comps = read_def(args.def_file)
    rules = STYLES[args.style]
    groups = {}
    missing = set()
    for name, master, x, y, orient in comps:
        if SKIP_MASTERS.search(master):
            continue
        if master not in sizes:
            missing.add(master)
            continue
        w, h = sizes[master]
        if orient in ("R90", "R270", "MXR90", "MYR90", "W", "E", "FW", "FE"):
            w, h = h, w
        groups.setdefault(classify(name, master, rules), []).append(Rectangle((x, y), w, h))
    if missing:
        print(f"no LEF size for {len(missing)} masters, e.g. {sorted(missing)[:3]}", file=sys.stderr)

    labels = sorted(groups, key=lambda g: (g == OTHER, g))
    cmap = plt.get_cmap("tab20")
    fig_w = 10
    aspect = (die[3] - die[1]) / (die[2] - die[0]) if die else 1
    fig, ax = plt.subplots(figsize=(fig_w + 6, max(fig_w * aspect + 1, 8)))
    handles = []
    for i, lab in enumerate(labels):
        color = "#bbbbbb" if lab == OTHER else cmap(i % 20)
        ax.add_collection(PatchCollection(groups[lab], facecolor=color, edgecolor="none", alpha=0.9))
        n = len(groups[lab])
        handles.append(Patch(color=color, label=f"{lab} ({n})"))
    if args.links:
        lines = link_lines(read_def.text, comps, sizes, lef_pins(args.lef, "tile_top"), "tile_top")
        seen = set()
        lengths = {}
        for x0, y0, x1, y1, label, color, chain, acc in lines:
            # a link's length: its longest path from the pin to a sink
            lengths[(label, chain)] = max(lengths.get((label, chain), 0.0), acc)
            ax.plot([x0, x1], [y0, y1], color=color, lw=0.9, alpha=0.85)
            if label not in seen:
                seen.add(label)
                handles.append(matplotlib.lines.Line2D([], [], color=color, label=f"link {label}"))
        print(f"{len(lines)} link lines")
        summary = {}
        for (label, chain), length in lengths.items():
            summary.setdefault(label, []).append((length, chain))
        for lab in sorted(summary):
            v = summary[lab]
            args.note += (f"\nlink {lab:13s}longest {max(v)[0] / 1e3:.1f} mm, "
                          f"mean {sum(l for l, _ in v) / len(v) / 1e3:.1f} mm")
        if args.links_json:
            json.dump({lab: {"links": len(v), "max_um": max(v)[0], "mean_um": sum(l for l, _ in v) / len(v),
                             "longest": max(v)[1]} for lab, v in summary.items()},
                      open(args.links_json, "w"), indent=1)
    # Labels on top: each group's name at the median of its instances (macros:
    # the middle of the group's bounding box), skipped for scattered leftovers.
    if not args.no_labels:
        for lab in labels:
            if lab == OTHER:
                continue
            rects = groups[lab]
            if lab.startswith("SRAM") or args.style == "grid" and lab.startswith("tile"):
                cx = (min(r.get_x() for r in rects) + max(r.get_x() + r.get_width() for r in rects)) / 2
                cy = (min(r.get_y() for r in rects) + max(r.get_y() + r.get_height() for r in rects)) / 2
            else:
                # densest cell of a 24 x 24 histogram: the main cluster, not the median
                cx, cy = densest(rects, die)
            ax.text(cx, cy, lab, ha="center", va="center", fontsize=9, weight="bold",
                    bbox=dict(boxstyle="round,pad=0.2", fc="white", ec="none", alpha=0.75))
    if die:
        ax.add_patch(Rectangle((die[0], die[1]), die[2] - die[0], die[3] - die[1], fill=False, lw=1.2))
        ax.set_xlim(die[0], die[2])
        ax.set_ylim(die[1], die[3])
    ax.set_aspect("equal")
    ax.set_xlabel("µm")
    ax.set_ylabel("µm")
    ax.set_title(args.title)
    ax.legend(handles=handles, loc="upper left", bbox_to_anchor=(1.01, 1), fontsize=8, frameon=False)
    if args.note:
        ax.text(1.02, 0.0, args.note, transform=ax.transAxes, fontsize=8, family="monospace", va="bottom")
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"wrote {args.out}: {sum(len(v) for v in groups.values())} instances in {len(groups)} groups")
    return 0


if __name__ == "__main__":
    sys.exit(main())
