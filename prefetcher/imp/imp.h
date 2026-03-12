#ifndef PREFETCHER_IMP_H
#define PREFETCHER_IMP_H

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

#include "address.h"
#include "champsim.h"
#include "modules.h"

// ====================================================================
// IMP-inspired Indirect Memory Prefetcher for ChampSim
//
// The original IMP (Yu et al., MICRO 2015) reads data values from the
// L1 cache to detect A[B[i]] patterns.  ChampSim's prefetcher API
// does not expose data values, only addresses and PCs.
//
// This adaptation captures the same class of workloads using a
// PC-keyed stream + delta-correlation approach:
//
//   1. STREAM TABLE:  Detect streaming (unit/constant-stride) accesses
//      keyed by instruction PC -- identical to IMP's stream table.
//
//   2. MISS CORRELATION TABLE:  For each streaming PC, record the
//      sequence of L2C miss addresses that occur between consecutive
//      stream accesses.  Learn the *delta* pattern of these correlated
//      misses.  When the stream advances, replay the learned deltas
//      ahead of the current position to issue indirect prefetches.
//
// This is effective when the same PC repeatedly iterates over an index
// array and triggers the same pattern of indirect misses (the common
// case for CSR-based graph/SpMV workloads in nested loops).
// ====================================================================

struct imp : public champsim::modules::prefetcher {
  using prefetcher::prefetcher;

  // ----------------------------------------------------------------
  // Parameters
  // ----------------------------------------------------------------
  static constexpr std::size_t STREAM_TABLE_SIZE = 16;
  static constexpr int STREAM_CONFIRM_THRESHOLD = 3;
  static constexpr int MAX_CORRELATED_MISSES = 4;
  static constexpr int PREFETCH_DEGREE = 4;
  static constexpr std::size_t DELTA_HISTORY_SIZE = 32;

  // ----------------------------------------------------------------
  // Stream Table entry: detect constant-stride access patterns per PC
  // ----------------------------------------------------------------
  struct stream_entry {
    bool valid = false;
    uint64_t pc = 0;
    uint64_t last_block = 0;
    int64_t stride = 0;
    int hit_count = 0;
    int lru = 0;
    bool confirmed = false;
  };

  // ----------------------------------------------------------------
  // Delta History entry: records miss-address deltas that follow
  // a confirmed streaming PC.
  // ----------------------------------------------------------------
  struct delta_history_entry {
    bool valid = false;
    uint64_t pc = 0;
    uint64_t last_miss_block = 0;
    bool has_last_miss = false;

    struct delta_record {
      int64_t delta = 0;
      int count = 0;
    };
    std::array<delta_record, DELTA_HISTORY_SIZE> deltas = {};
    int num_deltas = 0;

    int64_t predicted_stride = 0;
    int predicted_confidence = 0;
  };

  // ----------------------------------------------------------------
  // Active lookahead: pending prefetch sequence
  // ----------------------------------------------------------------
  struct lookahead_entry {
    uint64_t base_block = 0;
    int64_t stride = 0;
    int remaining = 0;
  };

  std::array<stream_entry, STREAM_TABLE_SIZE> stream_table = {};
  std::array<delta_history_entry, STREAM_TABLE_SIZE> delta_table = {};
  std::optional<lookahead_entry> active_lookahead;

  uint64_t current_stream_pc = 0;

  // ----------------------------------------------------------------
  // Stats
  // ----------------------------------------------------------------
  uint64_t stat_streams_detected = 0;
  uint64_t stat_indirect_prefetches = 0;
  uint64_t stat_stream_prefetches = 0;
  uint64_t stat_delta_patterns_learned = 0;

  // ----------------------------------------------------------------
  // Helpers
  // ----------------------------------------------------------------
  int find_stream(uint64_t pc) const;
  int alloc_stream(uint64_t pc);
  void update_lru(int idx);
  void record_correlated_miss(uint64_t pc, uint64_t miss_block);
  void try_indirect_prefetch(int stream_idx, uint64_t current_block);

  // ----------------------------------------------------------------
  // ChampSim API
  // ----------------------------------------------------------------
  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                    uint8_t cache_hit, bool useful_prefetch,
                                    access_type type, uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way,
                                 uint8_t prefetch, champsim::address evicted_addr,
                                 uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif
