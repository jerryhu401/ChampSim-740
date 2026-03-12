#include "imp.h"

#include <algorithm>
#include <cstdio>

#include "cache.h"

// ====================================================================
// Stream table helpers
// ====================================================================

int imp::find_stream(uint64_t pc) const
{
  for (std::size_t i = 0; i < STREAM_TABLE_SIZE; ++i) {
    if (stream_table[i].valid && stream_table[i].pc == pc)
      return static_cast<int>(i);
  }
  return -1;
}

int imp::alloc_stream(uint64_t pc)
{
  for (std::size_t i = 0; i < STREAM_TABLE_SIZE; ++i) {
    if (!stream_table[i].valid)
      return static_cast<int>(i);
  }
  int victim = 0;
  int max_lru = -1;
  for (std::size_t i = 0; i < STREAM_TABLE_SIZE; ++i) {
    if (stream_table[i].lru > max_lru) {
      max_lru = stream_table[i].lru;
      victim = static_cast<int>(i);
    }
  }
  stream_table[victim] = {};
  delta_table[victim] = {};
  return victim;
}

void imp::update_lru(int idx)
{
  int old = stream_table[idx].lru;
  for (std::size_t i = 0; i < STREAM_TABLE_SIZE; ++i) {
    if (stream_table[i].valid && stream_table[i].lru < old)
      stream_table[i].lru++;
  }
  stream_table[idx].lru = 0;
}

// ====================================================================
// Delta correlation: learn miss-address stride patterns that follow
// streaming accesses from a given PC.
// ====================================================================

void imp::record_correlated_miss(uint64_t pc, uint64_t miss_block)
{
  int idx = find_stream(pc);
  if (idx < 0)
    return;

  auto& dh = delta_table[idx];
  if (!dh.valid) {
    dh.valid = true;
    dh.pc = pc;
    dh.has_last_miss = false;
    dh.num_deltas = 0;
    dh.predicted_stride = 0;
    dh.predicted_confidence = 0;
  }

  if (!dh.has_last_miss) {
    dh.last_miss_block = miss_block;
    dh.has_last_miss = true;
    return;
  }

  int64_t delta = static_cast<int64_t>(miss_block) - static_cast<int64_t>(dh.last_miss_block);
  dh.last_miss_block = miss_block;

  if (delta == 0)
    return;

  // Record this delta and count occurrences
  bool found = false;
  for (int i = 0; i < dh.num_deltas; ++i) {
    if (dh.deltas[i].delta == delta) {
      dh.deltas[i].count++;
      found = true;

      // Promote as predicted stride if seen enough times
      if (dh.deltas[i].count >= 2 && dh.deltas[i].count > dh.predicted_confidence) {
        dh.predicted_stride = delta;
        dh.predicted_confidence = dh.deltas[i].count;
        stat_delta_patterns_learned++;
      }
      break;
    }
  }

  if (!found && dh.num_deltas < static_cast<int>(DELTA_HISTORY_SIZE)) {
    dh.deltas[dh.num_deltas] = {delta, 1};
    dh.num_deltas++;
  }
}

// ====================================================================
// Issue indirect prefetches using learned delta correlation
// ====================================================================

void imp::try_indirect_prefetch(int stream_idx, uint64_t current_block)
{
  auto& dh = delta_table[stream_idx];
  if (!dh.valid || dh.predicted_confidence < 2)
    return;

  if (!dh.has_last_miss)
    return;

  // Prefetch ahead using the learned miss-delta stride
  uint64_t pf_block = dh.last_miss_block;
  for (int d = 1; d <= PREFETCH_DEGREE; ++d) {
    pf_block = static_cast<uint64_t>(static_cast<int64_t>(pf_block) + dh.predicted_stride);
    uint64_t pf_addr = pf_block << LOG2_BLOCK_SIZE;

    champsim::address addr{pf_addr};
    const bool mshr_ok = intern_->get_mshr_occupancy_ratio() < 0.7;
    if (prefetch_line(addr, mshr_ok, 0))
      stat_indirect_prefetches++;
  }
}

// ====================================================================
// ChampSim API
// ====================================================================

void imp::prefetcher_initialize()
{
  for (auto& s : stream_table)
    s = {};
  for (auto& d : delta_table)
    d = {};
  active_lookahead.reset();
  stat_streams_detected = 0;
  stat_indirect_prefetches = 0;
  stat_stream_prefetches = 0;
  stat_delta_patterns_learned = 0;
}

uint32_t imp::prefetcher_cache_operate(champsim::address addr, champsim::address ip,
                                       uint8_t cache_hit, bool useful_prefetch,
                                       access_type type, uint32_t metadata_in)
{
  uint64_t raw_addr = addr.to<uint64_t>();
  uint64_t raw_ip = ip.to<uint64_t>();
  uint64_t block = raw_addr >> LOG2_BLOCK_SIZE;

  int idx = find_stream(raw_ip);

  if (idx >= 0) {
    auto& st = stream_table[idx];
    int64_t new_stride = static_cast<int64_t>(block) - static_cast<int64_t>(st.last_block);

    if (new_stride != 0) {
      if (new_stride == st.stride) {
        st.hit_count++;
        if (st.hit_count >= STREAM_CONFIRM_THRESHOLD && !st.confirmed) {
          st.confirmed = true;
          stat_streams_detected++;
        }
      } else {
        st.stride = new_stride;
        st.hit_count = 1;
      }
    }

    st.last_block = block;
    update_lru(idx);
    current_stream_pc = raw_ip;

    // If this PC has a confirmed stream, issue stream prefetches
    if (st.confirmed) {
      for (int d = 1; d <= 2; ++d) {
        uint64_t pf_block = static_cast<uint64_t>(static_cast<int64_t>(block) + d * st.stride);
        uint64_t pf_addr = pf_block << LOG2_BLOCK_SIZE;
        champsim::address pf{pf_addr};

        if (intern_->virtual_prefetch ||
            champsim::page_number{pf} == champsim::page_number{addr}) {
          const bool mshr_ok = intern_->get_mshr_occupancy_ratio() < 0.5;
          if (prefetch_line(pf, mshr_ok, 0))
            stat_stream_prefetches++;
        }
      }

      // Also try indirect prefetches from delta correlation
      try_indirect_prefetch(idx, block);
    }
  } else {
    idx = alloc_stream(raw_ip);
    auto& st = stream_table[idx];
    st.valid = true;
    st.pc = raw_ip;
    st.last_block = block;
    st.stride = 0;
    st.hit_count = 0;
    st.confirmed = false;
    st.lru = 0;
    update_lru(idx);
  }

  // On a cache miss, try to correlate with the most recently active stream
  if (!cache_hit && current_stream_pc != 0) {
    record_correlated_miss(current_stream_pc, block);
  }

  return metadata_in;
}

uint32_t imp::prefetcher_cache_fill(champsim::address addr, long set, long way,
                                    uint8_t prefetch, champsim::address evicted_addr,
                                    uint32_t metadata_in)
{
  return metadata_in;
}

void imp::prefetcher_cycle_operate()
{
  // Lookahead-based prefetch issuing (if active)
  if (!active_lookahead.has_value())
    return;

  auto& la = active_lookahead.value();
  uint64_t pf_block = static_cast<uint64_t>(static_cast<int64_t>(la.base_block) + la.stride);
  uint64_t pf_addr = pf_block << LOG2_BLOCK_SIZE;
  champsim::address pf{pf_addr};

  const bool mshr_ok = intern_->get_mshr_occupancy_ratio() < 0.5;
  bool success = prefetch_line(pf, mshr_ok, 0);
  if (success) {
    la.base_block = pf_block;
    la.remaining--;
    if (la.remaining <= 0)
      active_lookahead.reset();
  }
}

void imp::prefetcher_final_stats()
{
  std::printf("[IMP] Streams detected:         %lu\n", stat_streams_detected);
  std::printf("[IMP] Delta patterns learned:   %lu\n", stat_delta_patterns_learned);
  std::printf("[IMP] Stream prefetches issued: %lu\n", stat_stream_prefetches);
  std::printf("[IMP] Indirect prefetches:      %lu\n", stat_indirect_prefetches);
}
