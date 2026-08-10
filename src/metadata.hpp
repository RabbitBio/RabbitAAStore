#ifndef AASTORE_METADATA_HPP
#define AASTORE_METADATA_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace aastore::detail {

class Metadata {
public:
    explicit Metadata(const std::filesystem::path& store);
    ~Metadata();

    Metadata(Metadata&& other) noexcept;
    Metadata& operator=(Metadata&& other) noexcept;
    Metadata(const Metadata&) = delete;
    Metadata& operator=(const Metadata&) = delete;

    uint64_t sequence_count() const noexcept { return sequence_count_; }
    uint64_t total_residues() const noexcept { return total_residues_; }
    uint64_t page_residues() const noexcept { return page_residues_; }
    uint64_t page_count() const noexcept { return page_count_; }
    uint64_t packed_page_bytes() const noexcept;
    uint64_t expected_pages_bytes() const noexcept;
    uint64_t start_position(uint64_t sequence_index) const;

private:
    void open(const std::filesystem::path& store);
    bool map_compact(const std::filesystem::path& path,
                     bool initialize_store_fields);
    void map_native(const std::filesystem::path& path,
                    uint64_t positions_offset,
                    uint64_t positions_count);
    void clear() noexcept;

    uint64_t sequence_count_ = 0;
    uint64_t total_residues_ = 0;
    uint64_t page_residues_ = 0;
    uint64_t page_count_ = 0;
    bool stop_aware_encoding_ = false;

    void* mapping_ = nullptr;
    size_t mapping_bytes_ = 0;
    const uint64_t* native_positions_ = nullptr;
    const uint32_t* compact_offsets_ = nullptr;
    const uint64_t* compact_bases_ = nullptr;
    uint64_t compact_offset_count_ = 0;
    uint32_t compact_block_shift_ = 0;
};

}  // namespace aastore::detail

#endif
