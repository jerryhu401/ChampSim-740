# Plan: Implement 3 Advanced Memory Prefetcher Baselines for OpenEvolve

## Context

The goal is to implement advanced prefetchers in ChampSim whose internal policies (scoring functions, thresholds, classification logic) can be iteratively improved by OpenEvolve. Each prefetcher is chosen because it has rich, evolvable decision-making logic with clear tradeoffs across coverage, accuracy, timeliness, and performance.

**Existing prefetchers** in the repo: `no`, `next_line`, `ip_stride`, `va_ampm_lite`, `spp_dev`.

---

## Prefetchers to Implement

### 1. BOP (Best Offset Prefetcher) — ~180 LOC

**Algorithm**: Evaluates a set of candidate offsets in rounds. A Recent Request (RR) table records recent accesses. For each access, test if `addr - offset` is in the RR table (meaning that offset would have been a good prefetch). The offset with the highest score wins and is used until the next round.

**Evolvable parameters**:
- `SCORE_MAX` (31) — early-stop threshold
- `ROUND_MAX` (100) — accesses per evaluation round
- `BAD_SCORE` (1) — minimum acceptable score
- `PREFETCH_DEGREE` (1) — prefetches per access
- `MSHR_THRESHOLD` (0.7) — throttling threshold
- `OFFSET_LIST` — candidate offsets to evaluate
- `score_update()` — how scores are incremented (evolvable function)

**Files**: `prefetcher/bop/bop.h`, `prefetcher/bop/bop.cc`

### 2. IPCP (IP-based Classifier Prefetcher) — ~300 LOC

**Algorithm**: Classifies each instruction pointer into one of four classes based on access history, then applies a class-specific strategy:
- **CS (Constant Stride)**: Same stride repeats → prefetch at that stride
- **CPLX (Complex Stride)**: Varying strides form a signature → signature table predicts next delta
- **GS (Global Stream)**: No local pattern but global directional trend → prefetch in stream direction
- **NL (Next Line)**: Fallback → prefetch next cache line

**Evolvable parameters**:
- Per-class prefetch degrees: `CS_DEGREE` (3), `CPLX_DEGREE` (2), `GS_DEGREE` (4), `NL_DEGREE` (1)
- `CS_CONFIDENCE_THRESHOLD` (3) — confidence to classify as CS
- `CPLX_CONFIDENCE_THRESHOLD` (3) — confidence to classify as CPLX
- `STREAM_DETECT_THRESHOLD` (4) — accesses to detect stream direction
- `CONFIDENCE_SAT_MAX` (7) — saturating counter max
- `classify_ip()` — class transition function (evolvable)

**Files**: `prefetcher/ipcp/ipcp.h`, `prefetcher/ipcp/ipcp.cc`

### 3. Berti Prefetcher — ~340 LOC

**Algorithm**: Tracks per-PC access history with timestamps. For each PC, computes deltas between historical and current accesses and measures timeliness (was the delta's prefetch timely, late, or early?). Selects the delta with the best composite timeliness score.

**Evolvable parameters**:
- `TIMELY_THRESHOLD` (0.5) — latency ratio below which a prefetch is "timely"
- `LATE_THRESHOLD` (1.5) — latency ratio above which a prefetch is "late"
- `COVERAGE_WEIGHT`, `TIMELINESS_WEIGHT`, `ACCURACY_WEIGHT` — weights in composite score
- `PREFETCH_DEGREE` (1) — top-K deltas to prefetch
- `HISTORY_DEPTH` (16) — per-PC history buffer size
- `MIN_CONFIDENCE` (3) — minimum observations before trusting a delta
- `composite_score()` — delta ranking function (evolvable)

**Files**: `prefetcher/berti/berti.h`, `prefetcher/berti/berti.cc`

---

## Implementation Details

### Function Signatures (from existing code)

All prefetchers inherit from `champsim::modules::prefetcher` and use:
```cpp
using champsim::modules::prefetcher::prefetcher;  // inherit constructor
```

Required signatures (note `uint8_t` not `bool` for cache_hit/prefetch):
```cpp
uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
    uint8_t cache_hit, bool useful_prefetch, access_type type, uint32_t metadata_in);
uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
    uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
void prefetcher_cycle_operate();       // BOP: no-op, IPCP: no-op, Berti: cycle counter
void prefetcher_final_stats();         // print evolvable param values + accuracy stats
```

### Evolvable Parameter Convention

Each prefetcher marks evolvable parameters in a block for OpenEvolve to rewrite:
```cpp
// === BEGIN EVOLVABLE PARAMETERS ===
static constexpr int SCORE_MAX = 31;
static constexpr int ROUND_MAX = 100;
// ...
// === END EVOLVABLE PARAMETERS ===

// === BEGIN EVOLVABLE FUNCTION ===
static double composite_score(int timely, int late, int total) { ... }
// === END EVOLVABLE FUNCTION ===
```

### Data Structures

All three use `champsim::msl::lru_table<T>` for tracking tables (sets must be power of 2, entries need `index()` and `tag()` methods). Pattern follows `ip_stride`.

- **BOP**: RR table (direct-mapped `std::array<uint64_t, 256>`), scores vector, single `best_offset`
- **IPCP**: IP table (64 sets x 4 ways), CPLX signature table (64x4), region table (64x2) for stream detection
- **Berti**: PC table (64 sets x 4 ways) with per-entry history buffer and delta stats arrays

### Page Boundary Safety

All prefetchers check before issuing:
```cpp
if (intern_->virtual_prefetch || champsim::page_number{pf_addr} == champsim::page_number{addr})
    prefetch_line(pf_addr, fill_this_level, metadata);
```

---

## Implementation Order

1. **BOP** (simplest, ~180 LOC) — validates build integration
2. **IPCP** (~300 LOC) — multi-class strategy, richest classification logic
3. **Berti** (~340 LOC) — most complex, timing-aware, richest scoring function

---

## Key Files to Reference

| File | Purpose |
|------|---------|
| `prefetcher/ip_stride/ip_stride.h` | Pattern for header structure, lru_table usage |
| `prefetcher/ip_stride/ip_stride.cc` | Pattern for cache_operate/fill/cycle_operate |
| `prefetcher/spp_dev/spp_dev.h` | Pattern for complex multi-table prefetcher |
| `inc/modules.h` | Base class definition |
| `inc/msl/lru_table.h` | Set-associative table utility |
| `inc/address.h` | Address types: `champsim::address`, `block_number`, `page_number` |

---

## Verification

1. **Build**: For each prefetcher, create a test config JSON with `"prefetcher": "<name>"` at L2C, run `./config.sh <config>` and `make`
2. **Smoke test**: Run a short simulation trace and verify no crashes, prefetch stats are non-zero
3. **Correctness**: Check `prefetcher_final_stats()` output — coverage > 0, no obviously broken values
4. **Comparison**: Run all 3 + `ip_stride` + `no` baselines on the same trace, compare IPC and prefetch accuracy
