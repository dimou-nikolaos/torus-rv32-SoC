#!/usr/bin/env python3
"""Place and route in sky130hd or IHP SG13G2 with OpenROAD-flow-scripts (M5).

Runs the ORFS flow of the image ($ORFS_FLOW) on a design under pnr/<design>/
(config.mk, sdc.tcl, io.tcl). The RTL file list is expanded here (ORFS takes
plain file lists), the SDC is generated with the clock period of the run, and
the reports of the last stage reached are collected into
build/pnr/<design>_<variant>_report.{txt,json}: area, utilization, timing
(worst slack, achievable period), wirelength, power, runtime.

  scripts/pnr.py tile --period 20                 # synth .. finish (no GDS)
  scripts/pnr.py tile --period 20 --to floorplan  # stop after a stage
  scripts/pnr.py tile --period 15 --variant p15   # a second run next to the first
  scripts/pnr.py tile --report-only               # collect reports again
  scripts/pnr.py tile --abstract --variant cross  # LEF and timing model for the grid
  scripts/pnr.py grid --order folded --tile-variant cross --period 20
  scripts/pnr.py tile_ihp --period 20             # the tile in IHP SG13G2 (cross floorplan)
  scripts/pnr.py grid_ihp --order folded --tile-variant wide   # the grid of IHP tiles

The grid (soc_top, 4x4 plus the I/O row) uses a tile run's abstract as a hard
macro; this script places the tiles in logical or folded order (the fold of
scripts/gen_fold_svg.py) with routing channels between them and stops after
global routing: timing comes from global-route parasitics.

Each variant runs in build/pnr/{logs,objects,reports,results}/<platform>/<design>/<variant>.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, "build", "pnr")
STAGES = ["synth", "floorplan", "place", "cts", "grt", "route", "finish"]

FLOW = os.environ.get("ORFS_FLOW", "/opt/torus/orfs/flow")
PDK = os.environ.get("PDK_ROOT", "/opt/torus/pdk")
SKY130_SRAM = "sky130A/libs.ref/sky130_sram_macros/{}/sky130_sram_2kbyte_1rw1r_32x512_8{}"
IHP_SRAM = "platforms/ihp-sg13g2/{}/RM_IHPSG13_1P_512x64_c2_bm_bist{}"
# Per platform: the standard-cell liberty for synthesis, and the LEFs the
# layout images need (cells and SRAM macros)
PLATFORMS = {
    "sky130hd": {"lib": os.path.join(FLOW, "platforms/sky130hd/lib/sky130_fd_sc_hd__tt_025C_1v80.lib"),
                 "setrc": os.path.join(FLOW, "platforms/sky130hd/setRC.tcl"),
                 "lefs": [os.path.join(FLOW, "platforms/sky130hd/lef/sky130_fd_sc_hd_merged.lef"),
                          os.path.join(PDK, SKY130_SRAM.format("lef", ".lef"))]},
    "ihp-sg13g2": {"lib": os.path.join(FLOW, "platforms/ihp-sg13g2/lib/sg13g2_stdcell_typ_1p20V_25C.lib"),
                   "setrc": os.path.join(FLOW, "platforms/ihp-sg13g2/setRC.tcl"),
                   # Grid: the core origin and every macro and fence on the
                   # site grid (legalization rejects cells off it)
                   "site": (0.48, 3.78), "core_margin": 30.24,
                   # No I/O row fences: the repeaters placement adds on the long
                   # links join their driver's fence (exclusive or suggested),
                   # pile up on its edge and legalization fails
                   "fence": None,
                   "lefs": [os.path.join(FLOW, "platforms/ihp-sg13g2/lef/sg13g2_stdcell.lef"),
                            os.path.join(FLOW, IHP_SRAM.format("lef", ".lef"))]},
}

DESIGNS = {
    "tile": {"platform": "sky130hd", "flist": "rtl/tile/tile_pnr.f", "top": "tile_top", "defines": ["SPM_SKY130"],
             "params": {"NX": 4, "NY": 4},
             "macro_libs": [os.path.join(PDK, SKY130_SRAM.format("lib", "_TT_1p8V_25C.lib"))],
             "flat": {"core_wrap": ["BOOT_ADDR", "MTVEC_ADDR", "SPM_BYTES"]}},
    "tile_ihp": {"platform": "ihp-sg13g2", "flist": "rtl/tile/tile_pnr_ihp.f", "top": "tile_top",
                 "defines": ["SPM_IHP"], "params": {"NX": 4, "NY": 4},
                 "macro_libs": [os.path.join(FLOW, IHP_SRAM.format("lib", "_typ_1p20V_25C.lib"))],
                 "flat": {"core_wrap": ["BOOT_ADDR", "MTVEC_ADDR", "SPM_BYTES"]}},
    # Grids of a tile design's abstract (scripts/pnr.py <tile> --abstract)
    "grid": {"platform": "sky130hd", "tile": "tile", "flist": "rtl/soc/soc_pnr.f", "top": "soc_top",
             "defines": [], "params": {"NX": 4, "NY": 4}, "macro_libs": [],
             "blackbox": {"tile_top": ["STARVE_LIMIT", "NX", "NY", "LINK_SLICE", "IO_RD_OUTSTANDING"]}},
    "grid_ihp": {"platform": "ihp-sg13g2", "tile": "tile_ihp", "flist": "rtl/soc/soc_pnr.f", "top": "soc_top",
                 "defines": [], "params": {"NX": 4, "NY": 4}, "macro_libs": [],
                 "blackbox": {"tile_top": ["STARVE_LIMIT", "NX", "NY", "LINK_SLICE", "IO_RD_OUTSTANDING"]}},
}
NX, NY = 4, 4


def is_grid(design):
    return "tile" in DESIGNS[design]


def tile_abstract(grid, variant):
    """LEF and liberty of the grid's tile run (scripts/pnr.py <tile> --abstract)."""
    tile = DESIGNS[grid]["tile"]
    res = stage_dirs(tile, variant)["results"]
    lef, lib = os.path.join(res, "tile_top.lef"), os.path.join(res, "tile_top_typ.lib")
    for f in (lef, lib):
        if not os.path.exists(f):
            raise SystemExit(f"{f} missing: run scripts/pnr.py {tile} --variant {variant} --abstract first")
    return lef, lib


def lef_size(lef, macro):
    cur = None
    for line in open(lef):
        t = line.split()
        if t[:1] == ["MACRO"]:
            cur = t[1]
        elif t[:1] == ["SIZE"] and cur == macro:
            return float(t[1]), float(t[3])
    raise SystemExit(f"{lef}: no SIZE for {macro}")


def grid_floorplan(order, channel, io_band, lef, out, site=None, margin=10.0, fence="EXCLUSIVE"):
    """Writes the tile placement for the grid; returns the die (w, h).

    Physical columns run left to right, rows top to bottom (NoC0 runs east and
    south, to x+1 and y+1). Logical order: x and y as they are, the I/O row at
    the bottom. Folded: the rings of scripts/gen_fold_svg.py (x from rotation
    0, y with the I/O row first, on the top edge); a tile whose ring successor
    lies physically west or above is mirrored (MY, MX) so its east and south
    pins face it.
    """
    sys.path.insert(0, os.path.join(ROOT, "scripts"))
    from gen_fold_svg import Ring
    tw, th = lef_size(lef, "tile_top")

    def snap(v, axis):   # onto the site grid from the core origin
        if site is None:
            return v
        return margin + round((v - margin) / site[axis]) * site[axis]

    gy = NY + 1
    if order == "logical":
        col = {x: x for x in range(NX)}
        row = {y: y for y in range(gy)}
    else:
        rx, ry = Ring(NX, 0), Ring(gy, NY, head=1)
        col = {x: rx.slot(x) for x in range(NX)}
        row = {y: ry.slot(y) for y in range(gy)}
    heights = {row[y]: (io_band if y == NY else th) for y in range(gy)}
    die_w = channel + NX * (tw + channel)
    die_h = channel + sum(h + channel for h in heights.values())
    top_of = {}
    t = die_h - channel
    for r in range(gy):
        top_of[r] = t
        t -= heights[r] + channel
    lines = [f"# Generated by scripts/pnr.py: {order} order, {channel} um channels",
             "array set tile_pos {"]
    for y in range(NY):
        for x in range(NX):
            px = snap(channel + col[x] * (tw + channel), 0)
            py = snap(top_of[row[y]] - th, 1)
            # Mirrored only in folded order (as soc_top FT_ORDER 2 assumes)
            my = order == "folded" and col[(x + 1) % NX] < col[x]
            mx = order == "folded" and row[(y + 1) % gy] < row[y]
            orient = {(False, False): "R0", (True, False): "MY", (False, True): "MX", (True, True): "R180"}[(my, mx)]
            lines.append(f"  {x},{y} {{{px:.2f} {py:.2f} {orient}}}")
    lines += ["}", "set placed 0",
              "foreach inst [[ord::get_db_block] getInsts] {",
              "  if {[[$inst getMaster] getName] ne \"tile_top\"} continue",
              "  set name [$inst getName]",
              "  if {![regexp {g_y\\\\?\\[(\\d+)\\\\?\\].*g_x\\\\?\\[(\\d+)\\\\?\\]} $name -> y x]} { error \"cannot parse $name\" }",
              "  lassign $tile_pos($x,$y) px py orient",
              "  place_macro -macro_name $name -location [list $px $py] -orientation $orient",
              "  incr placed",
              "}",
              f"if {{$placed != {NX * NY}}} {{ error \"placed $placed tiles, expected {NX * NY}\" }}"]
    # The I/O row is standard cells at this level: fence each position (I/O tile
    # or router-only stop with its routers) into its column's span of the band,
    # where the floorplan puts that row, so its wires are measured in place.
    band_top = top_of[row[NY]]
    lines += ["set block [ord::get_db_block]", "proc um {v} { return [ord::microns_to_dbu $v] }"]
    for x in range(NX if fence else 0):
        x0 = snap(col[x] * (tw + channel) + channel / 2, 0)
        x1, y0, y1 = snap(x0 + tw + channel, 0), snap(band_top - io_band, 1), snap(band_top, 1)
        lines += [f"set r [odb::dbRegion_create $block io_row_{x}]",
                  f"odb::dbBox_create $r [um {x0:.2f}] [um {y0:.2f}] [um {x1:.2f}] [um {y1:.2f}]",
                  f"$r setRegionType {fence}",
                  f"set g [odb::dbGroup_create $r io_row_{x}]",
                  "set n 0",
                  "foreach inst [$block getInsts] {",
                  f"  if {{[regexp {{^g_y\\\\?\\[{NY}\\\\?\\]\\.g_x\\\\?\\[{x}\\\\?\\]}} [$inst getName]]}} {{ $g addInst $inst; incr n }}",
                  "}",
                  f"puts \"io_row_{x}: $n instances fenced\"",
                  f"if {{$n == 0}} {{ error \"no instances for I/O row position {x}\" }}"]
    os.makedirs(os.path.dirname(out), exist_ok=True)
    text = "\n".join(lines) + "\n"
    if not os.path.exists(out) or open(out).read() != text:   # a newer file reruns ORFS from here
        open(out, "w").write(text)
    return die_w, die_h


def expand_flist(path, files, incdirs):
    """Expands a .f file list (paths relative to the repo root) to absolute paths."""
    for line in open(os.path.join(ROOT, path)):
        line = line.split("//")[0].strip()
        if not line:
            continue
        if line.startswith("-f "):
            expand_flist(line[3:].strip(), files, incdirs)
        elif line.startswith("+incdir+"):
            incdirs.append(os.path.join(ROOT, line[len("+incdir+"):]))
        elif line.startswith(("-", "+")):
            raise SystemExit(f"{path}: unsupported option {line}")
        else:
            files.append(os.path.join(ROOT, line))
    return files, incdirs


def tcl_geometry(path):
    """Values of a Tcl file of `set name value` and `set name [expr {...}]` lines."""
    v = {}
    for line in open(path):
        line = line.split(";#")[0].strip()
        m = re.match(r"set (\w+)\s+(.+)$", line)
        if not m:
            continue
        expr = m.group(2)
        e = re.match(r"\[expr \{(.+)\}\]$", expr)
        expr = e.group(1) if e else expr
        v[m.group(1)] = eval(re.sub(r"\$(\w+)", lambda x: repr(v[x.group(1)]), expr), {})
    return v


def floorplan_vars(design, floorplan, variant, geom):
    """ORFS variables of a hand floorplan; geom (name=value) overrides its
    `set` values in a copy under build/pnr/floorplan, which the floorplan's Tcl
    sources instead (TILE_FLOORPLAN_TCL)."""
    if floorplan == "auto":
        if geom:
            raise SystemExit("--geom needs a hand floorplan")
        return []
    path = os.path.join(ROOT, "pnr", design, f"{floorplan}.tcl")
    extra = []
    if geom:
        text = open(path).read()
        for kv in geom:
            k, v = kv.split("=", 1)
            text, n = re.subn(rf"^set {k}(\s+)\S+", lambda m: f"set {k}{m.group(1)}{v}", text, flags=re.M)
            if n != 1:
                raise SystemExit(f"--geom {k}: no `set {k}` in {path}")
        path = os.path.join(WORK, "floorplan", f"{design}_{floorplan}_{variant}.tcl")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.exists(path) or open(path).read() != text:   # a newer file reruns ORFS from here
            open(path, "w").write(text)
        extra.append(f"TILE_FLOORPLAN_TCL={path}")
    g = tcl_geometry(path)
    m, w, h = g["margin"], g["die_w"], g["die_h"]
    return extra + [f"TILE_FLOORPLAN={floorplan}", f"DIE_AREA=0 0 {w:.2f} {h:.2f}",
                    f"CORE_AREA={m:.2f} {m:.2f} {w - m:.2f} {h - m:.2f}"]


def write_sdc(design, period, out):
    """The design's sdc.tcl, with the clock period set ahead of it."""
    os.makedirs(os.path.dirname(out), exist_ok=True)
    body = open(os.path.join(ROOT, "pnr", design, "sdc.tcl")).read()
    text = f"# Generated by scripts/pnr.py from pnr/{design}/sdc.tcl\nset clk_period {period}\n{body}"
    # Rewritten only when it changes: a newer SDC makes ORFS redo every stage
    if not os.path.exists(out) or open(out).read() != text:
        open(out, "w").write(text)


def canonicalize(design, variant, files, incdirs):
    """Reads the sources into the RTLIL ORFS's synthesis starts from
    (pnr/common/canonicalize.tcl), unless it is newer than every source."""
    d = DESIGNS[design]
    dirs = stage_dirs(design, variant)
    out = os.path.join(dirs["results"], "1_1_yosys_canonicalize.rtlil")
    srcs = files + [os.path.join(ROOT, "pnr", "common", "canonicalize.tcl")]
    if os.path.exists(out) and os.path.getmtime(out) > max(os.path.getmtime(f) for f in srcs):
        return 0
    for k in ("results", "logs"):
        os.makedirs(dirs[k], exist_ok=True)
    env = dict(os.environ,
               PNR_LIBS=PLATFORMS[d["platform"]]["lib"],
               PNR_MACRO_LIBS=" ".join(d["macro_libs"]),
               PNR_FILES=" ".join(files), PNR_INCDIRS=" ".join(incdirs),
               PNR_DEFINES=" ".join(d["defines"]), PNR_TOP=d["top"],
               PNR_PARAMS=" ".join(f"{k} {v}" for k, v in d["params"].items()),
               PNR_FLAT=" ; ".join(" ".join([m] + p) for m, p in d.get("flat", {}).items()),
               PNR_SLANG_ARGS=" ".join(d.get("slang_args", [])),
               PNR_BLACKBOX=" ; ".join(" ".join([m] + p) for m, p in d.get("blackbox", {}).items()),
               PNR_OUT=out)
    log = os.path.join(dirs["logs"], "1_1_yosys_canonicalize.log")
    print(f"+ yosys canonicalize.tcl > {os.path.relpath(log, ROOT)}", flush=True)
    rc = subprocess.run(["yosys", "-q", "-l", log, "-c", os.path.join(ROOT, "pnr", "common", "canonicalize.tcl")],
                        env=env).returncode
    if rc:
        print(f"canonicalize failed, see {log}", file=sys.stderr)
    return rc


def make(design, variant, target, period, extra, params=None):
    d = dict(DESIGNS[design])
    if params:
        d["params"] = dict(d["params"], **params)
        DESIGNS[design] = d
    files, incdirs = expand_flist(d["flist"], [], [])
    rc = canonicalize(design, variant, files, incdirs)
    if rc:
        return rc
    sdc = os.path.join(WORK, "sdc", f"{design}_{variant}.sdc")
    write_sdc(design, period, sdc)
    flow = os.environ.get("ORFS_FLOW", "/opt/torus/orfs/flow")
    args = ["make", "-C", flow, "--no-print-directory",
            f"DESIGN_CONFIG={os.path.join(ROOT, 'pnr', design, 'config.mk')}",
            f"WORK_HOME={WORK}", f"FLOW_VARIANT={variant}",
            f"VERILOG_FILES={' '.join(files)}",
            f"VERILOG_INCLUDE_DIRS={' '.join(incdirs)}",
            f"SDC_FILE={sdc}",
            f"YOSYS_EXE={shutil.which('yosys')}",
            f"OPENROAD_EXE={shutil.which('openroad')}",
            f"NUM_CORES={os.cpu_count()}"] + extra + [target]
    print("+", " ".join(a if " " not in a else f"'{a[:60]}...'" for a in args), flush=True)
    return subprocess.run(args).returncode


def stage_dirs(design, variant):
    sub = os.path.join(DESIGNS[design]["platform"], design, variant)
    return {k: os.path.join(WORK, k, sub) for k in ("logs", "reports", "results", "objects")}


def read_json(path):
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return {}


def collect(design, variant, period):
    """Metrics of the last stage reached, from ORFS's per-stage JSON logs."""
    dirs = stage_dirs(design, variant)
    logs = dirs["logs"]
    metrics = {}
    for name in sorted(os.listdir(logs)) if os.path.isdir(logs) else []:
        if name.endswith(".json"):
            metrics[name[:-5]] = read_json(os.path.join(logs, name))
    order = ["6_report", "5_3_fillcell", "5_2_route", "5_1_grt", "4_1_cts", "3_5_place_dp", "3_4_place_resized",
             "3_3_place_gp", "2_1_floorplan", "1_synth"]
    last = next((s for s in order if s in metrics), None)
    if last is None:
        return None
    m = metrics[last]

    def pick(*keys):
        for k in keys:
            for mk, v in m.items():
                if mk.endswith(k):
                    return v
        return None

    ws = pick("__timing__setup__ws")
    rep = {
        "design": design, "variant": variant, "stage": last, "period_ns": period,
        "die_um2": pick("__design__die__area"), "core_um2": pick("__design__core__area"),
        "stdcell_um2": pick("__design__instance__area__stdcell"),
        "macro_um2": pick("__design__instance__area__macros"),
        "macros": pick("__design__instance__count__macros"),
        "stdcells": pick("__design__instance__count__stdcell"),
        "utilization": pick("__design__instance__utilization"),
        "setup_ws_ns": ws, "setup_tns_ns": pick("__timing__setup__tns"),
        "hold_ws_ns": pick("__timing__hold__ws"),
        "achievable_period_ns": (period - ws) if isinstance(ws, (int, float)) else None,
        "wirelength_um": pick("__route__wirelength", "__detailedroute__wirelength"),
        "drc_errors": pick("__route__drc_errors"),
        "power_w": pick("__power__total"),
    }
    elapsed = 0.0
    for name in os.listdir(logs):
        if not name.endswith(".log"):
            continue
        for m in re.finditer(r"Elapsed time: ([\d:.]+)\[h:\]min:sec", open(os.path.join(logs, name), errors="replace").read()):
            secs = 0.0
            for part in m.group(1).split(":"):
                secs = secs * 60 + float(part)
            elapsed += secs
    rep["runtime_s"] = round(elapsed)
    return rep


def fmt(rep):
    def n(v, f="{:.0f}"):
        return "n/a" if v is None else f.format(v)
    lines = [f"PnR report: {rep['design']} ({rep['variant']}), {DESIGNS[rep['design']]['platform']}, "
             f"stage {rep['stage']}", "",
             f"  die area           {n(rep['die_um2'] and rep['die_um2'] / 1e6, '{:.2f}')} mm2 "
             f"(core {n(rep['core_um2'] and rep['core_um2'] / 1e6, '{:.2f}')} mm2)",
             f"  macros             {n(rep['macros'])}, {n(rep['macro_um2'] and rep['macro_um2'] / 1e6, '{:.2f}')} mm2",
             f"  standard cells     {n(rep['stdcells'])}, {n(rep['stdcell_um2'] and rep['stdcell_um2'] / 1e6, '{:.3f}')} mm2",
             f"  utilization        {n(rep['utilization'], '{:.3f}')}",
             f"  clock target       {rep['period_ns']} ns",
             f"  setup worst slack  {n(rep['setup_ws_ns'], '{:.3f}')} ns, total {n(rep['setup_tns_ns'], '{:.2f}')} ns",
             f"  achievable period  {n(rep['achievable_period_ns'], '{:.2f}')} ns",
             f"  hold worst slack   {n(rep['hold_ws_ns'], '{:.3f}')} ns",
             f"  wirelength         {n(rep['wirelength_um'] and rep['wirelength_um'] / 1e3, '{:.0f}')} mm",
             f"  DRC errors         {n(rep['drc_errors'])}",
             f"  power              {n(rep['power_w'] and rep['power_w'] * 1e3, '{:.1f}')} mW",
             f"  runtime            {rep['runtime_s']} s"]
    lt = rep.get("link_timing") or {}
    for k, name in (("x_out", "X links"), ("y_out", "Y links")):
        if k in lt:
            lines.append(f"  {name:18s} worst slack {lt[k]['worst_slack_ns']:.2f} ns ({lt['parasitics']})")
    for lab, v in sorted((rep.get("link_length") or {}).items()):
        lines.append(f"  link {lab:13s} longest {v['max_um'] / 1e3:.1f} mm, mean {v['mean_um'] / 1e3:.1f} mm")
    return "\n".join(lines) + "\n"


def link_timing(design, variant, tile_variant):
    """Worst link path per class (pnr/grid/linkpaths.tcl) on the last stage
    reached: global-route parasitics after routing, placement estimates before."""
    res = stage_dirs(design, variant)["results"]
    for stem, par in (("5_1_grt", "global_routing"), ("4_1_cts", "placement"), ("3_place", "placement"),
                      ("3_4_place_resized", "placement")):
        if os.path.exists(os.path.join(res, stem + ".odb")):
            break
    else:
        return None
    out = os.path.join(WORK, f"{design}_{variant}_links.txt")
    plat = PLATFORMS[DESIGNS[design]["platform"]]
    env = dict(os.environ, TILE_LIB=tile_abstract(design, tile_variant)[1], ODB=os.path.join(res, stem + ".odb"),
               STD_LIB=plat["lib"], SETRC=plat["setrc"],
               SDC=os.path.join(res, stem + ".sdc"), OUT=out, PARASITICS=par,
               GUIDES=os.path.join(res, "route.guide"))
    if subprocess.run(["openroad", "-exit", "-no_init", os.path.join(ROOT, "pnr", "grid", "linkpaths.tcl")],
                      env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        return None
    links = {"stage": stem, "parasitics": par}
    for line in open(out):
        t = line.split()
        if len(t) > 4 and t[1] == "paths":
            links[t[0]] = {"paths": int(t[2]), "worst_slack_ns": float(t[4]), "mean_slack_ns": float(t[6]),
                           "start": t[8], "end": t[10]}
    return links


def render(design, variant, rep, title):
    """Layout image of the last stage reached: DEF from its database, then
    scripts/plot_layout.py with the report as the note. Returns the PNG path."""
    res = stage_dirs(design, variant)["results"]
    odb = next((os.path.join(res, f) for f in ("6_final.odb", "5_3_fillcell.odb", "5_2_route.odb",
                                                "5_1_grt.odb", "4_1_cts.odb", "3_place.odb", "3_5_place_dp.odb", "3_4_place_resized.odb",
                                                "2_floorplan.odb") if os.path.exists(os.path.join(res, f))), None)
    if odb is None:
        return None
    df = odb[:-4] + "_layout.def"
    tcl = os.path.join(WORK, "write_def.tcl")
    open(tcl, "w").write("read_db $::env(ODB)\nwrite_def $::env(DEF)\n")
    if subprocess.run(["openroad", "-exit", "-no_init", tcl], env=dict(os.environ, ODB=odb, DEF=df),
                      stdout=subprocess.DEVNULL).returncode:
        return None
    lefs = list(PLATFORMS[DESIGNS[design]["platform"]]["lefs"])
    if is_grid(design):
        lefs.append(tile_abstract(design, rep.get("tile_variant", "cross"))[0])
    png = os.path.join(WORK, f"{design}_{variant}.png")
    note = "\n".join(l.strip() for l in fmt(rep).splitlines()[2:] if l.strip())
    rc = subprocess.run([sys.executable, os.path.join(ROOT, "scripts", "plot_layout.py"), "--def", df,
                         "--lef", *lefs, "--style", "grid" if is_grid(design) else "tile",
                         "--title", title, "--note", note, "--out", png]
                        + (["--links", "--links-json", png[:-4] + "_links.json"] if is_grid(design) else [])).returncode
    os.remove(df)
    return png if rc == 0 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("design", choices=sorted(DESIGNS))
    ap.add_argument("--period", type=float, default=20.0, help="clock period, ns")
    ap.add_argument("--variant", default="base")
    ap.add_argument("--to", choices=STAGES, default="finish", help="last stage")
    ap.add_argument("--floorplan", default="auto", help="auto (macro placer) or a pnr/<design>/<name>.tcl")
    ap.add_argument("--report-only", action="store_true")
    ap.add_argument("--set", nargs="*", default=[], metavar="VAR=VALUE", help="extra ORFS variables")
    ap.add_argument("--geom", nargs="*", default=[], metavar="NAME=VALUE",
                    help="override a hand floorplan's geometry (e.g. chan_v=800 for cross)")
    ap.add_argument("--abstract", action="store_true", help="tile: write the LEF and timing abstract of a finished run")
    grid = ap.add_argument_group("grid")
    grid.add_argument("--order", choices=["logical", "folded"], default="logical")
    grid.add_argument("--tile-variant", default="ft", help="tile run whose abstract is the macro")
    grid.add_argument("--channel", type=float, default=400.0, help="routing channel between tiles, um")
    grid.add_argument("--io-band", type=float, default=800.0, help="height of the I/O row band, um")
    grid.add_argument("--slice", type=int, default=0, help="LINK_SLICE (register slices per link)")
    grid.add_argument("--no-ft", action="store_true",
                      help="links only through the channels (soc_top FT_ORDER 0), not the tiles' feedthroughs")
    args = ap.parse_args()

    extra, params = list(args.set), None
    if is_grid(args.design):
        if args.variant == "base":
            args.variant = f"{args.order}_s{args.slice}" + ("_noft" if args.no_ft else "")
        if "--to" not in sys.argv:
            args.to = "grt"
        lef, lib = tile_abstract(args.design, args.tile_variant)
        tcl = os.path.join(WORK, "floorplan", f"{args.design}_{args.variant}.tcl")
        plat = PLATFORMS[DESIGNS[args.design]["platform"]]
        cm = plat.get("core_margin", 10.0)
        w, h = grid_floorplan(args.order, args.channel, args.io_band, lef, tcl, plat.get("site"), cm,
                              plat.get("fence", "EXCLUSIVE"))
        extra += [f"ADDITIONAL_LEFS={lef}", f"ADDITIONAL_LIBS={lib}", f"MACRO_PLACEMENT_TCL={tcl}",
                  f"DIE_AREA=0 0 {w:.2f} {h:.2f}", f"CORE_AREA={cm} {cm} {w - cm:.2f} {h - cm:.2f}"]
        params = {"LINK_SLICE": args.slice,
                  "FT_ORDER": 0 if args.no_ft else {"logical": 1, "folded": 2}[args.order]}
    else:
        if args.design == "tile_ihp" and args.floorplan == "auto":
            args.floorplan = "cross"   # its only floorplan
        extra = floorplan_vars(args.design, args.floorplan, args.variant, args.geom) + extra

    t0 = time.time()
    rc = 0
    if args.abstract:
        rc = make(args.design, args.variant, "do-generate_abstract", args.period, extra)
        return rc
    if not args.report_only:
        res = stage_dirs(args.design, args.variant)["results"]
        targets = {"finish": ["route", "do-finish"],
                   "grt": [os.path.join(res, "5_1_grt.odb")]}.get(args.to, [args.to])
        for t in targets:
            rc = make(args.design, args.variant, t, args.period, extra, params)
            if rc:
                break
    rep = collect(args.design, args.variant, args.period)
    if rep is None:
        print("no stage metrics found", file=sys.stderr)
        return rc or 1
    if is_grid(args.design):
        rep.update(order=args.order, tile_variant=args.tile_variant, channel_um=args.channel, link_slice=args.slice,
                   feedthroughs=not args.no_ft)
    out = os.path.join(WORK, f"{args.design}_{args.variant}_report")
    json.dump(rep, open(out + ".json", "w"), indent=1)
    title = (f"{args.design} ({args.variant}), {DESIGNS[args.design]['platform']}, {rep['stage']}, {args.period} ns"
             + (f", {args.order} order" if is_grid(args.design) else ""))
    if is_grid(args.design):
        rep["link_timing"] = link_timing(args.design, args.variant, args.tile_variant)
    png = render(args.design, args.variant, rep, title)
    if is_grid(args.design) and png and os.path.exists(png[:-4] + "_links.json"):
        rep["link_length"] = json.load(open(png[:-4] + "_links.json"))
        json.dump(rep, open(out + ".json", "w"), indent=1)
    if png:
        print(f"layout image {os.path.relpath(png, ROOT)}")
    txt = fmt(rep)
    open(out + ".txt", "w").write(txt)
    print(txt)
    if not args.report_only:
        print(f"wall time {time.time() - t0:.0f} s")
    return rc


if __name__ == "__main__":
    sys.exit(main())
