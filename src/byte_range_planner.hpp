#ifndef AASTORE_BYTE_RANGE_PLANNER_HPP
#define AASTORE_BYTE_RANGE_PLANNER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace aastore::detail {

template <typename T>
class NoInitAllocator {
public:
    using value_type = T;

    NoInitAllocator() noexcept = default;
    template <typename U>
    NoInitAllocator(const NoInitAllocator<U>&) noexcept {}

    T* allocate(size_t count) {
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* pointer, size_t count) noexcept {
        std::allocator<T>{}.deallocate(pointer, count);
    }
    template <typename U, typename... Args>
    void construct(U* pointer, Args&&... args) {
        if constexpr (sizeof...(Args) != 0 ||
                      !std::is_trivially_default_constructible_v<U>) {
            ::new (static_cast<void*>(pointer))
                U(std::forward<Args>(args)...);
        } else {
            (void)pointer;
        }
    }
};

template <typename T, typename U>
bool operator==(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const NoInitAllocator<T>&,
                const NoInitAllocator<U>&) noexcept {
    return false;
}

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
};

void sort_requests_by_sequence_id(SequenceRequests& requests,
                                  size_t thread_count,
                                  uint64_t sequence_count);

ReadPlan plan_byte_ranges(const SequenceRequests& sorted_requests,
                          uint64_t page_residues,
                          uint64_t packed_page_bytes);

}  // namespace aastore::detail

#endif
