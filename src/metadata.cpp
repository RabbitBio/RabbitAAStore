#include "metadata.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

namespace aastore::detail {
namespace {

template <typename T>
bool read_value(std::istream& input, T& value) {
    input.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(input);
}

uint64_t computed_page_count(uint64_t residues, uint64_t page_residues) {
    if (page_residues == 0) return 0;
    return residues == 0 ? 0 : (residues - 1) / page_residues + 1;
}

size_t checked_size(uint64_t value, const char* description) {
    if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error(description);
    }
    return static_cast<size_t>(value);
}

void* map_read_only(const std::filesystem::path& path, size_t bytes) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open " + path.string() + ": " +
                                 std::strerror(errno));
    }
    void* mapping = ::mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE,
                           descriptor, 0);
    const int saved_errno = errno;
    ::close(descriptor);
    if (mapping == MAP_FAILED) {
        throw std::runtime_error("cannot mmap " + path.string() + ": " +
                                 std::strerror(saved_errno));
    }
    return mapping;
}

}  // namespace

Metadata::Metadata(const std::filesystem::path& store) { open(store); }

Metadata::~Metadata() { clear(); }

Metadata::Metadata(Metadata&& other) noexcept { *this = std::move(other); }

Metadata& Metadata::operator=(Metadata&& other) noexcept {
    if (this == &other) return *this;
    clear();
    sequence_count_ = other.sequence_count_;
    total_residues_ = other.total_residues_;
    page_residues_ = other.page_residues_;
    page_count_ = other.page_count_;
    stop_aware_encoding_ = other.stop_aware_encoding_;
    mapping_ = other.mapping_;
    mapping_bytes_ = other.mapping_bytes_;
    native_positions_ = other.native_positions_;
    compact_offsets_ = other.compact_offsets_;
    compact_bases_ = other.compact_bases_;
    compact_offset_count_ = other.compact_offset_count_;
    compact_block_shift_ = other.compact_block_shift_;
    other.mapping_ = nullptr;
    other.mapping_bytes_ = 0;
    other.native_positions_ = nullptr;
    other.compact_offsets_ = nullptr;
    other.compact_bases_ = nullptr;
    return *this;
}

void Metadata::clear() noexcept {
    if (mapping_ != nullptr) ::munmap(mapping_, mapping_bytes_);
    mapping_ = nullptr;
    mapping_bytes_ = 0;
    native_positions_ = nullptr;
    compact_offsets_ = nullptr;
    compact_bases_ = nullptr;
    compact_offset_count_ = 0;
    compact_block_shift_ = 0;
}

uint64_t Metadata::packed_page_bytes() const noexcept {
    return (page_residues_ * 5 + 7) / 8;
}

uint64_t Metadata::expected_pages_bytes() const noexcept {
    if (page_count_ == 0) return 0;
    const uint64_t tail = total_residues_ % page_residues_;
    const uint64_t last_page_residues = tail == 0 ? page_residues_ : tail;
    return (page_count_ - 1) * packed_page_bytes() +
           (last_page_residues * 5 + 7) / 8;
}

uint64_t Metadata::start_position(uint64_t index) const {
    if (index > sequence_count_) {
        throw std::out_of_range("sequence index out of range");
    }
    if (compact_offsets_ != nullptr) {
        const uint64_t block = index >> compact_block_shift_;
        return compact_bases_[block] + compact_offsets_[index];
    }
    return native_positions_[index];
}

bool Metadata::map_compact(const std::filesystem::path& path,
                           bool initialize_store_fields) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    char magic[8] = {};
    uint64_t version = 0;
    uint64_t sequence_count = 0;
    uint64_t total_residues = 0;
    uint64_t page_residues = 0;
    uint64_t page_count = 0;
    uint64_t block_shift = 0;
    uint64_t block_count = 0;
    uint64_t offset_count = 0;
    uint64_t encoding_flags = 0;
    input.read(magic, sizeof(magic));
    const bool version_one = std::strncmp(magic, "AAPCMP1", 7) == 0;
    const bool version_two = std::strncmp(magic, "AAPCMP2", 7) == 0;
    if ((!version_one && !version_two) || !read_value(input, version) ||
        version != (version_one ? 1ULL : 2ULL) ||
        !read_value(input, sequence_count) ||
        !read_value(input, total_residues) ||
        !read_value(input, page_residues) ||
        !read_value(input, page_count) ||
        !read_value(input, block_shift) ||
        !read_value(input, block_count) ||
        !read_value(input, offset_count) ||
        !read_value(input, encoding_flags)) {
        return false;
    }
    if (offset_count != sequence_count + 1 || block_shift == 0 ||
        block_shift > 31 || (encoding_flags & ~1ULL) != 0 ||
        page_residues == 0 ||
        page_count != computed_page_count(total_residues, page_residues)) {
        return false;
    }
    if (!initialize_store_fields &&
        (sequence_count != sequence_count_ ||
         total_residues != total_residues_ ||
         page_residues != page_residues_ || page_count != page_count_ ||
         ((encoding_flags & 1ULL) != 0) != stop_aware_encoding_)) {
        return false;
    }

    constexpr uint64_t kHeaderBytes = 8 + 9 * sizeof(uint64_t);
    const uint64_t bases_bytes = (block_count + 1) * sizeof(uint64_t);
    const uint64_t offsets_bytes = offset_count * sizeof(uint32_t);
    const uint64_t expected_bytes = kHeaderBytes + bases_bytes + offsets_bytes;
    if (std::filesystem::file_size(path) != expected_bytes) return false;

    clear();
    mapping_bytes_ = checked_size(expected_bytes, "metadata exceeds address space");
    mapping_ = map_read_only(path, mapping_bytes_);
    const char* base = static_cast<const char*>(mapping_);
    if (version_one) {
        compact_bases_ = reinterpret_cast<const uint64_t*>(
            base + kHeaderBytes);
        compact_offsets_ = reinterpret_cast<const uint32_t*>(
            base + kHeaderBytes + bases_bytes);
    } else {
        compact_offsets_ = reinterpret_cast<const uint32_t*>(
            base + kHeaderBytes);
        compact_bases_ = reinterpret_cast<const uint64_t*>(
            base + kHeaderBytes + offsets_bytes);
    }
    compact_offset_count_ = offset_count;
    compact_block_shift_ = static_cast<uint32_t>(block_shift);
    if (initialize_store_fields) {
        sequence_count_ = sequence_count;
        total_residues_ = total_residues;
        page_residues_ = page_residues;
        page_count_ = page_count;
        stop_aware_encoding_ = (encoding_flags & 1ULL) != 0;
    }
    return start_position(0) == 0 &&
           start_position(sequence_count_) == total_residues_;
}

void Metadata::map_native(const std::filesystem::path& path,
                          uint64_t positions_offset,
                          uint64_t positions_count) {
    const uint64_t positions_bytes = positions_count * sizeof(uint64_t);
    const uint64_t required_bytes = positions_offset + positions_bytes;
    const uint64_t file_bytes = std::filesystem::file_size(path);
    if (file_bytes < required_bytes) {
        throw std::runtime_error("truncated original metadata");
    }
    clear();
    mapping_bytes_ = checked_size(file_bytes, "metadata exceeds address space");
    mapping_ = map_read_only(path, mapping_bytes_);
    native_positions_ = reinterpret_cast<const uint64_t*>(
        static_cast<const char*>(mapping_) + positions_offset);
    if (start_position(0) != 0 ||
        start_position(sequence_count_) != total_residues_) {
        throw std::runtime_error("invalid original sequence offsets");
    }
}

void Metadata::open(const std::filesystem::path& store) {
    const std::filesystem::path meta_path = store / "meta.bin";
    std::ifstream input(meta_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + meta_path.string());
    char magic[8] = {};
    input.read(magic, sizeof(magic));
    if (!input) throw std::runtime_error("cannot read original metadata");
    if (std::strncmp(magic, "AAPCMP", 6) == 0) {
        if (!map_compact(meta_path, true)) {
            throw std::runtime_error("invalid compact original metadata");
        }
        return;
    }

    const bool version_four = std::strncmp(magic, "AAPAGE4", 7) == 0;
    const bool version_three = std::strncmp(magic, "AAPAGE3", 7) == 0;
    if (!version_four && !version_three) {
        throw std::runtime_error(
            "unsupported original metadata version (expected AAPAGE3/4 or AAPCMP1/2)");
    }
    uint64_t version = 0;
    uint64_t duplicate_sequence_count = 0;
    if (!read_value(input, version) ||
        version != (version_four ? 4ULL : 3ULL) ||
        !read_value(input, sequence_count_) ||
        !read_value(input, total_residues_) ||
        !read_value(input, page_residues_) ||
        !read_value(input, duplicate_sequence_count) ||
        !read_value(input, page_count_)) {
        throw std::runtime_error("truncated original metadata header");
    }
    if (duplicate_sequence_count != sequence_count_ || page_residues_ == 0 ||
        page_count_ != computed_page_count(total_residues_, page_residues_)) {
        throw std::runtime_error("inconsistent original metadata header");
    }
    if (version_four) {
        uint64_t encoding_flags = 0;
        if (!read_value(input, encoding_flags) ||
            (encoding_flags & ~1ULL) != 0) {
            throw std::runtime_error("invalid original encoding flags");
        }
        stop_aware_encoding_ = (encoding_flags & 1ULL) != 0;
    }

    const std::filesystem::path compact_path =
        store / "meta.compact_u32.bin";
    if (std::filesystem::exists(compact_path) &&
        map_compact(compact_path, false)) {
        return;
    }
    const std::streampos position = input.tellg();
    if (position == std::streampos(-1)) {
        throw std::runtime_error("cannot locate original sequence offsets");
    }
    map_native(meta_path, static_cast<uint64_t>(position),
               sequence_count_ + 1);
}

}  // namespace aastore::detail
