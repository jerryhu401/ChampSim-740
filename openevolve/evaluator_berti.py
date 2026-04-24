"""
OpenEvolve evaluator for the Berti prefetcher in ChampSim.

Flow per evaluation:
  1. Copy the evolved program file to prefetcher/berti/berti.cc
  2. Run config.sh + make to rebuild bin/champsim_berti
  3. Run champsim on a short trace
  4. Parse IPC + L2 prefetch stats → return metrics

Supports both native Windows (MinGW) and WSL execution.
"""

import re
import shutil
import subprocess
from pathlib import Path

REPO   = Path(__file__).resolve().parent.parent
SRC_CC = REPO / "prefetcher" / "berti" / "berti.cc"
BINARY = REPO / "bin" / "champsim_berti"
TRACE  = REPO / "traces" / "602.gcc_s-734B.champsimtrace.xz"

BASELINE_IPC  = 0.471
WARMUP_INSTR  = 200_000
SIM_INSTR     = 2_000_000
BUILD_TIMEOUT = 300
RUN_TIMEOUT   = 300


def _is_wsl() -> bool:
    try:
        with open("/proc/sys/kernel/osrelease") as f:
            return "microsoft" in f.read().lower()
    except Exception:
        return False


def _win(path: Path) -> str:
    """Convert a WSL/POSIX path to a Windows path string."""
    return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()


def _fail(reason: str):
    return {"combined_score": 0.0, "ipc": 0.0, "speedup": 0.0, "error": reason}


def evaluate(program_path: str) -> dict:
    # 1. Deploy evolved file
    shutil.copyfile(program_path, SRC_CC)

    IS_WSL = _is_wsl()

    if IS_WSL:
        win_repo   = _win(REPO)
        win_binary = _win(BINARY) + ".exe"
        win_trace  = _win(TRACE)
        mingw_path = "C:\\msys64\\mingw64\\bin;C:\\msys64\\usr\\bin"
        path_prefix = f'set "PATH={mingw_path};%PATH%" && '
        cd_prefix   = f'cd /d "{win_repo}" && '

        cmd = "/mnt/c/Windows/System32/cmd.exe"

        # 2a. Regenerate _configuration.mk for champsim_berti
        cfg = subprocess.run(
            [cmd, "/c",
             path_prefix + cd_prefix +
             "C:\\msys64\\mingw64\\bin\\python3.exe config.sh champsim_config_berti.json"],
            capture_output=True, text=True, timeout=60,
        )
        if cfg.returncode != 0:
            return _fail("config_failed: " + cfg.stderr[-400:])

        # 2b. Build
        build = subprocess.run(
            [cmd, "/c",
             path_prefix + cd_prefix + "mingw32-make -j4 bin/champsim_berti"],
            capture_output=True, text=True, timeout=BUILD_TIMEOUT,
        )
        if build.returncode != 0:
            return _fail("build_failed: " + build.stderr[-400:])

        # 3. Run champsim
        run = subprocess.run(
            [cmd, "/c",
             f'"{win_binary}" --warmup-instructions {WARMUP_INSTR}'
             f' --simulation-instructions {SIM_INSTR} "{win_trace}"'],
            capture_output=True, text=True, timeout=RUN_TIMEOUT,
        )
    else:
        import os
        mingw = r"C:\msys64\mingw64\bin"
        msys  = r"C:\msys64\usr\bin"
        _env = {**os.environ, "PATH": mingw + ";" + msys + ";" + os.environ.get("PATH", "")}

        # 2a. Regenerate _configuration.mk
        cfg = subprocess.run(
            [r"C:\msys64\mingw64\bin\python3.exe", "config.sh", "champsim_config_berti.json"],
            cwd=REPO, capture_output=True, text=True, timeout=60, env=_env,
        )
        if cfg.returncode != 0:
            return _fail("config_failed: " + cfg.stderr[-400:])


        # 2b. Remove stale config-specific objects to avoid linker errors when
        #     switching between configurations (generated_environment and main
        #     objects embed a config hash and must match _configuration.mk).
        import glob as _glob, os as _os
        for pattern in ["*.d", "generated_environment.*", "*_main.d", "*_main.o"]:
            for f in _glob.glob(str(REPO / ".csconfig" / "**" / pattern), recursive=True):
                try: _os.remove(f)
                except OSError: pass
            for f in _glob.glob(str(REPO / ".csconfig" / pattern)):
                try: _os.remove(f)
                except OSError: pass
        stale = REPO / "bin" / "champsim_berti.exe"
        if stale.exists():
            stale.unlink()

        # 2c. Build
        build = subprocess.run(
            [r"C:\msys64\mingw64\bin\mingw32-make.exe", "-j4", BINARY.relative_to(REPO).as_posix()],
            cwd=REPO, capture_output=True, text=True, timeout=BUILD_TIMEOUT, env=_env,
        )
        if build.returncode != 0:
            return _fail("build_failed: " + build.stderr[-400:])

        # 3. Run champsim (pass env so MinGW DLLs are found)
        run = subprocess.run(
            [str(BINARY) + ".exe",
             "--warmup-instructions", str(WARMUP_INSTR),
             "--simulation-instructions", str(SIM_INSTR),
             str(TRACE)],
            cwd=REPO, capture_output=True, text=True, timeout=RUN_TIMEOUT, env=_env,
        )

    if run.returncode != 0:
        return _fail("run_failed: " + (run.stderr or run.stdout)[-400:])

    out = run.stdout

    # 4. Parse metrics
    ipc_match = re.search(r"CPU 0 cumulative IPC:\s*([\d.]+)", out)
    pf_match  = re.search(
        r"cpu0->cpu0_L2C PREFETCH REQUESTED:\s*(\d+)\s+ISSUED:\s*(\d+)\s+USEFUL:\s*(\d+)\s+USELESS:\s*(\d+)",
        out,
    )
    if not ipc_match or not pf_match:
        return _fail("parse_failed: " + out[-400:])

    ipc      = float(ipc_match.group(1))
    issued   = int(pf_match.group(2))
    useful   = int(pf_match.group(3))
    useless  = int(pf_match.group(4))
    accuracy = useful / (useful + useless) if (useful + useless) > 0 else 0.0
    speedup  = ipc / BASELINE_IPC

    combined = 0.9 * speedup + 0.1 * accuracy

    return {
        "combined_score": combined,
        "speedup":  speedup,
        "ipc":      ipc,
        "accuracy": accuracy,
        "useful":   useful,
        "useless":  useless,
        "issued":   issued,
    }


if __name__ == "__main__":
    import sys, json
    print(json.dumps(evaluate(sys.argv[1]), indent=2))
