# Generalising LLM-Driven Prefetcher Evolution Across Multiple SPEC Workloads

**15-740 Final Project Report**

---

## 0. Abstract

Automated optimisation of microarchitectural components with LLM-driven evolutionary search (OpenEvolve) has recently been shown to discover strong configurations for individual prefetchers when scored on a single trace. We extend that line of work by porting the evaluator to a multi-trace fitness signal and demonstrating that a single-trace optimum is a poor proxy for cross-workload performance. Targeting the IP-Classifier Prefetcher (IPCP) at the L2 cache in ChampSim, we evaluate 58 successive candidate configurations over an eight-trace SPEC CPU 2017 subset. The best candidate (v40) raises the geometric-mean speedup over no-prefetcher from 1.293x to 1.372x and the composite score from 1.243 to 1.316 — a 5.9% relative improvement over the previously reported gcc-tuned best — while also slightly outperforming that prior best on its own gcc-only metric (2.21 vs. 2.18). The dominant gains come from xalancbmk (+0.30 IPC) and lbm (+0.05 IPC). The principal insight is mechanical: the poster-evolved design left `GS_DEGREE = 3` because gcc never exposed deep-stream headroom, but xalancbmk absorbs prefetch chains all the way to the 4-KiB page boundary; multi-trace fitness exposes a single hyper-parameter that the gcc-only signal could not see.

## 1. Introduction

Hardware prefetchers hide DRAM latency by predicting future memory accesses. Modern designs (BOP, IPCP, Berti) expose dozens of numeric thresholds, scoring functions, and classification rules; the design space is large and sensitive to the workload mix used during tuning. Last semester's poster project applied OpenEvolve, an LLM-in-the-loop evolutionary search system, to three such prefetchers in ChampSim and reported substantial speedups on a single SPEC CPU 2017 trace, `602.gcc_s`. Each evolved prefetcher achieved a 1.3–1.4 multi-trace combined score on the eight SPEC traces we measure here despite scoring above 2.0 on the gcc-only fitness signal used during evolution.

This project asks: **does LLM-driven prefetcher evolution actually generalise, and if not, what changes when fitness is measured across a heterogeneous workload mix?** We focus on IPCP, an L2 prefetcher that classifies each load PC into one of four classes (CS, CPLX, GS, NL) and applies a class-specific prefetch policy. We instrument an eight-trace evaluator covering branch-heavy, memory-bound, and streaming workloads, drive 58 successive candidate configurations through it, and analyse where multi-trace headroom is real, where it is structural, and where it is bounded by the underlying cache architecture.

The main research question is whether the poster's headline single-trace gains survive a more rigorous evaluation, and whether the multi-trace signal exposes additional architectural levers that the single-trace fitness obscured. They do not survive unchanged, and it does — both answers turn out to be quantitatively significant.

## 2. Related Work

**IPCP** (Pakalapati & Panda, ISCA 2020) defines the four-class IP-Classifier prefetcher we extend. The implementation in this repository is a slightly reduced port; we treat its as-shipped configuration as the "stock" baseline.

**Best-Offset Prefetcher** (Michaud, HPCA 2016) and **Berti** (Navarro Torres et al., MICRO 2022) are alternative designs available in the repository. The original poster project evolved all three; we focus on IPCP in this report because it had the largest reported single-trace gain (+34%) and the most plausible structural lever (the global-stream classifier) on which to test multi-trace generalisation.

**OpenEvolve** is an open-source reimplementation of the AlphaEvolve (Romera-Paredes et al., 2024) pattern: an LLM proposes diff-based mutations to a marked region of source code, each candidate is compiled and scored by a user-supplied evaluator, and a multi-island genetic algorithm maintains a population. The poster project used `gpt-5-mini` via the CMU LiteLLM gateway with a population of 30 per island and a 90/10 IPC-vs-accuracy fitness function on `602.gcc_s`.

**ChampSim** (Gober et al., 2022) is the trace-driven simulator used. We use the configuration in `champsim_config_ipcp.json` (1 GHz × 4 OoO core, 64×8 L1I / 64×12 L1D / 1024×8 L2C / 2048×16 LLC, DDR4-3200) without modification so that all numbers are directly comparable to the poster's.

**The poster project itself** is the closest prior work and the one we directly extend. Its central contribution — and the one most relevant to our results — is the iteration-1 IPCP mutation that converted `classify_ip()` from a `static` member function to a non-`static` one, allowing it to read the class-instance field `global_stream_dir`. Before the change, `classify_ip()` saw only IP-local stride history; after the change, it could route IPs that happened to be moving in the same direction as the global workload directly to the GS class, which significantly raises `gcc_s` IPC. We carry this structural change forward unchanged into all of our candidates; every numeric and structural improvement we report is layered on top of it.

This report's distinguishing baseline is therefore **the poster's evolved IPCP**, not the stock IPCP.

## 3. Method

### 3.1 What we built

We extend the poster project's evaluation harness with three new pieces:

1. **`openevolve/evaluator_ipcp_multi.py`** — a multi-trace replacement for the original gcc-only evaluator. It deploys a candidate `ipcp.cc`, runs `./config.sh champsim_config_ipcp.json`, rebuilds `bin/champsim_ipcp`, and runs the binary against each of eight traces with 200K warmup + 2M sim instructions. Per-trace no-prefetcher IPCs are computed once and cached on disk in `openevolve/baselines_ipcp_multi.json` to amortise their cost across iterations. The evaluator returns the same dictionary shape OpenEvolve expects (`combined_score`, plus per-trace diagnostics) so it is a drop-in replacement when an API key is available.

2. **`openevolve/multi_trace_baseline.py`** — a one-shot harness that runs `bin/champsim` (no-prefetcher), `bin/champsim_bop`, `bin/champsim_ipcp`, and `bin/champsim_berti` over the eight-trace suite and emits a comparison table. We use this to establish the multi-trace baseline numbers reported below for every prefetcher.

3. **`openevolve/config_ipcp_multi.yaml`** — an OpenEvolve config that targets the multi-trace evaluator with system-prompt language explicitly framing the goal as geometric-mean speedup over a heterogeneous trace mix.

### 3.2 The fitness function

We use the same 90/10 IPC/accuracy weighting as the poster but aggregate across traces with a geometric mean so a single bad trace cannot be hidden by a single great one:

`combined_score = 0.9 * geomean_over_traces(speedup) + 0.1 * mean_over_traces(accuracy)`

where `speedup = ipc / per_trace_no_pf_ipc` and `accuracy = useful / (useful + useless)` are computed independently for each trace. The 90/10 weighting is unchanged so that single-trace numbers under our evaluator and the poster's evaluator are directly comparable when restricted to gcc.

### 3.3 The trace suite

Eight SPEC CPU 2017 traces (≈3.6 GiB total, downloaded from the Stony Brook mirror): `600.perlbench_s-210B`, `602.gcc_s-734B`, `605.mcf_s-665B`, `619.lbm_s-2676B`, `620.omnetpp_s-141B`, `623.xalancbmk_s-165B`, `631.deepsjeng_s-928B`, `657.xz_s-56B`. The set spans branch-heavy (perlbench, gcc, deepsjeng, xz), memory-bound (mcf, omnetpp), and streaming (lbm, xalancbmk) behaviour.

### 3.4 The evolution loop

OpenEvolve requires an LLM endpoint. The repository's configuration targets the CMU LiteLLM gateway, which requires an `OPENAI_API_KEY` we did not have on the machine used for these experiments (`401 Unauthorized` against `https://ai-gateway.andrew.cmu.edu/v1/models` confirmed this). Rather than block on credential provisioning, we adopted a manual workflow: a human operator (or, in our case, an LLM coding assistant playing the role of the OpenEvolve mutation agent directly) reads the current champion's per-trace numbers from the multi-trace evaluator, hypothesises a mutation, edits a candidate file, re-runs the evaluator, and either accepts or discards the change. The harness, fitness function, and candidate-vs-best comparison are unchanged from what `openevolve-run` would have used — only the candidate-proposal step is human-in-the-loop. We retain `openevolve/initial_program_ipcp_multi.cc` and `config_ipcp_multi.yaml` so the same study can be re-run end-to-end with `openevolve-run` once a key is available.

We tested 58 candidates (v1 through v58). Each evaluation takes ≈30s × 8 traces ≈ 4 min on a 28-core Xeon. All candidate sources are preserved in `openevolve/candidates_v*.cc` for reproducibility.

### 3.5 Baselines and metrics

We compare against four baselines on the eight-trace suite: no prefetcher, BOP (committed in repo), IPCP (committed in repo — this is the poster's evolved version), and Berti (committed in repo). For the gcc-only comparison we use the poster's published numbers directly.

## 4. Experimental Results

### 4.1 Multi-trace baseline of all four prefetchers

| Prefetcher | Geomean speedup | Mean accuracy | Combined |
|---|---:|---:|---:|
| no prefetcher | 1.000 | — | — |
| BOP (committed) | 1.218 | 82.2% | 1.180 |
| IPCP (poster-evolved, committed) | **1.293** | 79.1% | **1.243** |
| Berti (committed) | 1.294 | 78.0% | 1.243 |

The poster's evolved IPCP and Berti are nearly tied at the multi-trace level, despite both reporting >2.0 single-trace gcc scores. Both are catastrophically inaccurate on memory-bound traces: IPCP issues 168K prefetches on `mcf` with only 28.6% accuracy, and Berti issues 119K with 16.9% accuracy.

### 4.2 The evolution: from baseline to v40

The fitness landscape behaved in three distinct regimes.

**Iterations v1–v14 (numeric tightening): negative or flat.** Our initial hypotheses all involved making the existing classifier *more conservative* on the assumption that mcf/omnetpp pollution was the dominant cost. Adding `confidence ≥ 1` requirements (v1, v3, v9), a small-stride filter at limit 4 (v2), demoting "stale" GS classifications (v14), and tighter MSHR thresholds (v11) all produced regressions or flat scores (1.165–1.248). The lesson: tightening helped the bad traces but harmed gcc and xalancbmk far more than it improved mcf, because the over-aggressive prefetches were producing real IPC even on the noisy traces.

**Iterations v15–v23 (aggression-up): the breakthrough.** The gradient flipped when we tested the opposite hypothesis. Raising `CS_DEGREE` from 4 to 6 produced a small gain (v15: 1.248). Raising `GS_DEGREE` from 3 to 4 jumped to 1.255 (v16). Following the gradient — `GS=5` (v19: 1.267), `GS=6` (v22: 1.290), `GS=8` (v23: 1.306) — every step up improved the score, with most of the gain concentrated on a single trace, xalancbmk.

**Iterations v24–v40 (refinement at the new operating point): plateau formation.** Pushing further (`GS=10, 12, 16, 24, 32`) converged on `GS=16` as the practical ceiling: v28 (`GS=16`) scored 1.3134, and v30/v31 (`GS=24`/`32`) tied at 1.3134. The reason is architectural: the L2 prefetcher refuses cross-page prefetches, and a 4-KiB page contains 64 cache lines, so beyond a certain depth additional prefetches are silently dropped by `issue_prefetch`'s page-boundary check. Within that envelope, narrowing `GS_STRIDE_LIMIT` to 8 (v36) and lifting `CPLX_DEGREE` to 8 (v40) added small accuracy-side gains.

The final champion (v40) differs from the poster-evolved IPCP in five constants and one classifier line:

```cpp
constexpr int CS_DEGREE         = 8;     // was 4
constexpr int CPLX_DEGREE       = 8;     // was 3
constexpr int GS_DEGREE         = 16;    // was 3 — the dominant change
constexpr int GS_STRIDE_LIMIT   = 8;     // new constant
constexpr int CS_CONFIDENCE_THRESHOLD = 4;
constexpr int CPLX_CONFIDENCE_THRESHOLD = 2;
constexpr int STREAM_DETECT_THRESHOLD = 3;
constexpr int CONFIDENCE_SAT_MAX = 8;
constexpr double MSHR_THRESHOLD = 0.6;
```

with one new gate inside `classify_ip()`:

```cpp
if (dir_match && std::abs(new_stride) <= GS_STRIDE_LIMIT) {
  if (confidence < CS_CONFIDENCE_THRESHOLD || old_class == GS) return GS;
}
```

### 4.3 Per-trace breakdown

| Trace | no-pf | poster IPCP | v40 | speedup vs no-pf | Δ IPC vs poster |
|---|---:|---:|---:|---:|---:|
| 600.perlbench_s | 1.935 | 1.995 | 1.997 | 1.03× | +0.002 |
| 602.gcc_s | 0.469 | 1.088 | 1.106 | 2.36× | +0.018 |
| 605.mcf_s | 0.210 | 0.260 | 0.273 | 1.30× | +0.013 |
| 619.lbm_s | 0.583 | 0.708 | 0.758 | 1.30× | +0.050 |
| 620.omnetpp_s | 0.226 | 0.230 | 0.232 | 1.03× | +0.002 |
| **623.xalancbmk_s** | 0.411 | 0.739 | **1.024** | **2.49×** | **+0.285** |
| 631.deepsjeng_s | 1.090 | 1.109 | 1.112 | 1.02× | +0.003 |
| 657.xz_s | 2.156 | 2.508 | 2.508 | 1.16× | +0.000 |

No trace regresses. The aggregate is dominated by xalancbmk (+38% IPC) and lbm (+7%). On gcc, v40's 1.106 IPC slightly exceeds the poster's reported 1.088 — meaning that on the *poster's own metric* (single-trace gcc with the 0.9/0.1 formula), v40 also wins narrowly (2.21 vs 2.18). The multi-trace optimum is also a slightly better gcc-only optimum.

### 4.4 Iterations v41–v58: the structural plateau

We invested 18 further iterations probing structural changes that fell outside the original `EVOLVE-BLOCK` markers. None beat v40 by more than measurement noise (≤0.0001 combined score), and several exposed instructive failure modes:

- **v50** routed GS prefetches at the IP's actual stride instead of `global_stream_dir = ±1` and lost 2.6% combined score. The ±1 chain is good not because the global direction is meaningful but because the immediately-adjacent cache line is hot under spatial locality; replacing it with a strided chain skips that line entirely.
- **v54** removed the page-boundary check inside `issue_prefetch` to break the GS-degree ceiling. Cross-page prefetches do propagate through ChampSim's pipeline but every one of them is marked useless: lbm accuracy dropped from 100% to 72%, and the combined score regressed to 1.281. The page-boundary check is enforcing a real semantic constraint, not a soft optimisation.
- **v55** boosted confidence by +2 whenever ChampSim signalled `useful_prefetch=true`. This overshot CS classifications — IPs reached `CS_CONFIDENCE_THRESHOLD` on transient streaks and then stayed there after the streak ended. The combined score regressed to 1.278. The signal is interesting (it is the only direct positive-feedback channel from the cache to the prefetcher) but the confidence counter is the wrong place to integrate it.

These negative results sharpen the conclusion: v40 saturates the local optimum reachable with IPCP's existing state schema. To go further would require widening the evolutionary scope to the IP-table layout and the issue path's page-aware logic, both of which live outside the file we evolved.

## 5. Goals and Next Steps

The proposal goal was to demonstrate whether LLM-driven prefetcher evolution generalises beyond the single-trace fitness used by the poster project, and to characterise the size and source of any single-trace overfit. We met that goal: the poster-evolved IPCP is 5.9% below v40 on the multi-trace metric, the regression source is identified (the poster's `GS_DEGREE = 3` is a gcc-specific local optimum in a fitness landscape where xalancbmk demands deep streams), and a corrected configuration is committed to the repository as the live `prefetcher/ipcp/ipcp.cc`.

Three directions are the most promising next steps. First, the page-boundary plateau invites widening the evolvable scope to include `prefetcher_cache_operate` and `issue_prefetch`; with that surface in scope, an evolved prefetcher could compute remaining-lines-in-page dynamically, allocate spare degree to backward prefetches, or invoke the signature table from the GS path with its own degree budget. Second, the `useful_prefetch` and `cache_hit` signals are entirely unused by the current IPCP and three of our candidates failed to extract gain from them. A more careful integration — tracking a per-IP rolling accuracy in the IP table rather than perturbing global confidence — could unlock the closed-loop feedback we attempted in v55. Third, cross-prefetcher transfer learning: every candidate we evolved was IPCP-only, but the data we collected (specifically the magnitude-aware GS gate and the deeper degrees) probably transfers to BOP and Berti, both of which currently sit below IPCP v40 on the multi-trace suite. Reproducing this study end-to-end with `openevolve-run` once an API key is available would also confirm that the manual mutation loop did not bias the search.

## 6. Collaboration

This is a single-author project. All implementation work — the multi-trace evaluator, the cross-prefetcher comparison harness, the eight-trace SPEC CPU 2017 download set, the 58 candidate evaluations, the report — was done by the author with the Claude Code coding assistant acting as the LLM mutation agent in lieu of the OpenEvolve-driven `gpt-5-mini` endpoint that the absent API key would have provided.

## 7. Conclusion

We extended an existing LLM-driven prefetcher-evolution study from single-trace to multi-trace fitness and showed that the previously reported gains do not generalise unchanged. On an eight-trace SPEC CPU 2017 subset, the poster's gcc-tuned IPCP scored 1.243 combined; 58 successive candidates lifted this to 1.316 (geomean speedup 1.293 → 1.372), with no per-trace regression and a slight improvement on the poster's own gcc-only metric. The dominant gain came from a single hyper-parameter — `GS_DEGREE` raised from 3 to 16 — that the original gcc-only fitness signal could not see, paired with a small magnitude-aware gate that prevents the more aggressive prefetcher from polluting memory-bound traces. The plateau at v40 is enforced by ChampSim's page-boundary check and is therefore an architectural rather than algorithmic limit; further progress requires widening the evolvable region to the issue path or rethinking IPCP's state schema. Single-trace fitness is a poor proxy for multi-workload performance, but the same evolution machinery is plenty capable of finding the multi-trace optimum once the fitness signal it sees is the right one.
