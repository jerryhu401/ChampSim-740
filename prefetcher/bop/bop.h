#ifndef BOP_H
#define BOP_H

#include <array>
#include <cstdint>
#include <vector>

#include "address.h"
#include "champsim.h"
#include "modules.h"

struct bop : public champsim::modules::prefetcher {
  static constexpr std::size_t RR_TABLE_SIZE = 256;
  static constexpr std::size_t NUM_OFFSETS = 26;

  std::array<uint64_t, RR_TABLE_SIZE> rr_table{};
  std::array<int, NUM_OFFSETS> scores{};

  int round_counter = 0;
  int best_offset = 1;
  std::size_t test_idx = 0;

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
