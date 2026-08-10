#include "aastore/store.hpp"

#include "codec.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace aastore {
namespace {

class CompactMetadataWriter {
public:
    explicit CompactMetadataWriter(const std::filesystem::path& path)
        : final_path_(path), temporary_path_(path.string() + ".tmp") {
        output_.open(temporary_path_, std::ios::binary | std::ios::in |
                                          std::ios::out | std::ios::trunc);
        if (!output_) throw std::runtime_error("cannot create compact metadata");
        std::array<char, kHeaderBytes> placeholder{};
        output_.write(placeholder.data(), placeholder.size());
        offsets_.reserve(kBufferEntries);
        append(0);
    }

    ~CompactMetadataWriter() {
        if (output_.is_open()) output_.close();
        if (!finished_) {
            std::error_code error;
            std::filesystem::remove(temporary_path_, error);
        }
    }

    void append(uint64_t position) {
        if (position_count_ != 0 && position < last_position_) {
            throw std::runtime_error("non-monotonic sequence position");
        }
        if ((position_count_ & (kBlockSize - 1)) == 0) {
            current_base_ = position;
            bases_.push_back(position);
        }
        const uint64_t relative = position - current_base_;
        if (relative > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("compact metadata block exceeds uint32");
        }
        offsets_.push_back(static_cast<uint32_t>(relative));
        ++position_count_;
        last_position_ = position;
        if (offsets_.size() == kBufferEntries) flush_offsets();
    }

    void finish(uint64_t sequence_count,
                uint64_t total_residues,
                uint64_t page_count) {
        if (position_count_ != sequence_count + 1 ||
            last_position_ != total_residues) {
            throw std::runtime_error("incomplete compact metadata");
        }
        flush_offsets();
        const uint64_t block_count = sequence_count == 0
            ? 0
            : (sequence_count + kBlockSize - 1) / kBlockSize;
        if (bases_.size() == block_count) bases_.push_back(total_residues);
        if (bases_.size() != block_count + 1) {
            throw std::runtime_error("invalid compact metadata bases");
        }
        output_.write(reinterpret_cast<const char*>(bases_.data()),
                      static_cast<std::streamsize>(
                          bases_.size() * sizeof(uint64_t)));
        output_.seekp(0, std::ios::beg);
        char magic[8] = {};
        std::memcpy(magic, "AAPCMP2", 7);
        output_.write(magic, sizeof(magic));
        write_value(uint64_t{2});
        write_value(sequence_count);
        write_value(total_residues);
        write_value(kPageResidues);
        write_value(page_count);
        write_value(kBlockShift);
        write_value(block_count);
        write_value(sequence_count + 1);
        write_value(uint64_t{1});  // stop-aware '*'/'-' encoding
        output_.close();
        if (!output_) throw std::runtime_error("failed to finalize metadata");
        std::filesystem::rename(temporary_path_, final_path_);
        finished_ = true;
    }

private:
    template <typename T>
    void write_value(const T& value) {
        output_.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    void flush_offsets() {
        if (offsets_.empty()) return;
        output_.write(reinterpret_cast<const char*>(offsets_.data()),
                      static_cast<std::streamsize>(
                          offsets_.size() * sizeof(uint32_t)));
        if (!output_) throw std::runtime_error("failed to write metadata offsets");
        offsets_.clear();
    }

    static constexpr uint64_t kBlockShift = 12;
    static constexpr uint64_t kBlockSize = uint64_t{1} << kBlockShift;
    static constexpr size_t kHeaderBytes = 8 + 9 * sizeof(uint64_t);
    static constexpr size_t kBufferEntries = 2 * 1024 * 1024;
    std::filesystem::path final_path_;
    std::filesystem::path temporary_path_;
    std::fstream output_;
    std::vector<uint32_t> offsets_;
    std::vector<uint64_t> bases_;
    uint64_t position_count_ = 0;
    uint64_t last_position_ = 0;
    uint64_t current_base_ = 0;
    bool finished_ = false;
};

std::string normalize_sequence_line(const std::string& line) {
    std::string normalized;
    normalized.reserve(line.size());
    for (unsigned char value : line) {
        if (std::isspace(value)) continue;
        if (std::isalpha(value)) {
            normalized.push_back(
                static_cast<char>(std::toupper(value)));
        } else if (value == '*' || value == '-') {
            normalized.push_back(static_cast<char>(value));
        } else {
            normalized.push_back('X');
        }
    }
    return normalized;
}

void replace_file(const std::filesystem::path& from,
                  const std::filesystem::path& to,
                  bool overwrite) {
    if (overwrite) {
        std::error_code error;
        std::filesystem::remove(to, error);
    }
    std::filesystem::rename(from, to);
}

}  // namespace

BuildStats Builder::build_fasta(const std::filesystem::path& fasta,
                                const std::filesystem::path& store,
                                const BuildOptions& options) {
    namespace fs = std::filesystem;
    std::ifstream input(fasta);
    if (!input) throw std::runtime_error("cannot open FASTA: " + fasta.string());

    fs::create_directories(store);
    const fs::path pages_path = store / "pages.bin";
    const fs::path meta_path = store / "meta.bin";
    const fs::path names_path = store / "seq_names.txt";
    if (!options.overwrite &&
        (fs::exists(pages_path) || fs::exists(meta_path) ||
         fs::exists(names_path))) {
        throw std::runtime_error("store already exists: " + store.string());
    }

    const std::string suffix = ".tmp." + std::to_string(::getpid());
    const fs::path pages_tmp = pages_path.string() + suffix;
    const fs::path meta_tmp = meta_path.string() + suffix;
    const fs::path names_tmp = names_path.string() + suffix;
    const std::array<fs::path, 3> temporary = {
        pages_tmp, meta_tmp, names_tmp};
    auto cleanup = [&]() noexcept {
        std::error_code error;
        for (const fs::path& path : temporary) fs::remove(path, error);
    };
    cleanup();

    try {
        std::ofstream pages(pages_tmp, std::ios::binary | std::ios::trunc);
        std::ofstream names(names_tmp, std::ios::binary | std::ios::trunc);
        if (!pages || !names) throw std::runtime_error("cannot create store files");
        CompactMetadataWriter metadata(meta_tmp);
        BuildStats stats;
        std::vector<uint8_t> page(kPackedPageBytes, 0);
        uint64_t page_fill = 0;
        bool sequence_open = false;

        auto flush_page = [&]() {
            if (page_fill == 0) return;
            const uint64_t bytes = detail::packed_bytes_for_residues(page_fill);
            pages.write(reinterpret_cast<const char*>(page.data()),
                        static_cast<std::streamsize>(bytes));
            if (!pages) throw std::runtime_error("failed to write packed page");
            ++stats.pages;
            stats.packed_bytes += bytes;
            page_fill = 0;
            std::fill(page.begin(), page.end(), uint8_t{0});
        };

        auto finish_sequence = [&]() {
            if (!sequence_open) return;
            ++stats.sequences;
            metadata.append(stats.residues);
            sequence_open = false;
        };

        auto append_residues = [&](const std::string& residues) {
            size_t consumed = 0;
            while (consumed < residues.size()) {
                if (page_fill == kPageResidues) flush_page();
                const size_t take = std::min<size_t>(
                    residues.size() - consumed,
                    static_cast<size_t>(kPageResidues - page_fill));
                detail::append_packed_5bit(page.data(), page_fill,
                                           residues.data() + consumed, take);
                consumed += take;
                stats.residues += take;
            }
        };

        std::string line;
        uint64_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line.front() == '>') {
                finish_sequence();
                const size_t name_end = line.find_first_of(" \t", 1);
                names << line.substr(1, name_end == std::string::npos
                                            ? std::string::npos
                                            : name_end - 1)
                      << '\n';
                if (!names) throw std::runtime_error("failed to write names");
                sequence_open = true;
            } else if (!line.empty()) {
                if (!sequence_open) {
                    throw std::runtime_error(
                        "FASTA sequence before first header at line " +
                        std::to_string(line_number));
                }
                append_residues(normalize_sequence_line(line));
            }
        }
        if (!input.eof()) throw std::runtime_error("failed while reading FASTA");
        finish_sequence();
        flush_page();
        pages.close();
        names.close();
        if (!pages || !names) throw std::runtime_error("failed to finalize store");
        metadata.finish(stats.sequences, stats.residues, stats.pages);

        replace_file(pages_tmp, pages_path, options.overwrite);
        replace_file(meta_tmp, meta_path, options.overwrite);
        replace_file(names_tmp, names_path, options.overwrite);
        return stats;
    } catch (...) {
        cleanup();
        throw;
    }
}

}  // namespace aastore
