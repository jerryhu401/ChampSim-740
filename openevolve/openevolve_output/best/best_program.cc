#include "bop.h"

#include <algorithm>
#include <iostream>

#include "cache.h"

 // EVOLVE-BLOCK-START
namespace {
constexpr int SCORE_MAX = 8;
constexpr int ROUND_MAX = 40;
constexpr int BAD_SCORE = 2;
constexpr int PREFETCH_DEGREE = 2;
constexpr double MSHR_THRESHOLD = 0.60;

// Focused candidate offsets (emphasize small strides and common powers while reducing test set
// so the prefetcher converges faster and issues fewer low-quality prefetches).
constexpr std::array<int, 9> OFFSET_LIST = {
    1, 2, 3, 4, 6, 8, 12, 16, 32};

// Adaptive, saturating score growth: faster promotion for repeatedly confirmed offsets
// (increment = 1 + floor(old_score/2)), but with a lower SCORE_MAX to pick a winner quickly.
int score_update(int old_score)
{
  int increment = 1 + (old_score >> 1); // 1 + floor(old_score/2)
  int ns = old_score + increment;
  return (ns > SCORE_MAX) ? SCORE_MAX : ns;
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
