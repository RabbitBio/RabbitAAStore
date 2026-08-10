#ifndef AASTORE_BYTE_RANGE_PLANNER_HPP
#define AASTORE_BYTE_RANGE_PLANNER_HPP

#include "aastore/store.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aastore::detail {

struct SequenceRequest {
    uint64_t sequence_id;
    // Original request index during preparation; direct arena byte offset
    // after the physical output layout is built.
    size_t output_position;
    uint64_t start;
    uint64_t length;
};

using SequenceRequests =
    std::vector<SequenceRequest, NoInitAllocator<SequenceRequest>>;

struct ByteRange {
    uint64_t begin = 0;
    uint64_t end = 0;
    size_t request_begin = 0;
    size_t request_end = 0;
};

struct ReadPlan {
    std::vector<ByteRange> ranges;
    uint64_t requested_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t read_bytes = 0;
    uint64_t merged_gaps = 0;
    uint64_t selected_merge_gap_bytes = 0;
    uint64_t max_range_bytes = 0;
    uint64_t estimated_io_ns = 0;
    uint64_t merge_candidates = 0;
};

struct ByteRangePlanOptions {
    bool adaptive_merge_gap = false;
    uint64_t merge_gap_bytes = kByteRangeMergeGapBytes;
    uint64_t io_latency_ns = kAdaptiveIoLatencyNs;
    uint64_t io_bandwidth_bytes_per_second =
        kAdaptiveIoBandwidthBytesPerSecond;
    uint64_t max_merged_range_bytes = kAdaptiveMaxRangeBytes;
    size_t io_threads = 1;
};

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count);

ReadPlan plan_byte_ranges(const SequenceRequests& sorted_requests,
                          uint64_t page_residues,
                          uint64_t packed_page_bytes,
                          const ByteRangePlanOptions& options = {});

}  // namespace aastore::detail

#endif
