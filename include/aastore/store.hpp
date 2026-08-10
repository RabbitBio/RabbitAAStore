#ifndef AASTORE_STORE_HPP
#define AASTORE_STORE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aastore {

using SequenceId = uint64_t;

// The refactored reader deliberately has one policy: exact packed-byte ranges
// separated by no more than 4 KiB are read together.
inline constexpr uint64_t kByteRangeMergeGapBytes = 4096;
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

struct ReadOptions {
    size_t io_threads = 8;
    bool input_sorted = false;
};

struct BatchStats {
    uint64_t requested_sequences = 0;
    uint64_t requested_residues = 0;
    uint64_t unique_packed_bytes = 0;
    uint64_t ranges = 0;
    uint64_t merged_gaps = 0;
    uint64_t read_bytes = 0;
    uint64_t overread_bytes = 0;
    uint64_t workers = 0;
    uint64_t layout_ns = 0;
    uint64_t sort_ns = 0;
    uint64_t range_plan_ns = 0;
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
    std::vector<uint64_t> offsets;
    std::vector<uint64_t> lengths;
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
