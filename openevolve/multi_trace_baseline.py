"""
Run all four ChampSim binaries (no-pf, BOP, IPCP, Berti) over the multi-trace
suite and emit a comparison table. Reuses the per-trace no-pf baseline cache
so we don't re-run that cost.

Usage:
    python3 openevolve/multi_trace_baseline.py
"""

import json
import math
import re
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TRACES_DIR = REPO / "traces"
BASELINE_CACHE = REPO / "openevolve" / "baselines_ipcp_multi.json"

TRACE_NAMES = [
    "600.perlbench_s-210B",
    "602.gcc_s-734B",
    "605.mcf_s-665B",
    "619.lbm_s-2676B",
    "620.omnetpp_s-141B",
    "623.xalancbmk_s-165B",
    "631.deepsjeng_s-928B",
    "657.xz_s-56B",
]

BINARIES = [
    ("no-pf", REPO / "bin" / "champsim"),
    ("BOP",   REPO / "bin" / "champsim_bop"),
    ("IPCP",  REPO / "bin" / "champsim_ipcp"),
    ("Berti", REPO / "bin" / "champsim_berti"),
]

WARMUP = 200_000
SIM = 2_000_000
TIMEOUT = 300

_RE_IPC = re.compile(r"CPU 0 cumulative IPC:\s*([\d.]+)")
_RE_PF = re.compile(
    r"cpu0->cpu0_L2C PREFETCH REQUESTED:\s*(\d+)\s+ISSUED:\s*(\d+)\s+USEFUL:\s*(\d+)\s+USELESS:\s*(\d+)"
)


def run_one(binary, trace):
    proc = subprocess.run(
        [str(binary), "--warmup-instructions", str(WARMUP),
         "--simulation-instructions", str(SIM), str(trace)],
        cwd=REPO, capture_output=True, text=True, timeout=TIMEOUT,
    )
    if proc.returncode != 0:
        return None
    ipc = float(_RE_IPC.search(proc.stdout).group(1))
    m = _RE_PF.search(proc.stdout)
    if m:
        issued, useful, useless = int(m.group(2)), int(m.group(3)), int(m.group(4))
    else:
        issued = useful = useless = 0
    return {"ipc": ipc, "issued": issued, "useful": useful, "useless": useless}


def main():
    baselines = {}
    if BASELINE_CACHE.exists():
        baselines = json.loads(BASELINE_CACHE.read_text())

    results = {name: {} for name, _ in BINARIES}

    for tname in TRACE_NAMES:
        tpath = TRACES_DIR / f"{tname}.champsimtrace.xz"
        if not tpath.exists():
            print(f"[skip] {tname}: missing")
            continue
        print(f"\n=== {tname} ===")
        for label, binary in BINARIES:
            r = run_one(binary, tpath)
            if r is None:
                print(f"  [{label}] FAILED")
                continue
            results[label][tname] = r
            print(f"  [{label}] IPC={r['ipc']:.4f}  useful={r['useful']:>6}  useless={r['useless']:>6}")

    # Use no-pf as the per-trace speedup baseline
    nopf = results["no-pf"]
    summary = {}
    for label, _ in BINARIES:
        speedups, accs = [], []
        for tname, r in results[label].items():
            base = nopf.get(tname, {}).get("ipc")
            if not base:
                continue
            speedups.append(r["ipc"] / base)
            tot = r["useful"] + r["useless"]
            if tot > 0:
                accs.append(r["useful"] / tot)
        geo = math.exp(sum(math.log(s) for s in speedups) / len(speedups)) if speedups else 0.0
        mean_acc = sum(accs) / len(accs) if accs else 0.0
        summary[label] = {"geomean_speedup": geo, "mean_accuracy": mean_acc, "n": len(speedups)}

    print("\n\n========== Summary ==========")
    print(f"{'prefetcher':<10} {'geomean speedup':>16} {'mean acc':>10} {'n':>4}")
    for label, s in summary.items():
        print(f"{label:<10} {s['geomean_speedup']:>16.4f} {s['mean_accuracy']:>10.4f} {s['n']:>4}")

    out = REPO / "openevolve" / "multi_trace_summary.json"
    out.write_text(json.dumps({"per_trace": results, "summary": summary}, indent=2))
    print(f"\nWrote {out}")


if __name__ == "__main__":
    main()
