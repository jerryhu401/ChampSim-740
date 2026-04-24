#include "berti.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "cache.h"

// EVOLVE-BLOCK-START
namespace {
  // Timeliness classification: ratio of access latency to avg miss latency
  constexpr double TIMELY_THRESHOLD = 0.5;  // below this → timely prefetch
  constexpr double LATE_THRESHOLD   = 1.5;  // above this → late prefetch

  // Weights in composite_score()
  constexpr double COVERAGE_WEIGHT    = 1.0;
  constexpr double TIMELINESS_WEIGHT  = 1.0;
  constexpr double ACCURACY_WEIGHT    = 0.5;

  // How many deltas to prefetch per access
  constexpr int PREFETCH_DEGREE = 2;

  // Per-PC history buffer depth (capped at pc_entry::history array size = 16)
  constexpr int HISTORY_DEPTH = 16;

  // Minimum observations of a delta before trusting it
  constexpr int MIN_CONFIDENCE = 3;

  // Maximum absolute delta value to track
  constexpr int MAX_DELTA = 64;

  // Throttle fill-level prefetches when MSHR is this full
  constexpr double MSHR_THRESHOLD = 0.5;
} // namespace

double berti::composite_score(int timely, int late, int total)
{
  if (total == 0)
    return 0.0;
  double timeliness_ratio = static_cast<double>(timely) / total;
  double lateness_penalty = static_cast<double>(late) / total;
  double coverage = static_cast<double>(total);
  return COVERAGE_WEIGHT * std::log2(coverage + 1) + TIMELINESS_WEIGHT * timeliness_ratio - ACCURACY_WEIGHT * lateness_penalty;
}
// EVOLVE-BLOCK-END

void berti::add_history(pc_entry& entry, champsim::block_number block, uint64_t cycle)
{
  int depth = std::min(HISTORY_DEPTH, 16);
  entry.history[static_cast<std::size_t>(entry.history_head)] = {block, cycle};
  entry.history_head = (entry.history_head + 1) % depth;
  if (entry.history_size < depth)
    entry.history_size++;
}

int berti::best_delta(const pc_entry& entry)
{
  double best_score = -1.0;
  int best_d = 0;

  for (int d = -MAX_DELTA; d <= MAX_DELTA; d++) {
    if (d == 0)
      continue;
    auto idx = static_cast<std::size_t>(d + MAX_DELTA);
    const auto& ds = entry.deltas[idx];
    if (ds.count < MIN_CONFIDENCE)
      continue;
    double score = composite_score(ds.timely, ds.late, ds.count);
    if (score > best_score) {
      best_score = score;
      best_d = d;
    }
  }
  return best_d;
}

void berti::record_pending(champsim::block_number block, uint64_t cycle)
{
  std::size_t oldest_idx = 0;
  uint64_t oldest_cycle = UINT64_MAX;
  for (std::size_t i = 0; i < PENDING_SIZE; i++) {
    if (!pending[i].valid) {
      pending[i] = {block, cycle, true};
      return;
    }
    if (pending[i].issue_cycle < oldest_cycle) {
      oldest_cycle = pending[i].issue_cycle;
      oldest_idx = i;
    }
  }
  pending[oldest_idx] = {block, cycle, true};
}

void berti::prefetcher_initialize()
{
  current_cycle = 0;
  pending.fill({});
}

uint32_t berti::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                         uint32_t metadata_in)
{
  champsim::block_number block{addr};

  auto found = pc_table.check_hit({ip});

  if (found.has_value()) {
    int depth = std::min(HISTORY_DEPTH, 16);
    for (int i = 0; i < found->history_size; i++) {
      auto& hist = found->history[static_cast<std::size_t>(i)];
      if (hist.timestamp == 0)
        continue;
      int64_t delta = champsim::offset(hist.block, block);
      if (delta == 0 || std::abs(delta) > MAX_DELTA)
        continue;

      auto idx = static_cast<std::size_t>(delta + MAX_DELTA);
      found->deltas[idx].count++;

      uint64_t latency = current_cycle - hist.timestamp;
      double avg_miss_latency = 200.0;
      double ratio = static_cast<double>(latency) / avg_miss_latency;
      if (ratio < TIMELY_THRESHOLD) {
        found->deltas[idx].timely++;
      } else if (ratio > LATE_THRESHOLD) {
        found->deltas[idx].late++;
      }
    }

    add_history(*found, block, current_cycle);

    int bd = best_delta(*found);

    pc_table.fill(*found);

    if (bd != 0) {
      for (int d = 1; d <= PREFETCH_DEGREE; d++) {
        champsim::address pf_addr{champsim::block_number{block + static_cast<int64_t>(bd) * d}};
        if (intern_->virtual_prefetch || champsim::page_number{pf_addr} == champsim::page_number{addr}) {
          bool fill_this = intern_->get_mshr_occupancy_ratio() < MSHR_THRESHOLD;
          bool success = prefetch_line(pf_addr, fill_this, metadata_in);
          if (success) {
            record_pending(champsim::block_number{pf_addr}, current_cycle);
          }
        }
      }
    }
  } else {
    pc_entry new_entry{};
    new_entry.ip = ip;
    add_history(new_entry, block, current_cycle);
    pc_table.fill(new_entry);
  }

  return metadata_in;
}

uint32_t berti::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  if (prefetch) {
    champsim::block_number block{addr};
    for (auto& p : pending) {
      if (p.valid && p.block == block) {
        p.valid = false;
        break;
      }
    }
  }
  return metadata_in;
}

void berti::prefetcher_cycle_operate()
{
  current_cycle++;
}

void berti::prefetcher_final_stats()
{
  std::cout << "Berti evolvable params: TIMELY_THR=" << TIMELY_THRESHOLD << " LATE_THR=" << LATE_THRESHOLD << " COV_W=" << COVERAGE_WEIGHT
            << " TIME_W=" << TIMELINESS_WEIGHT << " ACC_W=" << ACCURACY_WEIGHT << " PF_DEGREE=" << PREFETCH_DEGREE << " HIST_DEPTH=" << HISTORY_DEPTH
            << " MIN_CONF=" << MIN_CONFIDENCE << std::endl;
}
