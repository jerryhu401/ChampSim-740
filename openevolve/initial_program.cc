#include "bop.h"

#include <algorithm>
#include <iostream>

#include "cache.h"

// EVOLVE-BLOCK-START
namespace {
constexpr int SCORE_MAX = 31;
constexpr int ROUND_MAX = 100;
constexpr int BAD_SCORE = 1;
constexpr int PREFETCH_DEGREE = 1;
constexpr double MSHR_THRESHOLD = 0.7;

constexpr std::array<int, 26> OFFSET_LIST = {
    1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 15, 16, 18, 20, 24, 25, 27, 30, 32, 36, 40, 45, 48, 50, 54, 60};

int score_update(int old_score)
{
  return old_score + 1;
}
} // namespace
// EVOLVE-BLOCK-END

std::size_t bop::rr_hash(uint64_t block_val) const
{
  return block_val % RR_TABLE_SIZE;
}

void bop::select_best_offset()
{
  int max_score = BAD_SCORE;
  int winner = 0;
  for (std::size_t i = 0; i < OFFSET_LIST.size(); i++) {
    if (scores[i] > max_score) {
      max_score = scores[i];
      winner = OFFSET_LIST[i];
    }
  }
  best_offset = winner;
}

void bop::reset_round()
{
  scores.fill(0);
  round_counter = 0;
  test_idx = 0;
}

void bop::prefetcher_initialize()
{
  rr_table.fill(0);
  scores.fill(0);
  round_counter = 0;
  best_offset = 1;
  test_idx = 0;
}

uint32_t bop::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                       uint32_t metadata_in)
{
  champsim::block_number block{addr};
  uint64_t block_val = block.to<uint64_t>();

  int test_offset = OFFSET_LIST[test_idx];
  uint64_t test_val = block_val - static_cast<uint64_t>(test_offset);
  std::size_t h = rr_hash(test_val);
  if (rr_table[h] == test_val) {
    scores[test_idx] = score_update(scores[test_idx]);
    if (scores[test_idx] >= SCORE_MAX) {
      select_best_offset();
      reset_round();
    }
  }

  test_idx = (test_idx + 1) % OFFSET_LIST.size();
  round_counter++;

  if (round_counter >= ROUND_MAX) {
    select_best_offset();
    reset_round();
  }

  rr_table[rr_hash(block_val)] = block_val;

  if (best_offset != 0) {
    for (int d = 1; d <= PREFETCH_DEGREE; d++) {
      champsim::address pf_addr{champsim::block_number{block + best_offset * d}};
      if (intern_->virtual_prefetch || champsim::page_number{pf_addr} == champsim::page_number{addr}) {
        bool fill_this = intern_->get_mshr_occupancy_ratio() < MSHR_THRESHOLD;
        prefetch_line(pf_addr, fill_this, metadata_in);
      }
    }
  }

  return metadata_in;
}

uint32_t bop::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  if (prefetch) {
    champsim::block_number block{addr};
    uint64_t block_val = block.to<uint64_t>();
    rr_table[rr_hash(block_val)] = block_val;
  }
  return metadata_in;
}

void bop::prefetcher_final_stats()
{
  std::cout << "BOP best_offset: " << best_offset << std::endl;
  std::cout << "BOP evolvable params: SCORE_MAX=" << SCORE_MAX << " ROUND_MAX=" << ROUND_MAX << " BAD_SCORE=" << BAD_SCORE
            << " PREFETCH_DEGREE=" << PREFETCH_DEGREE << " MSHR_THRESHOLD=" << MSHR_THRESHOLD << std::endl;
}
