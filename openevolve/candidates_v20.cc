#include "ipcp.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>

#include "cache.h"

// EVOLVE-BLOCK-START
namespace {
  constexpr int CS_DEGREE   = 8;
  constexpr int CPLX_DEGREE = 3;
  constexpr int GS_DEGREE   = 5;
  constexpr int NL_DEGREE   = 1;

  constexpr int CS_CONFIDENCE_THRESHOLD   = 4;
  constexpr int CPLX_CONFIDENCE_THRESHOLD = 2;
  constexpr int STREAM_DETECT_THRESHOLD   = 3;
  constexpr int CONFIDENCE_SAT_MAX = 8;

  constexpr double MSHR_THRESHOLD = 0.6;

  // Only route to GS if the IP's own stride is small. Large-magnitude strides
  // matched against a global +/- direction are coincidental, not streaming —
  // this is the v2 fix that protects mcf / omnetpp.
  constexpr int GS_STRIDE_LIMIT = 64;
} // namespace

ipcp::ip_class_t ipcp::classify_ip(int64_t old_stride, int64_t new_stride, int confidence, ip_class_t old_class)
{
  if (new_stride == 0)
    return NL;

  if (global_stream_dir != 0) {
    bool dir_match = (global_stream_dir > 0 && new_stride > 0) || (global_stream_dir < 0 && new_stride < 0);
    if (dir_match && std::abs(new_stride) <= GS_STRIDE_LIMIT) {
      if (confidence < CS_CONFIDENCE_THRESHOLD || old_class == GS) {
        return GS;
      }
    }
  }

  if (new_stride == old_stride) {
    if (confidence >= CS_CONFIDENCE_THRESHOLD)
      return CS;
    if (old_class == CS)
      return CS;
    return NL;
  }

  if (old_class == CPLX && confidence >= 1)
    return CPLX;
  if (confidence >= CPLX_CONFIDENCE_THRESHOLD)
    return CPLX;

  return NL;
}
// EVOLVE-BLOCK-END

void ipcp::prefetcher_initialize()
{
  global_pos_count = 0;
  global_neg_count = 0;
  global_stream_dir = 0;
}

void ipcp::issue_prefetch(champsim::address addr, champsim::block_number block, int64_t delta, int degree, uint32_t metadata)
{
  for (int d = 1; d <= degree; d++) {
    champsim::address pf_addr{champsim::block_number{block + delta * d}};
    if (intern_->virtual_prefetch || champsim::page_number{pf_addr} == champsim::page_number{addr}) {
      bool fill_this = intern_->get_mshr_occupancy_ratio() < MSHR_THRESHOLD;
      prefetch_line(pf_addr, fill_this, metadata);
    }
  }
}

uint32_t ipcp::prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                        uint32_t metadata_in)
{
  champsim::block_number block{addr};
  auto found = ip_table.check_hit({ip, block, 0, 0, NONE, 0});

  int64_t new_stride = 0;
  int confidence = 0;
  ip_class_t ip_class = NL;
  uint16_t sig = 0;

  if (found.has_value()) {
    new_stride = champsim::offset(found->last_block, block);
    int64_t old_stride = found->last_stride;
    confidence = found->confidence;
    sig = found->signature;

    if (new_stride == old_stride && new_stride != 0) {
      confidence = std::min(confidence + 1, CONFIDENCE_SAT_MAX);
    } else if (new_stride != old_stride) {
      confidence = std::max(confidence - 1, 0);
    }

    ip_class = classify_ip(old_stride, new_stride, confidence, found->ip_class);

    if (new_stride != 0) {
      uint16_t delta_bits = static_cast<uint16_t>(std::abs(new_stride)) & 0xF;
      if (new_stride < 0)
        delta_bits |= 0x10;
      sig = ((sig << 3) ^ delta_bits) & 0xFFF;
    }
  }

  if (new_stride > 0) global_pos_count++;
  else if (new_stride < 0) global_neg_count++;
  if (global_pos_count - global_neg_count > STREAM_DETECT_THRESHOLD) global_stream_dir = 1;
  else if (global_neg_count - global_pos_count > STREAM_DETECT_THRESHOLD) global_stream_dir = -1;
  if (global_pos_count + global_neg_count > 1024) {
    global_pos_count >>= 1;
    global_neg_count >>= 1;
  }

  ip_table.fill({ip, block, new_stride, confidence, ip_class, sig});

  switch (ip_class) {
  case CS:
    if (new_stride != 0)
      issue_prefetch(addr, block, new_stride, CS_DEGREE, metadata_in);
    break;
  case CPLX: {
    if (found.has_value() && new_stride != 0) {
      cplx_table.fill({found->signature, new_stride, confidence});
    }
    auto pred = cplx_table.check_hit({sig, 0, 0});
    if (pred.has_value() && pred->delta != 0) {
      issue_prefetch(addr, block, pred->delta, CPLX_DEGREE, metadata_in);
    }
    break;
  }
  case GS:
    if (global_stream_dir != 0)
      issue_prefetch(addr, block, global_stream_dir, GS_DEGREE, metadata_in);
    break;
  case NL:
    issue_prefetch(addr, block, 1, NL_DEGREE, metadata_in);
    break;
  default:
    break;
  }

  return metadata_in;
}

uint32_t ipcp::prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in)
{
  return metadata_in;
}

void ipcp::prefetcher_final_stats()
{
  std::cout << "IPCP global_stream_dir: " << global_stream_dir << std::endl;
  std::cout << "IPCP evolvable params: CS_DEGREE=" << CS_DEGREE << " CPLX_DEGREE=" << CPLX_DEGREE << " GS_DEGREE=" << GS_DEGREE
            << " NL_DEGREE=" << NL_DEGREE << " CS_CONF_THR=" << CS_CONFIDENCE_THRESHOLD << " CPLX_CONF_THR=" << CPLX_CONFIDENCE_THRESHOLD
            << " STREAM_THR=" << STREAM_DETECT_THRESHOLD << std::endl;
}
