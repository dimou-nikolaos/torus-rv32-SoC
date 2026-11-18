#!/usr/bin/env python3
"""Regression runner: one command for everything in the current milestone.

  scripts/regress.py                 # build, then run the M0 regression
  scripts/regress.py --seed 1234     # replay a whole run
  scripts/regress.py -k isa          # only tests whose name contains "isa"
  scripts/regress.py --synth         # also run the Yosys area report

Every random test gets a seed derived from the run seed and its name; both are
printed, and a failing test prints the exact command that replays it. Results
go to build/regress/: one log per test, junit.xml and summary.txt (which starts
with the tool versions).
"""
import argparse
import concurrent.futures as cf
import hashlib
import os
import shlex
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OUT = BUILD / "regress"


def derive_seed(base, name):
    return int.from_bytes(hashlib.sha256(f"{base}:{name}".encode()).digest()[:4], "little")


class Test:
    def __init__(self, name, cmd, seeded=True, timeout=600, expect_fail=False):
        self.name, self.cmd, self.seeded, self.timeout = name, cmd, seeded, timeout
        self.expect_fail = expect_fail  # negative tests: must exit nonzero


def discover(args):
    lint = ["verilator", "--lint-only", "-Wall", "-Werror-USERFATAL", "-Werror-USERERROR",
            "rtl/lint/vendor.vlt"]
    tests = [Test("lint.tile", lint + ["--top-module", "tile_top", "-f", "rtl/tile/tile_sim.f"],
                  seeded=False)]
    for i in range(args.spm_seeds):
        tests.append(Test(f"unit.spm.{i}", [str(BUILD / "tb/unit/tb_spm"), "--cycles=200000"]))
    tests.append(Test("unit.flit", [str(BUILD / "tb/unit/tb_flit")]))
    for cfg in ("s0_d4", "s1_d6", "s2_d8", "s0_d3", "s2_d7"):
        tests.append(Test(f"unit.link.{cfg}", [str(BUILD / f"tb/unit/tb_link_{cfg}")]))
    for i in range(3):
        tests.append(Test(f"unit.niu.{i}", [str(BUILD / "tb/unit/tb_niu")]))
    for depth in (8, 16):
        tests.append(Test(f"unit.cdc.d{depth}", [str(BUILD / f"tb/unit/tb_cdc_d{depth}"), "--quiet"]))
    for r in range(6):   # AXI : fabric clock ratios
        tests.append(Test(f"unit.io.r{r}", [str(BUILD / "tb/unit/tb_io"), f"--ratio={r}"]))
    for d in (0, 1):
        for sl in (0, 1, 2):
            tests.append(Test(f"unit.router.p{d}_s{sl}", [str(BUILD / f"tb/unit/tb_router_p{d}_s{sl}")]))
    for cfg in ("2x2_p0_s0", "2x2_p1_s0", "4x4_p0_s0", "4x4_p1_s1", "4x4_p0_s2", "3x5_p0_s0", "3x5_p1_s0",
                "4x4_p01_s0", "2x3_p01_s0_io", "4x5_p01_s0_io", "4x5_p01_s1_io", "3x4_p01_s2_io"):
        tests.append(Test(f"noc.{cfg}", [str(BUILD / f"tb/unit/tb_noc_{cfg}")]))
    tests.append(Test("noc.soak_4x4", [str(BUILD / "tb/unit/tb_noc_4x4_p0_s0"), "--soak=2000000"], timeout=1200))
    # Both planes with the I/O row, as in the 4x4 SoC
    tests.append(Test("noc.soak_4x5_p01_io", [str(BUILD / "tb/unit/tb_noc_4x5_p01_s0_io"), "--soak=2000000"],
                      timeout=1200))
    # Without the bubble rule the torus must deadlock and the watchdog must see it
    for cfg in ("3x3_p0_s0", "4x4_p0_s0"):
        tests.append(Test(f"noc.{cfg}_nobubble_deadlocks",
                          [str(BUILD / f"tb/unit/tb_noc_{cfg}_nobubble"), "--expect-deadlock"]))
    # Elaboration must reject an input buffer below the credit round trip
    tests.append(Test("lint.link_too_shallow", lint + [
        "--top-module", "tb_link_top", "-GSLICE=1", "-GDEPTH=5",
        "-f", "rtl/noc/noc.f", "tb/unit/link/tb_link_top.sv"], seeded=False, expect_fail=True))
    tests.append(Test("lint.router_too_shallow", lint + [
        "--top-module", "noc_router", "-GLINK_SLICE=1", "-GDEPTH=6",
        "-f", "rtl/noc/noc.f"], seeded=False, expect_fail=True))
    # Elaboration must reject a READ_REQ queue below the grid's READ_REQs in flight
    tests.append(Test("lint.niu_rq_too_shallow", lint + [
        "--top-module", "niu", "-GRQ_NEEDED=24", "-GRQ_DEPTH=23", "-f", "rtl/tile/niu.f"],
        seeded=False, expect_fail=True))
    tb = str(BUILD / "tb/soc/tb_soc_1x1")
    tests.append(Test("tile.hello", [tb, str(BUILD / "sw/hello.elf")]))
    tests.append(Test("tile.regtest", [tb, str(BUILD / "sw/regtest.elf")]))
    tests.append(Test("tile.dmatest", [tb, str(BUILD / "sw/dmatest.elf")]))
    tests.append(Test("tile.nocops", [tb, str(BUILD / "sw/nocops.elf")]))
    soc22 = str(BUILD / "tb/soc/tb_soc_2x2")
    tests.append(Test("soc2x2.pingpong", [soc22, str(BUILD / "sw/pingpong.elf")]))
    tests.append(Test("soc2x2.dmatest", [soc22, str(BUILD / "sw/dmatest.elf")]))
    tests.append(Test("soc2x2.hello", [soc22, str(BUILD / "sw/hello.elf")]))
    tests.append(Test("soc2x2.nocops", [soc22, str(BUILD / "sw/nocops.elf")]))
    soc44 = str(BUILD / "tb/soc/tb_soc_4x4")
    tests.append(Test("soc4x4.nocops", [soc44, str(BUILD / "sw/nocops.elf")], timeout=1200))
    # M2 exit test: memcpy launched from C++ through the I/O tile, checked
    # bit-exact and against the performance model (exact per-link flit counts,
    # copy time within tolerance)
    # M3: (-1,-1) runs on NoC1 only, (1,-1) on both planes (by row)
    for grid, shift in (("2x2", "1,1"), ("4x4", "1,1"), ("4x4", "2,2"), ("4x4", "-1,-1"), ("4x4", "1,-1")):
        tag = shift.replace(",", "_").replace("-", "m")
        tests.append(Test(f"soc{grid}.memcpy_s{tag}", [
            sys.executable, "model/perf_model.py", "run-memcpy", "--host", str(BUILD / f"tb/soc/host_memcpy_{grid}"),
            "--elf", str(BUILD / "sw/memcpy.elf"), f"--shift={shift}"], timeout=1200))
    for elf in sorted((BUILD / "sw/isa").glob("*.elf")):
        tests.append(Test(f"isa.{elf.stem}", [tb, str(elf)]))
    if args.synth:
        tests.append(Test("synth.tile", [sys.executable, "scripts/synth_area.py", "--top", "tile_top",
                                         "--flist", "rtl/tile/tile_syn.f", "--units", "core_wrap",
                                         "spm_banks", "obi_spm_adapter", "tile_ctrl", "counter_bank",
                                         "sim_console", "niu", "noc_router", "--out", str(BUILD / "syn/tile")],
                          seeded=False, timeout=1800))
        tests.append(Test("synth.io", [sys.executable, "scripts/synth_area.py", "--top", "io_tile",
                                       "--flist", "rtl/special/io_syn.f", "--units", "io_axi_front", "io_bridge",
                                       "io_regs", "io_cdc_fifo", "--out", str(BUILD / "syn/io")],
                          seeded=False, timeout=1800))
    tests.append(Test("lint.io_tile", lint + ["--top-module", "io_tile", "-f", "rtl/special/io_syn.f"],
                      seeded=False))
    tests.append(Test("lint.soc_3x2", lint + ["--top-module", "soc_top", "-GNX=3", "-GNY=2",
                                             "-f", "rtl/soc/soc_sim.f"], seeded=False))
    return tests


def run(test, base_seed):
    cmd = list(test.cmd)
    seed = None
    if test.seeded:
        seed = derive_seed(base_seed, test.name)
        cmd.append(f"--seed={seed}")
    log = OUT / f"{test.name}.log"
    t0 = time.time()
    try:
        p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, timeout=test.timeout)
        rc, out = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        rc, out = -1, (e.stdout or "") + f"\nTIMEOUT after {test.timeout}s\n"
    dt = time.time() - t0
    if test.expect_fail:
        rc = 0 if rc != 0 else 1
        out += "\n(negative test: a nonzero exit is the expected result)\n"
    log.write_text(f"$ {shlex.join(cmd)}\n{out}")
    return test, rc, dt, seed, out, cmd


def versions():
    p = subprocess.run([str(ROOT / "tools/print_versions.sh")], cwd=ROOT, capture_output=True, text=True)
    return p.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=None, help="run seed (default: random)")
    ap.add_argument("-k", dest="filter", default="", help="only tests whose name contains this")
    ap.add_argument("-j", type=int, default=os.cpu_count(), help="parallel tests")
    ap.add_argument("--spm-seeds", type=int, default=4, help="scratchpad unit test seeds")
    ap.add_argument("--synth", action="store_true", help="include the Yosys area report")
    ap.add_argument("--no-build", action="store_true", help="skip the CMake build step")
    args = ap.parse_args()

    base = args.seed if args.seed is not None else int.from_bytes(os.urandom(4), "little")
    print(f"RUN SEED={base}")

    if not args.no_build:
        if not (BUILD / "build.ninja").exists():
            subprocess.run(["cmake", "-S", ".", "-B", "build", "-G", "Ninja",
                            "-DCMAKE_BUILD_TYPE=Release"], cwd=ROOT, check=True)
        b = subprocess.run(["ninja", "-C", "build"], cwd=ROOT)
        if b.returncode != 0:
            print("BUILD FAILED")
            return 1

    OUT.mkdir(parents=True, exist_ok=True)
    tests = [t for t in discover(args) if args.filter in t.name]
    results = []
    with cf.ThreadPoolExecutor(max_workers=args.j) as ex:
        for test, rc, dt, seed, out, cmd in ex.map(lambda t: run(t, base), tests):
            ok = rc == 0
            print(f"{'PASS' if ok else 'FAIL'}  {test.name:32s} {dt:7.1f}s"
                  + (f"  seed={seed}" if seed is not None else ""))
            if not ok:
                print(f"      replay: {shlex.join(cmd)}")
                print("      " + "\n      ".join(out.strip().splitlines()[-6:]))
            results.append((test, ok, dt, seed, out))

    # JUnit
    suite = ET.Element("testsuite", name="torus-regress", tests=str(len(results)),
                       failures=str(sum(not r[1] for r in results)),
                       time=f"{sum(r[2] for r in results):.1f}")
    props = ET.SubElement(suite, "properties")
    ET.SubElement(props, "property", name="run_seed", value=str(base))
    for test, ok, dt, seed, out in results:
        tc = ET.SubElement(suite, "testcase", classname=test.name.split(".")[0], name=test.name,
                           time=f"{dt:.1f}")
        if seed is not None:
            ET.SubElement(tc, "properties").append(ET.Element("property", name="seed", value=str(seed)))
        if not ok:
            f = ET.SubElement(tc, "failure", message="nonzero exit")
            f.text = out[-4000:]
        ET.SubElement(tc, "system-out").text = out[-20000:]
    ET.ElementTree(suite).write(OUT / "junit.xml", encoding="utf-8", xml_declaration=True)

    npass = sum(r[1] for r in results)
    summary = (f"Tool versions:\n{versions()}\nRun seed: {base}\n"
               f"{npass}/{len(results)} passed\n"
               + "".join(f"{'PASS' if ok else 'FAIL'} {t.name}\n" for t, ok, *_ in results))
    (OUT / "summary.txt").write_text(summary)
    print(f"\n{npass}/{len(results)} passed  (logs, junit.xml, summary.txt in {OUT.relative_to(ROOT)})")
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
