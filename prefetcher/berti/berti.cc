#include "berti.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "cache.h"

// EVOLVE-BLOCK-START
namespace {
  // Timeliness classification: ratio of access latency to avg miss latency
  // Make 'timely' slightly stricter so we reward early arrivals, and flag
  // 'late' a bit sooner to avoid pollution from slow prefetches.
  constexpr double TIMELY_THRESHOLD = 0.40;  // below this → timely prefetch
  constexpr double LATE_THRESHOLD   = 1.12;  // above this → late prefetch

  // Weights in composite_score()
  // Reduce raw coverage weight (diminishing returns), increase timeliness
  // importance, and make accuracy (late penalty) stronger for persistent lateness.
  constexpr double COVERAGE_WEIGHT    = 0.70;
  constexpr double TIMELINESS_WEIGHT  = 1.80;
  constexpr double ACCURACY_WEIGHT    = 1.60;

  // How many deltas to prefetch per access
  // Slightly increase degree to capture short multi-step sequences when the signal is strong.
  constexpr int PREFETCH_DEGREE = 4;

  // Per-PC history buffer depth (capped at pc_entry::history array size = 16)
  // Favor somewhat-recent behavior but keep a reasonably-sized window.
  constexpr int HISTORY_DEPTH = 10;

  // Minimum observations of a delta before trusting it
  // Keep low so we can exploit repeating patterns early, but rely on stronger scoring/penalties.
  constexpr int MIN_CONFIDENCE = 2;

  // Maximum absolute delta value to track
  // Narrow the tracked delta range to avoid far-away pollution.
  constexpr int MAX_DELTA = 24;

  // Throttle fill-level prefetches when MSHR is this full
  // Be a bit more conservative under memory pressure.
  constexpr double MSHR_THRESHOLD = 0.55;
} // namespace

double berti::composite_score(int timely, int late, int total)
{
  if (total == 0)
    return 0.0;

  // Ratios of observation categories
  double timely_ratio = static_cast<double>(timely) / total;
  double late_ratio = static_cast<double>(late) / total;
  double neutral_ratio = std::max(0.0, 1.0 - timely_ratio - late_ratio);

  // Coverage: diminishing returns so high-count deltas are valued but not linearly
  double coverage_score = std::log2(static_cast<double>(total) + 1.0) * COVERAGE_WEIGHT;

  // Timeliness: neutrals give partial credit (50%), prioritize early arrivals more aggressively
  double timeliness_score = (timely_ratio + 0.5 * neutral_ratio) * TIMELINESS_WEIGHT;

  // Mild confidence growth with observations
  double confidence = std::sqrt(static_cast<double>(total));

  // Late penalty: punish late ratios nonlinearly (more sensitive when late_ratio is high),
  // and strengthen penalty slightly with confidence so consistent lateness is avoided.
  double late_penalty = std::pow(late_ratio, 1.5) * (1.0 + confidence / 5.0) * ACCURACY_WEIGHT;

  // Consistency bonus: reward deltas that are clearly early more strongly (scaled by confidence)
  double consistency_bonus = 0.0;
  if (timely_ratio > late_ratio + 0.20)
    consistency_bonus = 0.18 * confidence;

  // Regularizer to avoid noisy high-count candidates winning by coverage alone
  double regularizer = 0.03 * confidence;

  // Final composite:
  // - coverage opens the gate,
  // - timeliness adds reward,
  // - late observations subtract strongly (especially if frequent),
  // - consistent early deltas get an extra bump,
  // - a small regularizer keeps noisy winners in check.
  double score = coverage_score + timeliness_score - late_penalty + consistency_bonus - regularizer;
  return score;
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
