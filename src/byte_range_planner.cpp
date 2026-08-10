#include "byte_range_planner.hpp"

#include "aastore/store.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <thread>

namespace aastore::detail {
namespace {

constexpr std::array<uint64_t, 14> kAdaptiveMergeGaps = {
    0,
    256,
    512,
    1024,
    2048,
    3072,
    4096,
    8192,
    16384,
    32768,
    65536,
    262144,
    1048576,
    std::numeric_limits<uint64_t>::max(),
};
constexpr uint64_t kAdaptivePivotGap = 3072;
constexpr uint64_t kGapSampleStride = 16;

uint64_t packed_bytes_for_residues(uint64_t residues) {
    return (residues * 5 + 7) / 8;
}

struct PackedByteExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
};

struct GapProfile {
    std::array<uint64_t, kAdaptiveMergeGaps.size()> counts{};
    std::array<uint64_t, kAdaptiveMergeGaps.size()> bytes{};
    uint64_t exact_ranges = 0;
    uint64_t total_gaps = 0;
    uint64_t total_gap_bytes = 0;
    uint64_t sampled_gaps = 0;
};

uint64_t scale_sample(uint64_t value,
                      uint64_t population,
                      uint64_t samples,
                      uint64_t upper_bound) {
    if (samples == 0 || value == 0) return 0;
    const long double estimate =
        static_cast<long double>(value) * population / samples;
    if (estimate >= static_cast<long double>(upper_bound)) {
        return upper_bound;
    }
    return static_cast<uint64_t>(estimate + 0.5L);
}

PackedByteExtent packed_byte_extent(const SequenceRequest& request,
                                    uint64_t page_residues,
                                    uint64_t packed_page_bytes) {
    if (request.length == 0) return {};
    const uint64_t first_page = request.start / page_residues;
    const uint64_t first_page_offset = request.start % page_residues;
    const uint64_t aligned_first_residue = first_page_offset & ~uint64_t{7};
    const uint64_t begin = first_page * packed_page_bytes +
        (aligned_first_residue / 8) * 5;

    const uint64_t last_residue = request.start + request.length;
    const uint64_t last_page = (last_residue - 1) / page_residues;
    const uint64_t last_page_end =
        last_residue - last_page * page_residues;
    const uint64_t end = last_page * packed_page_bytes +
        packed_bytes_for_residues(last_page_end);
    return {begin, end};
}

uint64_t estimate_io_ns(uint64_t read_bytes,
                        uint64_t range_count,
                        const ByteRangePlanOptions& options) {
    if (options.io_bandwidth_bytes_per_second == 0) {
        return std::numeric_limits<uint64_t>::max();
    }
    const uint64_t workers = std::max<uint64_t>(1, options.io_threads);
    const uint64_t rounds = range_count == 0
        ? 0
        : 1 + (range_count - 1) / workers;
    const long double transfer_ns =
        static_cast<long double>(read_bytes) * 1000000000.0L /
        static_cast<long double>(options.io_bandwidth_bytes_per_second);
    const long double latency_ns =
        static_cast<long double>(rounds) * options.io_latency_ns;
    const long double total = transfer_ns + latency_ns;
    if (total >= static_cast<long double>(
            std::numeric_limits<uint64_t>::max())) {
        return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(total);
}

ReadPlan build_plan(const SequenceRequests& requests,
                    uint64_t page_residues,
                    uint64_t packed_page_bytes,
                    uint64_t merge_gap_bytes,
                    uint64_t max_merged_range_bytes,
                    GapProfile* profile) {
    ReadPlan plan;
    plan.selected_merge_gap_bytes = merge_gap_bytes;
    plan.ranges.reserve(std::min<size_t>(requests.size(), 65536));
    uint64_t covered_end = 0;
    bool have_coverage = false;

    for (size_t i = 0; i < requests.size(); ++i) {
        const SequenceRequest& request = requests[i];
        plan.requested_residues += request.length;
        if (request.length == 0) continue;
        const PackedByteExtent extent = packed_byte_extent(
            request, page_residues, packed_page_bytes);

        if (!have_coverage) {
            if (profile != nullptr) profile->exact_ranges = 1;
        } else if (extent.begin > covered_end && profile != nullptr) {
            ++profile->exact_ranges;
            const uint64_t gap = extent.begin - covered_end;
            profile->total_gap_bytes += gap;
            const uint64_t gap_index = profile->total_gaps++;
            if (gap_index % kGapSampleStride == 0) {
                ++profile->sampled_gaps;
                const auto bucket = std::lower_bound(
                    kAdaptiveMergeGaps.begin(),
                    kAdaptiveMergeGaps.end(), gap);
                const size_t index = static_cast<size_t>(
                    bucket - kAdaptiveMergeGaps.begin());
                ++profile->counts[index];
                profile->bytes[index] += gap;
            }
        }

        if (!have_coverage || extent.end > covered_end) {
            const uint64_t uncovered_begin = have_coverage
                ? std::max(extent.begin, covered_end)
                : extent.begin;
            plan.unique_packed_bytes += extent.end - uncovered_begin;
            covered_end = extent.end;
            have_coverage = true;
        }

        const bool overlaps = !plan.ranges.empty() &&
            extent.begin <= plan.ranges.back().end;
        const uint64_t gap = plan.ranges.empty() || overlaps
            ? 0
            : extent.begin - plan.ranges.back().end;
        const bool within_size_limit = plan.ranges.empty() || overlaps ||
            max_merged_range_bytes == 0 ||
            extent.end - plan.ranges.back().begin <= max_merged_range_bytes;
        if (!plan.ranges.empty() && within_size_limit &&
            (overlaps || gap <= merge_gap_bytes)) {
            ByteRange& range = plan.ranges.back();
            if (!overlaps) ++plan.merged_gaps;
            range.end = std::max(range.end, extent.end);
            range.request_end = i + 1;
        } else {
            plan.ranges.push_back(
                ByteRange{extent.begin, extent.end, i, i + 1});
        }
    }

    for (const ByteRange& range : plan.ranges) {
        const uint64_t bytes = range.end - range.begin;
        plan.read_bytes += bytes;
        plan.max_range_bytes = std::max(plan.max_range_bytes, bytes);
    }
    return plan;
}

ReadPlan merge_plan(ReadPlan plan,
                    uint64_t merge_gap_bytes,
                    uint64_t max_merged_range_bytes) {
    std::vector<ByteRange> merged;
    merged.reserve(plan.ranges.size());
    uint64_t newly_merged_gaps = 0;
    for (const ByteRange& range : plan.ranges) {
        const bool overlaps = !merged.empty() &&
            range.begin <= merged.back().end;
        const uint64_t gap = merged.empty() || overlaps
            ? 0
            : range.begin - merged.back().end;
        const bool within_size_limit = merged.empty() || overlaps ||
            max_merged_range_bytes == 0 ||
            range.end - merged.back().begin <= max_merged_range_bytes;
        if (!merged.empty() && within_size_limit &&
            (overlaps || gap <= merge_gap_bytes)) {
            if (!overlaps) ++newly_merged_gaps;
            merged.back().end = std::max(merged.back().end, range.end);
            merged.back().request_end = range.request_end;
        } else {
            merged.push_back(range);
        }
    }
    plan.ranges = std::move(merged);
    plan.selected_merge_gap_bytes = merge_gap_bytes;
    plan.merged_gaps += newly_merged_gaps;
    plan.read_bytes = 0;
    plan.max_range_bytes = 0;
    for (const ByteRange& range : plan.ranges) {
        const uint64_t bytes = range.end - range.begin;
        plan.read_bytes += bytes;
        plan.max_range_bytes = std::max(plan.max_range_bytes, bytes);
    }
    return plan;
}

}  // namespace

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count) {
    if (requests.size() <= 1) return;
    if (sequence_count == 0 || sequence_count - 1 >
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        std::sort(requests.begin(), requests.end(),
                  [](const SequenceRequest& left,
                     const SequenceRequest& right) {
                      return left.sequence_id < right.sequence_id;
                  });
        return;
    }

    constexpr unsigned kRadixBits = 11;
    constexpr size_t kRadix = size_t{1} << kRadixBits;
    constexpr uint32_t kMask = static_cast<uint32_t>(kRadix - 1);
    thread_count = requests.size() >= 65536
        ? std::min(std::max<size_t>(1, thread_count), requests.size())
        : size_t{1};
    std::vector<std::array<size_t, kRadix>> counts(thread_count);
    std::vector<std::array<size_t, kRadix>> positions(thread_count);

    auto run_workers = [&](auto&& function) {
        std::vector<std::thread> workers;
        workers.reserve(thread_count - 1);
        for (size_t worker = 1; worker < thread_count; ++worker) {
            workers.emplace_back(function, worker);
        }
        function(size_t{0});
        for (std::thread& worker : workers) worker.join();
    };

    auto count_pass = [&](auto&& key_at, unsigned shift) {
        run_workers([&](size_t worker) {
            auto& local = counts[worker];
            local.fill(0);
            const size_t begin = requests.size() * worker / thread_count;
            const size_t end = requests.size() * (worker + 1) / thread_count;
            for (size_t i = begin; i < end; ++i) {
                ++local[(key_at(i) >> shift) & kMask];
            }
        });

        size_t sum = 0;
        for (size_t bucket = 0; bucket < kRadix; ++bucket) {
            size_t bucket_total = 0;
            for (size_t worker = 0; worker < thread_count; ++worker) {
                positions[worker][bucket] = sum + bucket_total;
                bucket_total += counts[worker][bucket];
            }
            sum += bucket_total;
        }
    };

    struct CompactKey {
        uint32_t sequence_id;
        uint32_t request_index;
    };
    static_assert(sizeof(CompactKey) == 8);
    const bool use_compact_keys = requests.size() >= 4000000 &&
        requests.size() <= static_cast<size_t>(
            std::numeric_limits<uint32_t>::max());
    if (use_compact_keys) {
        std::vector<CompactKey> keys(requests.size());
        std::vector<CompactKey> scratch_keys(requests.size());
        SequenceRequests sorted_requests(requests.size());
        run_workers([&](size_t worker) {
            const size_t begin = requests.size() * worker / thread_count;
            const size_t end = requests.size() * (worker + 1) / thread_count;
            for (size_t i = begin; i < end; ++i) {
                keys[i] = CompactKey{
                    static_cast<uint32_t>(requests[i].sequence_id),
                    static_cast<uint32_t>(i)};
            }
        });
        for (unsigned shift = 0; shift < 32; shift += kRadixBits) {
            count_pass([&](size_t i) { return keys[i].sequence_id; }, shift);
            const bool final_pass = shift + kRadixBits >= 32;
            run_workers([&](size_t worker) {
                auto& local = positions[worker];
                const size_t begin = requests.size() * worker / thread_count;
                const size_t end = requests.size() * (worker + 1) / thread_count;
                for (size_t i = begin; i < end; ++i) {
                    const CompactKey key = keys[i];
                    const size_t bucket = (key.sequence_id >> shift) & kMask;
                    const size_t output = local[bucket]++;
                    if (final_pass) {
                        sorted_requests[output] = requests[key.request_index];
                    } else {
                        scratch_keys[output] = key;
                    }
                }
            });
            if (!final_pass) keys.swap(scratch_keys);
        }
        requests.swap(sorted_requests);
        return;
    }

    SequenceRequests scratch(requests.size());
    for (unsigned shift = 0; shift < 32; shift += kRadixBits) {
        count_pass(
            [&](size_t i) {
                return static_cast<uint32_t>(requests[i].sequence_id);
            },
            shift);

        run_workers([&](size_t worker) {
            auto& local = positions[worker];
            const size_t begin = requests.size() * worker / thread_count;
            const size_t end = requests.size() * (worker + 1) / thread_count;
            for (size_t i = begin; i < end; ++i) {
                const SequenceRequest request = requests[i];
                const size_t bucket =
                    (static_cast<uint32_t>(request.sequence_id) >> shift) &
                    kMask;
                scratch[local[bucket]++] = request;
            }
        });
        requests.swap(scratch);
    }
}

ReadPlan plan_byte_ranges(const SequenceRequests& requests,
                          uint64_t page_residues,
                          uint64_t packed_page_bytes,
                          const ByteRangePlanOptions& options) {
    if (!options.adaptive_merge_gap) {
        ReadPlan plan = build_plan(
            requests, page_residues, packed_page_bytes,
            options.merge_gap_bytes, 0, nullptr);
        plan.estimated_io_ns = estimate_io_ns(
            plan.read_bytes, plan.ranges.size(), options);
        return plan;
    }
    if (options.io_bandwidth_bytes_per_second == 0) {
        throw std::invalid_argument(
            "adaptive I/O bandwidth must be greater than zero");
    }

    GapProfile profile;
    ReadPlan pivot = build_plan(
        requests, page_residues, packed_page_bytes, kAdaptivePivotGap,
        options.max_merged_range_bytes, &profile);
    pivot.merge_candidates = kAdaptiveMergeGaps.size();
    if (profile.exact_ranges == 0) return pivot;

    uint64_t cumulative_sample_count = 0;
    uint64_t cumulative_sample_bytes = 0;
    uint64_t selected_gap = 0;
    uint64_t selected_cost = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < kAdaptiveMergeGaps.size(); ++i) {
        cumulative_sample_count += profile.counts[i];
        cumulative_sample_bytes += profile.bytes[i];
        const bool includes_all_gaps =
            i + 1 == kAdaptiveMergeGaps.size();
        const uint64_t merged_count = includes_all_gaps
            ? profile.total_gaps
            : scale_sample(cumulative_sample_count, profile.total_gaps,
                           profile.sampled_gaps, profile.total_gaps);
        const uint64_t merged_bytes = includes_all_gaps
            ? profile.total_gap_bytes
            : scale_sample(cumulative_sample_bytes, profile.total_gaps,
                           profile.sampled_gaps,
                           profile.total_gap_bytes);
        const uint64_t ranges = profile.exact_ranges - merged_count;
        const uint64_t read_bytes =
            pivot.unique_packed_bytes + merged_bytes;
        const uint64_t cost = estimate_io_ns(read_bytes, ranges, options);
        if (cost < selected_cost) {
            selected_cost = cost;
            selected_gap = kAdaptiveMergeGaps[i];
        }
    }

    ReadPlan plan;
    if (selected_gap < kAdaptivePivotGap) {
        plan = build_plan(
            requests, page_residues, packed_page_bytes, selected_gap,
            options.max_merged_range_bytes, nullptr);
    } else if (selected_gap == kAdaptivePivotGap) {
        plan = std::move(pivot);
    } else {
        plan = merge_plan(std::move(pivot), selected_gap,
                          options.max_merged_range_bytes);
    }
    plan.merge_candidates = kAdaptiveMergeGaps.size();
    plan.estimated_io_ns = estimate_io_ns(
        plan.read_bytes, plan.ranges.size(), options);
    return plan;
}

}  // namespace aastore::detail
