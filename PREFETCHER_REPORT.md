# Prefetcher Baseline Implementation Report

## Overview

We implemented 3 advanced L2C prefetchers in ChampSim as baselines for OpenEvolve optimization. Each has clearly marked evolvable parameters and scoring functions.

| Prefetcher | Core Idea | Evolvable Knobs | LOC |
|---|---|---|---|
| **BOP** (Best Offset) | Test candidate offsets against recent accesses; pick the best one | `SCORE_MAX`, `ROUND_MAX`, `BAD_SCORE`, `PREFETCH_DEGREE`, `score_update()` | ~100 |
| **IPCP** (IP Classifier) | Classify each PC as constant-stride / complex / global-stream / next-line; apply class-specific strategy | Per-class degrees, confidence thresholds, `classify_ip()` | ~140 |
| **Berti** (Timing-Aware) | Track per-PC access history with timestamps; rank deltas by timeliness | `TIMELY_THRESHOLD`, `LATE_THRESHOLD`, coverage/timeliness/accuracy weights, `composite_score()` | ~170 |

## Results (10M instructions, 1M warmup)

### IPC Comparison

| Trace | None | BOP | IPCP | Berti | Best Speedup |
|---|---|---|---|---|---|
| perlbench | 2.157 | 2.168 | **2.174** | 2.168 | +0.8% (IPCP) |
| gcc | 0.471 | 0.821 | 0.811 | **0.972** | +106% (Berti) |
| mcf | 0.282 | 0.360 | **0.376** | 0.371 | +33% (IPCP) |
| xalancbmk | 0.532 | 0.767 | 0.733 | **1.025** | +93% (Berti) |

### L2C Prefetch Accuracy

| Trace | Prefetcher | Issued | Useful | Useless | Accuracy |
|---|---|---|---|---|---|
| gcc | BOP | 76,429 | 76,429 | 1,729 | 98% |
| gcc | IPCP | 77,181 | 77,181 | 1,571 | 98% |
| gcc | Berti | 286,439 | 69,389 | 4,123 | 94% |
| mcf | BOP | 266,574 | 87,617 | 132,760 | 40% |
| mcf | IPCP | 499,404 | 102,128 | 150,404 | 40% |
| mcf | Berti | 647,886 | 115,860 | 266,036 | 30% |
| xalancbmk | BOP | 121,503 | 121,503 | 15,660 | 89% |
| xalancbmk | IPCP | 107,398 | 107,398 | 16,108 | 87% |
| xalancbmk | Berti | 153,509 | 153,509 | 14,243 | 91% |

### L2C Load Miss Reduction (vs. no prefetcher)

| Trace | No Prefetch | BOP | IPCP | Berti |
|---|---|---|---|---|
| gcc | 80,706 | 52,965 (−34%) | 52,614 (−35%) | **11,539 (−86%)** |
| mcf | 250,156 | 196,447 (−21%) | **183,568 (−27%)** | 183,568 (−27%) |
| xalancbmk | 185,704 | 107,398 (−42%) | 115,004 (−38%) | **53,017 (−71%)** |

## Key Observations

1. **No single prefetcher wins everywhere.** Berti dominates on structured workloads (gcc, xalancbmk), IPCP wins on pointer-heavy mcf. This per-workload variation is exactly the optimization space for OpenEvolve.

2. **Coverage vs. accuracy tradeoff is visible.** On mcf, all prefetchers have 30-40% accuracy — aggressive prefetching helps IPC but wastes bandwidth. OpenEvolve can tune aggressiveness (degree, confidence thresholds) to find better operating points.

3. **Berti's composite_score() is the richest evolution target.** It weighs coverage, timeliness, and accuracy — small changes to these weights dramatically shift behavior between conservative (high accuracy, low coverage) and aggressive (high coverage, low accuracy).

4. **perlbench is CPU-bound** with only ~1.5K L2C misses in 10M instructions. All prefetchers are within noise. Memory-intensive workloads (gcc, mcf, xalancbmk) are the meaningful benchmarks.

## File Locations

```
prefetcher/bop/bop.h, bop.cc          # Best Offset Prefetcher
prefetcher/ipcp/ipcp.h, ipcp.cc       # IP Classifier Prefetcher
prefetcher/berti/berti.h, berti.cc    # Berti Timing-Aware Prefetcher
champsim_config_bop.json              # Config: BOP at L2C
champsim_config_ipcp.json             # Config: IPCP at L2C
champsim_config_berti.json            # Config: Berti at L2C
run_compare.sh                        # Comparison script
results/                              # Raw simulation outputs
```

## How to Reproduce

```bash
# Build all configs
./config.sh champsim_config_bop.json && make -j$(nproc)
./config.sh champsim_config_ipcp.json && make -j$(nproc)
./config.sh champsim_config_berti.json && make -j$(nproc)

# Run comparison (skips already-completed runs)
./run_compare.sh
```

Traces from: https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/
