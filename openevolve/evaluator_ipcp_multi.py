"""
Multi-trace evaluator for the IPCP prefetcher in ChampSim.

Per evaluation:
  1. Copy candidate -> prefetcher/ipcp/ipcp.cc.
  2. Rebuild bin/champsim_ipcp (also rebuilds the no-pf bin/champsim if missing,
     so per-trace baselines can be cached).
  3. For each trace: run champsim_ipcp; compute speedup = ipc / no_pf_ipc and
     accuracy = useful / (useful + useless).
  4. Combined score = 0.9 * geomean(speedups) + 0.1 * mean(accuracy),
     averaged across traces. Geomean over speedups so a single bad trace can't
     hide behind a single great trace.

Build or run failure on any trace returns combined_score = 0 to give the LLM a
clean gradient away from broken candidates.

Per-trace no-prefetcher IPC values are cached in
openevolve/baselines_ipcp_multi.json so we only pay the baseline cost once.
"""

import json
import math
import os
import re
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
IPCP_CC = REPO / "prefetcher" / "ipcp" / "ipcp.cc"
BIN_IPCP = REPO / "bin" / "champsim_ipcp"
BIN_NOPF = REPO / "bin" / "champsim"
TRACES_DIR = REPO / "traces"
BASELINE_CACHE = REPO / "openevolve" / "baselines_ipcp_multi.json"

# Multi-set: a focused subset spanning branch-heavy, memory-bound, and streaming.
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

WARMUP_INSTR = 200_000
SIM_INSTR = 2_000_000
BUILD_TIMEOUT = 300
RUN_TIMEOUT = 300


def _existing_traces():
    out = []
    for name in TRACE_NAMES:
        p = TRACES_DIR / f"{name}.champsimtrace.xz"
        if p.exists():
            out.append((name, p))
    return out


def _fail(reason):
    return {"combined_score": 0.0, "error": reason}


_RE_IPC = re.compile(r"CPU 0 cumulative IPC:\s*([\d.]+)")
_RE_PF = re.compile(
    r"cpu0->cpu0_L2C PREFETCH REQUESTED:\s*(\d+)\s+ISSUED:\s*(\d+)\s+USEFUL:\s*(\d+)\s+USELESS:\s*(\d+)"
)


def _run_champsim(binary, trace_path):
    """Returns (ipc, useful, useless, issued) or raises RuntimeError."""
    proc = subprocess.run(
        [str(binary),
         "--warmup-instructions", str(WARMUP_INSTR),
         "--simulation-instructions", str(SIM_INSTR),
         str(trace_path)],
        cwd=REPO, capture_output=True, text=True, timeout=RUN_TIMEOUT,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"champsim run failed: {proc.stderr[-300:]}")
    ipc_m = _RE_IPC.search(proc.stdout)
    if not ipc_m:
        raise RuntimeError("could not parse IPC")
    ipc = float(ipc_m.group(1))
    pf_m = _RE_PF.search(proc.stdout)
    useful, useless, issued = 0, 0, 0
    if pf_m:
        issued = int(pf_m.group(2))
        useful = int(pf_m.group(3))
        useless = int(pf_m.group(4))
    return ipc, useful, useless, issued


def _baselines():
    """Per-trace no-prefetcher IPC. Cached on disk because it's expensive
    and the baseline binary doesn't change across iterations."""
    if BASELINE_CACHE.exists():
        try:
            data = json.loads(BASELINE_CACHE.read_text())
        except Exception:
            data = {}
    else:
        data = {}
    missing = [t for t, _ in _existing_traces() if t not in data]
    if missing and BIN_NOPF.exists():
        for tname, tpath in _existing_traces():
            if tname in data:
                continue
            try:
                ipc, *_ = _run_champsim(BIN_NOPF, tpath)
                data[tname] = ipc
                print(f"[baseline] {tname}: IPC={ipc:.4f}")
            except Exception as e:
                print(f"[baseline-fail] {tname}: {e}")
        BASELINE_CACHE.write_text(json.dumps(data, indent=2))
    return data


def evaluate(program_path):
    # 1. Deploy candidate (skip when caller passed the live source itself)
    src = Path(program_path).resolve()
    if src != IPCP_CC.resolve():
        shutil.copyfile(src, IPCP_CC)

    # 2. Build. Make sure .csconfig matches the IPCP build (it can be left in
    # a different state by sibling configs). Then incremental make.
    cfg = subprocess.run(
        ["./config.sh", "champsim_config_ipcp.json"],
        cwd=REPO, capture_output=True, text=True, timeout=BUILD_TIMEOUT,
    )
    if cfg.returncode != 0:
        return _fail("config_failed: " + cfg.stderr[-400:])
    build = subprocess.run(
        ["make", "-j4", "bin/champsim_ipcp"],
        cwd=REPO, capture_output=True, text=True, timeout=BUILD_TIMEOUT,
    )
    if build.returncode != 0:
        return _fail("build_failed: " + build.stderr[-400:])

    baselines = _baselines()
    traces = _existing_traces()
    if not traces:
        return _fail("no_traces_present")

    speedups, accuracies = [], []
    per_trace = {}
    for tname, tpath in traces:
        try:
            ipc, useful, useless, issued = _run_champsim(BIN_IPCP, tpath)
        except Exception as e:
            return _fail(f"run_failed[{tname}]: {e}")

        nopf_ipc = baselines.get(tname)
        if not nopf_ipc:
            # Skip traces without baselines rather than aborting — still useful
            # signal on the rest of the suite.
            continue

        speedup = ipc / nopf_ipc
        accuracy = useful / (useful + useless) if (useful + useless) > 0 else 0.0
        speedups.append(speedup)
        accuracies.append(accuracy)
        per_trace[tname] = {
            "ipc": ipc, "speedup": speedup, "accuracy": accuracy,
            "useful": useful, "useless": useless, "issued": issued,
        }

    if not speedups:
        return _fail("no_traces_scored")

    geo_speedup = math.exp(sum(math.log(s) for s in speedups) / len(speedups))
    mean_acc = sum(accuracies) / len(accuracies)
    combined = 0.9 * geo_speedup + 0.1 * mean_acc

    return {
        "combined_score": combined,
        "geomean_speedup": geo_speedup,
        "mean_accuracy": mean_acc,
        "num_traces": len(speedups),
        "per_trace": per_trace,
    }


if __name__ == "__main__":
    import sys
    print(json.dumps(evaluate(sys.argv[1]), indent=2))
