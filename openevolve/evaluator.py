"""
OpenEvolve evaluator for the BOP prefetcher in ChampSim.

Flow per evaluation:
  1. Copy the evolved program file to prefetcher/bop/bop.cc
  2. Run `make` to rebuild bin/champsim_bop
  3. Run champsim on a short trace
  4. Parse IPC + L2 prefetch stats → return metrics
"""

import os
import re
import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BOP_CC = REPO / "prefetcher" / "bop" / "bop.cc"
BINARY = REPO / "bin" / "champsim_bop"
TRACE = REPO / "traces" / "602.gcc_s-734B.champsimtrace.xz"
BASELINE_IPC = 0.471  # gcc no-prefetcher IPC for normalization

WARMUP_INSTR = 200_000
SIM_INSTR = 2_000_000
BUILD_TIMEOUT = 180
RUN_TIMEOUT = 180


def _fail(reason: str, ipc: float = 0.0):
    return {"combined_score": 0.0, "ipc": ipc, "speedup": 0.0, "error": reason}


def evaluate(program_path: str) -> dict:
    # 1. Deploy evolved file
    shutil.copyfile(program_path, BOP_CC)

    # 2. Rebuild (incremental — only bop.cc changed)
    build = subprocess.run(
        ["make", "-j4", str(BINARY.relative_to(REPO))],
        cwd=REPO, capture_output=True, text=True, timeout=BUILD_TIMEOUT,
    )
    if build.returncode != 0:
        return _fail("build_failed: " + build.stderr[-400:])

    # 3. Run champsim
    run = subprocess.run(
        [str(BINARY),
         "--warmup-instructions", str(WARMUP_INSTR),
         "--simulation-instructions", str(SIM_INSTR),
         str(TRACE)],
        cwd=REPO, capture_output=True, text=True, timeout=RUN_TIMEOUT,
    )
    if run.returncode != 0:
        return _fail("run_failed: " + run.stderr[-400:])

    out = run.stdout

    # 4. Parse metrics
    ipc_match = re.search(r"CPU 0 cumulative IPC:\s*([\d.]+)", out)
    pf_match = re.search(
        r"cpu0->cpu0_L2C PREFETCH REQUESTED:\s*(\d+)\s+ISSUED:\s*(\d+)\s+USEFUL:\s*(\d+)\s+USELESS:\s*(\d+)",
        out,
    )
    if not ipc_match or not pf_match:
        return _fail("parse_failed")

    ipc = float(ipc_match.group(1))
    issued = int(pf_match.group(2))
    useful = int(pf_match.group(3))
    useless = int(pf_match.group(4))
    accuracy = useful / (useful + useless) if (useful + useless) > 0 else 0.0
    speedup = ipc / BASELINE_IPC

    # Composite score: IPC is the bottom line; accuracy breaks ties & discourages pollution.
    combined = 0.9 * speedup + 0.1 * accuracy

    return {
        "combined_score": combined,
        "speedup": speedup,
        "ipc": ipc,
        "accuracy": accuracy,
        "useful": useful,
        "useless": useless,
        "issued": issued,
    }


if __name__ == "__main__":
    import sys, json
    print(json.dumps(evaluate(sys.argv[1]), indent=2))
