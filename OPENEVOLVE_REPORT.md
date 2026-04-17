# OpenEvolve × BOP Integration

Hooked the Best-Offset Prefetcher up to OpenEvolve so the LLM can iteratively
rewrite its internal policy and get scored by real ChampSim runs.

## What changed

- **Refactored `prefetcher/bop/bop.cc`**: moved all evolvable constants
  (`SCORE_MAX`, `ROUND_MAX`, `BAD_SCORE`, `PREFETCH_DEGREE`, `MSHR_THRESHOLD`),
  the `OFFSET_LIST`, and the `score_update()` function into a single anonymous
  namespace wrapped in `// EVOLVE-BLOCK-START` / `// EVOLVE-BLOCK-END` markers.
  `bop.h` keeps the class members + required ChampSim hooks (not evolvable).
- **New scaffold under [openevolve/](openevolve/)**:
  - [initial_program.cc](openevolve/initial_program.cc) — starting point, a copy of `bop.cc`.
  - [evaluator.py](openevolve/evaluator.py) — copies candidate → `bop.cc`,
    runs `make`, runs champsim on the 602.gcc trace (200K warmup, 2M sim),
    parses IPC + prefetch stats, returns
    `combined_score = 0.9 * speedup + 0.1 * accuracy`. Build or run failure
    returns score 0.
  - [config.yaml](openevolve/config.yaml) — gpt-5-mini via CMU LiteLLM gateway,
    `max_iterations: 20`, 2 islands, diff-based evolution, system prompt
    telling the LLM what it may and may not change.
  - [.env.example](openevolve/.env.example) — template for the API key.
- **`.gitignore`**: excludes `openevolve/.env` and `openevolve_output/`.

## Results (602.gcc_s, 200K warmup + 2M sim)

| Run                         | IPC   | Speedup vs no-pf | Accuracy | Issued | Useful | Useless | Combined |
|-----------------------------|-------|------------------|----------|--------|--------|---------|----------|
| Initial BOP                 | 0.818 | 1.74x            | 98.7%    | 31,680 | 15,357 | 204     | 1.66     |
| Smoke test (2 iters, best)  | 1.034 | 2.20x            | 97.6%    | 62,962 | 15,532 | 387     | 2.07     |
| **20-iter run (best: iter 6)** | **1.040** | **2.21x**    | 96.5%    | 60,296 | 14,967 | 536     | **2.08** |

Baseline `no` prefetcher IPC on this slice: 0.471. Best evolved BOP delivers
**+27% IPC over initial BOP** / **+121% over no-prefetcher**.

### Analysis of the 20-iter run

- **Early plateau.** Best candidate appeared at **iteration 6 of 20**; the
  remaining 14 iterations produced no improvement on `combined_score`.
- **Near-identical to the 2-iter result** (IPC 1.040 vs 1.034 — sub-1%
  gain). The LLM exhausted the easy wins in the first handful of tries.
- **Accuracy drift.** 98.7% → 96.5% as the evolver traded accuracy for
  coverage: `issued` roughly doubled (31.7k → 60.3k), `useless` jumped
  204 → 536. Still a net `combined_score` win because the fitness function
  weights speedup 9× over accuracy.
- **Root cause of the plateau.** Two likely contributors:
  1. **Small search space** — only 5 numeric constants, one ~26-element
     `OFFSET_LIST`, and a trivial `score_update()`. Not much left to vary.
  2. **Saturated single-trace signal** — once speedup is ~2.2x on gcc,
     run-to-run noise swamps subtle algorithmic gains.

What the LLM changed in the best program (diff vs initial):
- `PREFETCH_DEGREE`: 1 → 2 (more aggressive)
- `MSHR_THRESHOLD`: 0.7 → 0.6 (back off sooner under pressure)
- `ROUND_MAX`: 100 → 64 (adapt faster)
- `SCORE_MAX`: 31 → 28, `BAD_SCORE`: 1 → 0
- `OFFSET_LIST`: duplicated small offsets (`1,1,2,2,3,3,...`) so good
  candidates get multiple independent trials; dropped a few unusual offsets,
  added 64/96.
- `score_update`: `old+1` → `min(old+2, SCORE_MAX)` (faster convergence, capped).

Evolved source: [openevolve/openevolve_output/best/best_program.cc](openevolve/openevolve_output/best/best_program.cc).

## How to run

```bash
# 1. One-time: set the CMU gateway API key
cp openevolve/.env.example openevolve/.env
# edit openevolve/.env and paste the key (sk-...)
# then EVERY new shell:
export OPENAI_API_KEY="sk-..."         # or: source openevolve/.env

# 2. Run evolution (start small, 2 iters ≈ 4 min)
openevolve-run \
  openevolve/initial_program.cc \
  openevolve/evaluator.py \
  --config openevolve/config.yaml \
  --iterations 2

# 3. Inspect the best candidate
cat openevolve/openevolve_output/best/best_program_info.json
diff openevolve/initial_program.cc openevolve/openevolve_output/best/best_program.cc
```

For the real run, bump `--iterations` to 20 (matches `config.yaml` default) —
each iteration is ~1–2 min of LLM call + ChampSim build + 2M-instr sim.

### Shared trace set

To keep results comparable across teammates, we're standardizing on this
14-trace subset of the ChampSim SPEC 2017 bundle (one region per benchmark).
All downloaded into [traces/](traces/) from the Stony Brook mirror:

```bash
mkdir -p traces && cd traces
BASE=https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu
for t in \
  600.perlbench_s-210B  602.gcc_s-734B       603.bwaves_s-1080B \
  605.mcf_s-665B        619.lbm_s-2676B      620.omnetpp_s-141B \
  621.wrf_s-575B        623.xalancbmk_s-165B 631.deepsjeng_s-928B \
  638.imagick_s-824B    641.leela_s-149B     644.nab_s-5853B \
  654.roms_s-293B       657.xz_s-56B; do
    curl -sS -o "${t}.champsimtrace.xz" "${BASE}/${t}.champsimtrace.xz"
done
```

Total download is ~3–5 GB. The set covers branch-heavy (perlbench, gcc,
deepsjeng, leela, xz), memory-bound (mcf, omnetpp, lbm, roms), and streaming
(bwaves, wrf, imagick, nab, xalancbmk) workloads so the evaluator's averaged
score won't overfit to a single access pattern.

### Sanity-check the evaluator standalone

```bash
python openevolve/evaluator.py openevolve/initial_program.cc
```

Should print a JSON dict with `combined_score`, `speedup`, `ipc`, `accuracy`.

### Notes / caveats

- Evaluator currently scores only on **602.gcc_s**. We have 4 traces locally
  (perlbench, gcc, mcf, xalancbmk) — still a thin slice of SPEC 2017.
- Each failed build/run returns `combined_score = 0`, so the LLM gets a
  gradient away from broken code.
- `prefetcher/bop/bop.cc` is rewritten every evaluation. If you're hand-editing
  BOP in parallel, commit first — OpenEvolve will clobber it.

## Next step

1. **Download more SPEC 2017 traces.** Current 4-trace set is too narrow —
   the 20-iter run plateaued at iter 6, suggesting the single-trace signal is
   saturated. Aim for ~10–15 traces covering memory-bound (bwaves, lbm, roms),
   branch-heavy (deepsjeng, leela), and streaming (cam4, wrf) workloads from
   the standard ChampSim trace bundle.
2. **Extend [openevolve/evaluator.py](openevolve/evaluator.py)** to average
   `combined_score` across the expanded trace suite so the final number isn't
   gcc-only. Trade-off: wall time scales linearly with trace count per
   iteration.
3. Kick off a longer run on the multi-trace evaluator.
4. If time permits, port the same EVOLVE-BLOCK scaffold to IPCP and Berti.
