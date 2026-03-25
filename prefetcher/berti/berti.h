#ifndef BERTI_H
#define BERTI_H

#include <array>
#include <cstdint>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "msl/lru_table.h"

struct berti : public champsim::modules::prefetcher {
  // === BEGIN EVOLVABLE PARAMETERS ===
  static constexpr double TIMELY_THRESHOLD = 0.5;
  static constexpr double LATE_THRESHOLD = 1.5;
  static constexpr double COVERAGE_WEIGHT = 1.0;
  static constexpr double TIMELINESS_WEIGHT = 1.0;
  static constexpr double ACCURACY_WEIGHT = 0.5;
  static constexpr int PREFETCH_DEGREE = 2;
  static constexpr int HISTORY_DEPTH = 16;
  static constexpr int MIN_CONFIDENCE = 3;
  static constexpr int MAX_DELTA = 64;
  static constexpr double MSHR_THRESHOLD = 0.5;
  // === END EVOLVABLE PARAMETERS ===

  uint64_t current_cycle = 0;

  // Per-PC history: stores recent (block_addr, timestamp) pairs
  struct history_entry {
    champsim::block_number block{};
    uint64_t timestamp = 0;
  };

  // Delta statistics: how often a delta was observed, timely, late
  struct delta_stats {
    int count = 0;
    int timely = 0;
    int late = 0;
  };

  // PC tracker entry
  struct pc_entry {
    champsim::address ip{};
    std::array<history_entry, 16> history{};
    int history_head = 0;
    int history_size = 0;
    // Delta stats: indexed by delta + MAX_DELTA (so delta range is [-MAX_DELTA, MAX_DELTA])
    std::array<delta_stats, 129> deltas{}; // 2*64+1

    auto index() const { using namespace champsim::data::data_literals; return ip.slice_upper<2_b>(); }
    auto tag() const { using namespace champsim::data::data_literals; return ip.slice_upper<2_b>(); }
  };

  static constexpr std::size_t PC_TABLE_SETS = 64;
  static constexpr std::size_t PC_TABLE_WAYS = 4;

  champsim::msl::lru_table<pc_entry> pc_table{PC_TABLE_SETS, PC_TABLE_WAYS};

  // Pending prefetches for timeliness tracking
  static constexpr std::size_t PENDING_SIZE = 64;
  struct pending_entry {
    champsim::block_number block{};
    uint64_t issue_cycle = 0;
    bool valid = false;
  };
  std::array<pending_entry, PENDING_SIZE> pending{};

  // === BEGIN EVOLVABLE FUNCTION ===
  static double composite_score(int timely, int late, int total);
  // === END EVOLVABLE FUNCTION ===

  void add_history(pc_entry& entry, champsim::block_number block, uint64_t cycle);
  int best_delta(const pc_entry& entry);
  void record_pending(champsim::block_number block, uint64_t cycle);

public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_cycle_operate();
  void prefetcher_final_stats();
};

#endif
