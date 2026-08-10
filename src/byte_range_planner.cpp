#include "byte_range_planner.hpp"

#include "aastore/store.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>

namespace aastore::detail {
namespace {

uint64_t packed_bytes_for_residues(uint64_t residues) {
    return (residues * 5 + 7) / 8;
}

struct PackedByteExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
};

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

}  // namespace

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count) {
    if (requests.size() <= 1) return;
    const uint64_t max_sequence_id = std::max_element(
        requests.begin(), requests.end(),
        [](const SequenceRequest& left, const SequenceRequest& right) {
            return left.sequence_id < right.sequence_id;
        })->sequence_id;
    if (max_sequence_id >
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
                          uint64_t packed_page_bytes) {
    ReadPlan plan;
    plan.ranges.reserve(std::min<size_t>(requests.size(), 65536));
    uint64_t covered_end = 0;
    bool have_coverage = false;

    for (size_t i = 0; i < requests.size(); ++i) {
        const SequenceRequest& request = requests[i];
        plan.requested_residues += request.length;
        if (request.length == 0) continue;
        const PackedByteExtent extent = packed_byte_extent(
            request, page_residues, packed_page_bytes);

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
        if (!plan.ranges.empty() &&
            (overlaps || gap <= kByteRangeMergeGapBytes)) {
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
        plan.read_bytes += range.end - range.begin;
    }
    return plan;
}

}  // namespace aastore::detail
