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
import re
import sys

import matplotlib

matplotlib.use("Agg")
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--def", dest="def_file", required=True)
    ap.add_argument("--lef", nargs="+", required=True)
    ap.add_argument("--style", choices=sorted(STYLES), required=True)
    ap.add_argument("--title", default="")
    ap.add_argument("--note", default="", help="text box (area, timing)")
    ap.add_argument("--out", required=True)
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
    fig, ax = plt.subplots(figsize=(fig_w + 4, fig_w * aspect + 1))
    handles = []
    for i, lab in enumerate(labels):
        color = "#bbbbbb" if lab == OTHER else cmap(i % 20)
        ax.add_collection(PatchCollection(groups[lab], facecolor=color, edgecolor="none", alpha=0.9))
        n = len(groups[lab])
        handles.append(Patch(color=color, label=f"{lab} ({n})"))
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
