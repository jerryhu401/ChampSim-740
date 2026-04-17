#include "bop.h"

#include <algorithm>
#include <iostream>

#include "cache.h"

 // EVOLVE-BLOCK-START
namespace {
  // Aim: improve IPC while reducing pollution.
  // - Slightly larger SCORE_MAX to allow a truly good offset to accumulate evidence.
  // - Longer ROUND_MAX to give more time for candidates to show recurring behavior.
  // - Increase BAD_SCORE to avoid switching on weak signals.
  // - PREFETCH_DEGREE reduced to 1 to lower useless prefetches and cache pollution.
  // - Keep MSHR threshold moderate to avoid overloading memory system.
  constexpr int SCORE_MAX = 10;
  constexpr int ROUND_MAX = 48;
  constexpr int BAD_SCORE = 3;
  constexpr int PREFETCH_DEGREE = 1;
  constexpr double MSHR_THRESHOLD = 0.60;

  // Compact, prioritized offsets: emphasize the smallest strides and common power-of-two jumps.
  // Smaller candidate set converges faster and generates fewer exploratory prefetches.
  constexpr std::array<int, 7> OFFSET_LIST = {
      1, 2, 3, 4, 8, 16, 32};

  // Conservative-then-accelerate score growth:
  // - First confirmation provides a minimal boost to avoid elevating spurious single hits.
  // - While an offset has only a few confirmations, grow slowly (require repeated evidence).
  // - Once an offset shows sustained confirmations, accelerate growth modestly to converge.
  // - Always saturate at SCORE_MAX.
  int score_update(int old_score)
  {
    if (old_score == 0) {
      return 1;                       // minimal initial promotion
    } else if (old_score < 3) {
      // slow, steady growth for early confirmations
      int ns = old_score + 1;
      return (ns > SCORE_MAX) ? SCORE_MAX : ns;
    } else {
      // accelerate for sustained confirmations but keep modest increment to avoid runaway
      int increment = 1 + (old_score >> 2); // 1 + floor(old_score/4)
      int ns = old_score + increment;
      return (ns > SCORE_MAX) ? SCORE_MAX : ns;
    }
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
