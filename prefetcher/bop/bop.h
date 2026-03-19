#ifndef BOP_H
#define BOP_H

#include <array>
#include <cstdint>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"

struct bop : public champsim::modules::prefetcher {
  // === BEGIN EVOLVABLE PARAMETERS ===
  static constexpr int SCORE_MAX = 31;
  static constexpr int ROUND_MAX = 100;
  static constexpr int BAD_SCORE = 1;
  static constexpr int PREFETCH_DEGREE = 1;
  static constexpr double MSHR_THRESHOLD = 0.7;
  // === END EVOLVABLE PARAMETERS ===

  static constexpr std::size_t RR_TABLE_SIZE = 256;

  // Candidate offsets (positive only; tested in both directions via RR lookup)
  static const std::array<int, 26> OFFSET_LIST;

  // Recent Request table: direct-mapped, stores block numbers
  std::array<uint64_t, RR_TABLE_SIZE> rr_table{};

  // Per-offset scores for the current round
  std::array<int, 26> scores{};

  int round_counter = 0;
  int best_offset = 1;
  std::size_t test_idx = 0;

  // === BEGIN EVOLVABLE FUNCTION ===
  static int score_update(int old_score);
  // === END EVOLVABLE FUNCTION ===

  void select_best_offset();
  void reset_round();
  std::size_t rr_hash(uint64_t block_val) const;

public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
