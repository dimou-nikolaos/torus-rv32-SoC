#!/usr/bin/env python3
"""Generic Yosys synthesis with a per-unit area report.

SRAM banks (spm_bank) are blackboxed as macros and reported in bits; all other
logic is mapped to Yosys generic cells and reported as cell counts and the CMOS
transistor estimate of `stat -tech cmos`.

Two views:
  - the top, synthesized flat: the area in context, where tied-off inputs and
    unused outputs are optimized away;
  - each unit (a submodule named on the command line) synthesized on its own,
    with its default parameters and all ports live: stable per-unit numbers for
    tracking trends. In-context area approaches these as tied-off ports (NIU,
    engine) get connected in later milestones.
Instance counts come from an elaboration of the top with the units blackboxed.
Units run in parallel.

With --liberty, every run is also mapped to that standard-cell library
(dfflibmap, abc) and its cell area reported in um^2 (sky130_fd_sc_hd from the
image's PDK by default via $PDK_ROOT); SRAM stays a macro, reported in bits.

  scripts/synth_area.py --top tile_top --flist rtl/tile/tile_syn.f \\
      --units core_wrap spm_banks tile_ctrl --out build/syn/tile
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import sys

SRAM_MACROS = {"spm_bank": 1024 * 128}  # bits per instance


def yosys(script, log):
    res = subprocess.run(["yosys", "-m", "slang", "-q", "-l", log, "-p", script],
                         capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"yosys failed, see {log}\n{res.stderr}")


def bb_opts(mods):
    return " ".join(f"--blackboxed-module {m}" for m in mods)


def synth(flist, top, out, liberty=None):
    """Flat synthesis of `top`; returns (cells, transistors, sram_bits, um2 or None)."""
    stat = f"{out}_{top}_stat.json"
    lstat = f"{out}_{top}_lib_stat.json"
    mapping = (f"dfflibmap -liberty {liberty}\nabc -liberty {liberty}\nopt_clean\n"
               f"tee -q -o {lstat} stat -json -liberty {liberty}\n") if liberty else ""
    yosys(f"read_slang -D SYNTHESIS {bb_opts(SRAM_MACROS)} -f {flist} --top {top}\n"
          f"hierarchy -top {top}\nsynth -top {top} -flatten\n"
          f"tee -q -o {stat} stat -json -tech cmos\n{mapping}", f"{out}_{top}_yosys.log")
    um2 = None
    if liberty:
        lmod = next(v for k, v in json.load(open(lstat))["modules"].items() if k.lstrip("\\") == top)
        um2 = float(lmod.get("area", 0))
    mod = next(v for k, v in json.load(open(stat))["modules"].items() if k.lstrip("\\") == top)
    cells, sram = 0, 0
    for ctype, n in mod.get("num_cells_by_type", {}).items():
        name = ctype.lstrip("\\")
        if name in SRAM_MACROS:
            sram += n * SRAM_MACROS[name]
        else:
            cells += n
    tr = int(str(mod.get("estimated_num_transistors", 0)).rstrip("+"))
    return cells, tr, sram, um2


def instance_counts(flist, top, units, out):
    stat = f"{out}_instances.json"
    yosys(f"read_slang -D SYNTHESIS {bb_opts(list(SRAM_MACROS) + units)} -f {flist} --top {top}\n"
          f"tee -q -o {stat} stat -json\n", f"{out}_instances_yosys.log")
    mods = {k.lstrip("\\"): v for k, v in json.load(open(stat))["modules"].items()}
    by_type = {k.lstrip("\\"): n for k, n in mods[top].get("num_cells_by_type", {}).items()}
    return {u: by_type.get(u, 0) for u in units}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", required=True)
    ap.add_argument("--flist", required=True)
    ap.add_argument("--units", nargs="*", default=[], help="direct submodules to report")
    ap.add_argument("--out", required=True, help="output prefix")
    ap.add_argument("--liberty", nargs="?", const="sky130",
                    help="also map to a liberty file (no value: sky130_fd_sc_hd tt_025C_1v80 under $PDK_ROOT)")
    args = ap.parse_args()
    if args.liberty == "sky130":
        args.liberty = os.path.join(os.environ.get("PDK_ROOT", "/opt/torus/pdk"), "sky130A", "libs.ref",
                                    "sky130_fd_sc_hd", "lib", "sky130_fd_sc_hd__tt_025C_1v80.lib")
    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)

    try:
        counts = instance_counts(args.flist, args.top, args.units, args.out)
        with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
            jobs = {m: ex.submit(synth, args.flist, m, args.out, args.liberty) for m in [args.top] + args.units}
            res = {m: j.result() for m, j in jobs.items()}
    except RuntimeError as e:
        sys.stderr.write(str(e) + "\n")
        return 1

    total = res[args.top]
    rows = []
    for u in args.units:
        n = counts[u]
        c, t, sb, um2 = res[u]
        rows.append({"module": u, "instances": n, "cells": n * c, "transistors": n * t,
                     "sram_bits": n * sb, "um2": n * um2 if um2 is not None else None})

    lib = f", {os.path.basename(args.liberty)}" if args.liberty else ""
    lib_ctx = f", {total[3]:.0f} um2 of standard cells" if args.liberty else ""
    lines = [f"Area report: {args.top} (Yosys generic cells{lib}, SRAM banks as macros)", "",
             f"In context (flat {args.top}): {total[0]} cells, ~{total[1]} transistors of logic{lib_ctx}, "
             f"{total[2]} SRAM bits ({total[2] // 8192} KB)", "",
             "Units standalone, all ports live (x instances):",
             f"  {'unit':26s} {'inst':>5s} {'cells':>9s} {'transistors':>12s} {'SRAM bits':>10s}"
             + (f" {'um2':>10s}" if args.liberty else "")]
    for r in sorted(rows, key=lambda r: -r["transistors"]):
        lines.append(f"  {r['module']:26s} {r['instances']:5d} {r['cells']:9d} {r['transistors']:12d} "
                     f"{r['sram_bits']:10d}" + (f" {r['um2']:10.0f}" if args.liberty else ""))
    txt = "\n".join(lines) + "\n"
    open(args.out + "_area.txt", "w").write(txt)
    json.dump({"top": args.top, "liberty": args.liberty,
               "in_context": {"cells": total[0], "transistors": total[1], "sram_bits": total[2], "um2": total[3]},
               "units_standalone": rows},
              open(args.out + "_area.json", "w"), indent=1)
    print(txt)
    return 0


if __name__ == "__main__":
    sys.exit(main())
