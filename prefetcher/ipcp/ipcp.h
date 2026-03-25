#ifndef IPCP_H
#define IPCP_H

#include <array>
#include <cstdint>

#include "address.h"
#include "champsim.h"
#include "modules.h"
#include "msl/lru_table.h"

struct ipcp : public champsim::modules::prefetcher {
  // === BEGIN EVOLVABLE PARAMETERS ===
  static constexpr int CS_DEGREE = 3;
  static constexpr int CPLX_DEGREE = 2;
  static constexpr int GS_DEGREE = 4;
  static constexpr int NL_DEGREE = 1;
  static constexpr int CS_CONFIDENCE_THRESHOLD = 3;
  static constexpr int CPLX_CONFIDENCE_THRESHOLD = 3;
  static constexpr int STREAM_DETECT_THRESHOLD = 4;
  static constexpr int CONFIDENCE_SAT_MAX = 7;
  static constexpr double MSHR_THRESHOLD = 0.5;
  // === END EVOLVABLE PARAMETERS ===

  enum ip_class_t : uint8_t { NONE = 0, CS = 1, CPLX = 2, GS = 3, NL = 4 };

  // IP tracker table
  struct ip_entry {
    champsim::address ip{};
    champsim::block_number last_block{};
    int64_t last_stride = 0;
    int confidence = 0;
    ip_class_t ip_class = NONE;
    // For CPLX: signature built from recent strides
    uint16_t signature = 0;

    auto index() const { using namespace champsim::data::data_literals; return ip.slice_upper<2_b>(); }
    auto tag() const { using namespace champsim::data::data_literals; return ip.slice_upper<2_b>(); }
  };

  // CPLX signature table: maps signature -> predicted delta
  struct cplx_entry {
    uint16_t signature = 0;
    int64_t delta = 0;
    int confidence = 0;

    auto index() const { return static_cast<std::size_t>(signature & 0x3F); }
    auto tag() const { return signature; }
  };

  // Global stream detector
  int global_pos_count = 0;
  int global_neg_count = 0;
  int global_stream_dir = 0; // +1 or -1, 0 = unknown

  static constexpr std::size_t IP_TABLE_SETS = 64;
  static constexpr std::size_t IP_TABLE_WAYS = 4;
  static constexpr std::size_t CPLX_TABLE_SETS = 64;
  static constexpr std::size_t CPLX_TABLE_WAYS = 4;

  champsim::msl::lru_table<ip_entry> ip_table{IP_TABLE_SETS, IP_TABLE_WAYS};
  champsim::msl::lru_table<cplx_entry> cplx_table{CPLX_TABLE_SETS, CPLX_TABLE_WAYS};

  // === BEGIN EVOLVABLE FUNCTION ===
  static ip_class_t classify_ip(int64_t old_stride, int64_t new_stride, int confidence, ip_class_t old_class);
  // === END EVOLVABLE FUNCTION ===

  void issue_prefetch(champsim::address addr, champsim::block_number block, int64_t delta, int degree, uint32_t metadata);

public:
  using champsim::modules::prefetcher::prefetcher;

  void prefetcher_initialize();
  uint32_t prefetcher_cache_operate(champsim::address addr, champsim::address ip, uint8_t cache_hit, bool useful_prefetch, access_type type,
                                    uint32_t metadata_in);
  uint32_t prefetcher_cache_fill(champsim::address addr, long set, long way, uint8_t prefetch, champsim::address evicted_addr, uint32_t metadata_in);
  void prefetcher_final_stats();
};

#endif
