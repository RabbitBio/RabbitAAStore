#ifndef AASTORE_STORE_HPP
#define AASTORE_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace aastore {

using SequenceId = uint64_t;

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

inline constexpr uint64_t kByteRangeMergeGapBytes = 4096;
inline constexpr uint64_t kAdaptiveIoLatencyNs = 5000;
inline constexpr uint64_t kAdaptiveColdIoLatencyNs = 3000;
inline constexpr uint64_t kAdaptiveIoBandwidthBytesPerSecond =
    8ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kAdaptiveColdIoBandwidthBytesPerSecond =
    4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kAdaptiveMaxRangeBytes =
    1ULL * 1024ULL * 1024ULL;
inline constexpr uint64_t kPackedPageBytes = 4096;
inline constexpr uint64_t kPageResidues = (kPackedPageBytes * 8) / 5;

struct BuildOptions {
    bool overwrite = false;
};

struct BuildStats {
    uint64_t sequences = 0;
    uint64_t residues = 0;
    uint64_t pages = 0;
    uint64_t packed_bytes = 0;
};

class Builder {
public:
    static BuildStats build_fasta(const std::filesystem::path& fasta,
                                  const std::filesystem::path& store,
                                  const BuildOptions& options = {});
};

struct AdaptiveMergeOptions {
    bool cache_aware = true;
    uint64_t io_latency_ns = kAdaptiveIoLatencyNs;
    uint64_t cold_io_latency_ns = kAdaptiveColdIoLatencyNs;
    uint64_t io_bandwidth_bytes_per_second =
        kAdaptiveIoBandwidthBytesPerSecond;
    uint64_t cold_io_bandwidth_bytes_per_second =
        kAdaptiveColdIoBandwidthBytesPerSecond;
    // Zero disables the cap. A range that is intrinsically larger than the
    // cap is retained because it cannot be split without duplicating decode.
    uint64_t max_merged_range_bytes = kAdaptiveMaxRangeBytes;
};

struct ReadOptions {
    size_t io_threads = 8;
    bool input_sorted = false;
    bool adaptive_merge_gap = false;
    uint64_t merge_gap_bytes = kByteRangeMergeGapBytes;
    AdaptiveMergeOptions adaptive;
};

struct BatchStats {
    uint64_t requested_sequences = 0;
    uint64_t requested_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t ranges = 0;
    uint64_t merged_gaps = 0;
    uint64_t read_bytes = 0;
    uint64_t overread_bytes = 0;
    uint64_t selected_merge_gap_bytes = 0;
    uint64_t max_range_bytes = 0;
    uint64_t estimated_io_ns = 0;
    uint64_t merge_candidates = 0;
    uint64_t modeled_io_latency_ns = 0;
    uint64_t modeled_io_bandwidth_bytes_per_second = 0;
    uint64_t page_cache_resident_bytes = 0;
    uint64_t page_cache_total_bytes = 0;
    uint64_t workers = 0;
    bool adaptive_merge_gap = false;
    bool page_cache_residency_known = false;
    uint64_t layout_ns = 0;
    uint64_t sort_ns = 0;
    uint64_t range_plan_ns = 0;
    uint64_t worker_wall_ns = 0;
    uint64_t worker_max_busy_ns = 0;
    uint64_t read_ns = 0;
    uint64_t decode_ns = 0;
};

class ByteArena {
public:
    ByteArena() = default;
    ByteArena(ByteArena&&) noexcept = default;
    ByteArena& operator=(ByteArena&&) noexcept = default;
    ByteArena(const ByteArena&) = delete;
    ByteArena& operator=(const ByteArena&) = delete;

    void resize_uninitialized(size_t size);
    char* data() noexcept { return data_.get(); }
    const char* data() const noexcept { return data_.get(); }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    char& operator[](size_t index) noexcept { return data_[index]; }
    const char& operator[](size_t index) const noexcept { return data_[index]; }

private:
    std::unique_ptr<char[]> data_;
    size_t size_ = 0;
};

class Batch {
public:
    size_t size() const noexcept { return lengths.size(); }
    std::string_view view(size_t index) const;

    ByteArena storage;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> offsets;
    std::vector<uint64_t, NoInitAllocator<uint64_t>> lengths;
    BatchStats stats;
};

class Store {
public:
    explicit Store(const std::filesystem::path& path);
    ~Store();

    Store(Store&&) noexcept;
    Store& operator=(Store&&) noexcept;
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    uint64_t size() const noexcept;
    uint64_t total_residues() const noexcept;
    uint64_t packed_bytes() const noexcept;
    uint64_t sequence_length(SequenceId id) const;
    const char* decode_kernel_name() const noexcept;

    std::string get(SequenceId id) const;
    Batch batch_get(const std::vector<SequenceId>& ids,
                    const ReadOptions& options = {}) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace aastore

#endif
