#ifndef __PROTEIN_AASTORE__
#define __PROTEIN_AASTORE__

#include "Packed5BitDecoder.hpp"
#include "Packed5BitEncoder.hpp"

#include <sdsl/int_vector.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <fstream>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <algorithm>

class ProteinAAStore {
public:
    void init(uint64_t reserve_size) {
        pages_.reserve((reserve_size * 5 + 7) / 8);
    }

    static uint64_t default_packed_page_bytes() {
        return 4096;
    }

    static uint64_t default_page_residues() {
        return (default_packed_page_bytes() * 8) / 5;
    }

    void set_page_residues(uint64_t page_residues) {
        if (page_residues == 0) throw std::invalid_argument("page_residues must be positive");
        if (total_length_ != 0 || !pages_.empty() || data_.size() != 0) {
            throw std::runtime_error("cannot change page_residues after data has been added");
        }
        page_residues_ = page_residues;
        packed_page_bytes_ = packed_bytes_for_residues(page_residues_);
    }

    static inline uint8_t enc(char c) {
        return uint8_t(c & 0x1F);
    }

    static inline char dec(uint8_t code) {
        return char(0x40 | code);
    }

    static const char* decode_kernel_name() {
        return aastore_detail::packed_5bit_decode_kernel_name(
            aastore_detail::packed_5bit_decode_kernel());
    }

    static const char* encode_kernel_name() {
        return aastore_detail::packed_5bit_encode_kernel_name(
            aastore_detail::packed_5bit_encode_kernel());
    }

    // Build from in-memory sequences.
    void build(const std::vector<std::string_view>& seqs) {
        n_ = seqs.size();
        total_length_ = 0;
        page_count_ = 0;
        data_ = sdsl::int_vector<5>();
        pages_.clear();
        start_positions_.clear();
        start_positions_.reserve(seqs.size() + 1);

        for (size_t i = 0; i < seqs.size(); ++i) {
            start_positions_.push_back(total_length_);
            auto s = seqs[i];
            append_sequence_to_pages(s);
        }
        // Record the end position of the last sequence
        start_positions_.push_back(total_length_);
    }

    size_t size() const { return n_; }

    // Add a sequence after build
    void add(std::string_view seq) {
        // Record start position
        start_positions_.push_back(total_length_);

        // Append sequence data
        append_sequence_to_pages(seq);

        add_name("");
        ++n_;
    }

    // Add a sequence with name after build
    void add(std::string_view seq, std::string_view name) {
        // Record start position
        start_positions_.push_back(total_length_);

        // Append sequence data
        append_sequence_to_pages(seq);

        add_name(name);
        ++n_;
    }

    // Save names and name index (uint64_t offsets) to separate files.
    // Returns false if names are missing or file I/O fails.
    // If keep_names is true, names_ will be restored after saving; otherwise it remains modified.
    bool save_names(const std::string& names_path, const std::string& index_path = "", bool keep_names = true) {
        if (name_idx_.empty() || name_idx_.size() != n_) return false;
        std::string idx_path = index_path.empty() ? (names_path + ".idx") : index_path;

        // Write index file directly
        std::ofstream index_out(idx_path, std::ios::binary);
        if (!index_out) return false;
        index_out.write(reinterpret_cast<const char*>(name_idx_.data()),
                        static_cast<std::streamsize>(name_idx_.size() * sizeof(uint64_t)));

        // Replace '\0' with '\n' for human readability
        for (char& c : names_) {
            if (c == '\0') c = '\n';
        }

        // Write names file directly
        std::ofstream names_out(names_path, std::ios::binary);
        if (!names_out) {
            // Restore '\0' on failure if needed
            if (keep_names) {
                for (char& c : names_) {
                    if (c == '\n') c = '\0';
                }
            }
            return false;
        }
        names_out.write(names_.data(), static_cast<std::streamsize>(names_.size()));

        // Restore '\0' if names_ will be used later
        if (keep_names) {
            for (char& c : names_) {
                if (c == '\n') c = '\0';
            }
        }
        return true;
    }

    // Load names and name index (uint64_t offsets) from separate files.
    // Returns false if file I/O fails or count mismatch with existing sequences.
    bool load_names(const std::string& names_path, const std::string& index_path = "") {
        std::string idx_path = index_path.empty() ? (names_path + ".idx") : index_path;

        // Read index file
        std::ifstream index_in(idx_path, std::ios::binary | std::ios::ate);
        if (!index_in) return false;
        std::streamsize idx_size = index_in.tellg();
        if (idx_size < 0 || (idx_size % static_cast<std::streamsize>(sizeof(uint64_t))) != 0) return false;
        size_t count = static_cast<size_t>(idx_size / sizeof(uint64_t));
        index_in.seekg(0);

        if (n_ != 0 && count != n_) return false;

        name_idx_.resize(count);
        if (count > 0) {
            index_in.read(reinterpret_cast<char*>(name_idx_.data()), idx_size);
            if (!index_in) return false;
        }

        // Read names file directly into buffer
        std::ifstream names_in(names_path, std::ios::binary | std::ios::ate);
        if (!names_in) return false;
        std::streamsize names_size = names_in.tellg();
        if (names_size < 0) return false;
        names_in.seekg(0);

        names_.resize(static_cast<size_t>(names_size));
        if (names_size > 0) {
            names_in.read(names_.data(), names_size);
            if (!names_in) return false;
            // Replace '\n' with '\0' for null-terminated strings
            for (char& c : names_) {
                if (c == '\n') c = '\0';
            }
        }
        if (names_.empty() || names_.back() != '\0') {
            names_.push_back('\0');
        }
        return true;
    }

    // Finalize: record the end position of the last sequence
    void finalize() {
        if (!start_positions_.empty() && start_positions_.size() == n_) {
            start_positions_.push_back(total_length_);
        }
    }

    // Save to paged directory format: meta.bin + pages.bin.
    bool save(const std::string& filepath) const {
        return save_paged_directory(filepath);
    }

    // Save to legacy single-file format for old experiments.
    bool save_legacy(const std::string& filepath) const {
        std::ofstream out(filepath, std::ios::binary);
        if (!out) return false;
        // write n_
        out.write(reinterpret_cast<const char*>(&n_), sizeof(n_));
        // write start_positions_
        uint64_t pos_size = start_positions_.size();
        out.write(reinterpret_cast<const char*>(&pos_size), sizeof(pos_size));
        if (pos_size) {
            out.write(reinterpret_cast<const char*>(start_positions_.data()), pos_size * sizeof(uint64_t));
        }
        if (!out) return false;
        // write data_ via sdsl serialize
        sdsl::int_vector<5> continuous = to_continuous_int_vector();
        continuous.serialize(out);
        return static_cast<bool>(out);
    }

    // Load from paged directory format, or legacy single-file format.
    bool load(const std::string& filepath) {
        namespace fs = std::filesystem;
        if (fs::is_directory(filepath)) {
            return load_paged_directory(filepath);
        }
        return load_legacy_file(filepath);
    }

    bool load_legacy_file(const std::string& filepath) {
        std::ifstream in(filepath, std::ios::binary | std::ios::ate);
        if (!in) return false;
        std::streamsize file_size = in.tellg();
        if (file_size < static_cast<std::streamsize>(sizeof(n_) + sizeof(uint64_t))) return false;
        in.seekg(0);
        auto t0 = std::chrono::high_resolution_clock::now();
        // read n_
        in.read(reinterpret_cast<char*>(&n_), sizeof(n_));
        // read start_positions_
        uint64_t pos_size = 0;
        in.read(reinterpret_cast<char*>(&pos_size), sizeof(pos_size));
        uint64_t max_pos_entries = static_cast<uint64_t>(file_size) / sizeof(uint64_t);
        if (pos_size > max_pos_entries || (n_ != 0 && pos_size != n_ + 1)) return false;
        start_positions_.resize(pos_size);
        if (pos_size) {
            in.read(reinterpret_cast<char*>(start_positions_.data()), pos_size * sizeof(uint64_t));
        }
        if (!in) return false;
        if (start_positions_.size() != n_ + 1) return false;
        if (!start_positions_.empty()) {
            uint64_t total_residues = start_positions_.back();
            uint64_t expected_data_bytes = ((total_residues * 5 + 63) / 64) * sizeof(uint64_t);
            uint64_t expected_file_size =
                sizeof(n_) + sizeof(uint64_t) + pos_size * sizeof(uint64_t) +
                sizeof(uint64_t) + expected_data_bytes;
            if (static_cast<uint64_t>(file_size) < expected_file_size) {
                std::cerr << "Error: truncated ProteinAAStore file. Expected at least "
                          << expected_file_size << " bytes, got "
                          << static_cast<uint64_t>(file_size) << " bytes" << std::endl;
                return false;
            }
        }
        std::cerr << "Load: read meta & positions (" << pos_size << " entries) in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t0)
                         .count()
                  << " ms" << std::endl;
        auto t1 = std::chrono::high_resolution_clock::now();
        // read data_
        data_.load(in);
        if (!in) return false;
        if (data_.size() != start_positions_.back()) return false;
        total_length_ = start_positions_.back();
        page_count_ = computed_page_count(total_length_, page_residues_);
        if (!convert_continuous_data_to_pages()) return false;
        data_ = sdsl::int_vector<5>();
        std::cerr << "Load: read data vector in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t1)
                         .count()
                  << " ms" << std::endl;
        // basic validation
        return true;
    }

    // Get i-th sequence (0-based)
    std::string get(size_t i) const {
        uint64_t start_pos = 0;
        uint64_t length = 0;
        sequence_bounds(i, start_pos, length);

        std::string out(static_cast<size_t>(length), '\0');
        if (!out.empty()) {
            decode_global_range(start_pos, length, out.data());
        }
        return out;
    }

    void get_into(size_t i, char* dst) const {
        uint64_t start_pos = 0;
        uint64_t length = 0;
        sequence_bounds(i, start_pos, length);
        if (length != 0) {
            decode_global_range(start_pos, length, dst);
        }
    }

    size_t get_raw(size_t i, std::vector<uint8_t>& out) const {
        uint64_t start_pos = 0;
        uint64_t length = 0;
        sequence_bounds(i, start_pos, length);

        out.resize(length);
        if (!out.empty()) {
            copy_global_codes_range(start_pos, length, out.data());
        }
        return length;
    }

    std::vector<std::string> batch_get(const std::vector<size_t>& ids) const {
        std::vector<std::string> out(ids.size());
        for (size_t i = 0; i < ids.size(); ++i) {
            uint64_t start_pos = 0;
            uint64_t len = 0;
            sequence_bounds(ids[i], start_pos, len);
            out[i].resize(static_cast<size_t>(len));
            if (len != 0) {
                decode_global_range(start_pos, len, out[i].data());
            }
        }
        return out;
    }

    void batch_get_into(const std::vector<size_t>& ids,
                        const std::vector<char*>& outputs) const {
        if (outputs.size() != ids.size()) {
            throw std::invalid_argument("outputs size must match ids size");
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            get_into(ids[i], outputs[i]);
        }
    }

    template <typename Visitor>
    void batch_visit(const std::vector<size_t>& ids, Visitor&& visitor) const {
        std::vector<uint64_t> offsets(ids.size() + 1, 0);
        for (size_t i = 0; i < ids.size(); ++i) {
            offsets[i + 1] = offsets[i] + length(ids[i]);
        }

        std::vector<char> buffer(static_cast<size_t>(offsets.back()));
        std::vector<char*> outputs(ids.size(), nullptr);
        for (size_t i = 0; i < ids.size(); ++i) {
            outputs[i] = buffer.empty()
                ? nullptr
                : buffer.data() + static_cast<size_t>(offsets[i]);
        }
        batch_get_into(ids, outputs);

        for (size_t i = 0; i < ids.size(); ++i) {
            const char* data = buffer.empty()
                ? nullptr
                : buffer.data() + static_cast<size_t>(offsets[i]);
            size_t len = static_cast<size_t>(offsets[i + 1] - offsets[i]);
            visitor(i, std::string_view(data, len));
        }
    }

    // Length of i-th sequence
    uint64_t length(size_t i) const {
        uint64_t start_pos = 0;
        uint64_t length = 0;
        sequence_bounds(i, start_pos, length);
        return length;
    }

    // Compute a lightweight checksum of the encoded sequence without allocating strings.
    uint64_t checksum(size_t i) const {
        uint64_t start_pos = 0;
        uint64_t length = 0;
        sequence_bounds(i, start_pos, length);
        return checksum_global_range(start_pos, length);
    }

    // Calculate memory usage in bytes (actual memory allocated)
    uint64_t memory_usage() const {
        uint64_t total = 0;
        total += pages_.capacity() * sizeof(uint8_t);
        if (data_.size() != 0) {
            total += ((data_.bit_size() + 63) >> 6) * sizeof(uint64_t);
        }
        // Memory for start_positions_ vector
        total += start_positions_.capacity() * sizeof(uint64_t);
        return total;
    }

    // Memory usage of names buffer and index (bytes)
    uint64_t names_memory_usage() const {
        uint64_t total = 0;
        total += names_.capacity() * sizeof(char);
        total += name_idx_.capacity() * sizeof(uint64_t);
        return total;
    }

    // Get memory breakdown: returns {data_size, positions_size, unused}
    std::tuple<uint64_t, uint64_t, uint64_t> memory_breakdown() const {
        uint64_t data_size = pages_.capacity() * sizeof(uint8_t);
        if (data_.size() != 0) {
            data_size += ((data_.bit_size() + 63) >> 6) * sizeof(uint64_t);
        }
        uint64_t positions_size = start_positions_.capacity() * sizeof(uint64_t);
        return std::make_tuple(data_size, positions_size, 0);
    }

    uint64_t total_length() const {
        return total_length_;
    }

    uint64_t page_residues() const { return page_residues_; }
    uint64_t page_count() const { return page_count_; }

    bool has_names() const {
        return !name_idx_.empty() && name_idx_.size() == n_;
    }

    const char* name(size_t i) const {
        if (i >= name_idx_.size()) throw std::out_of_range("name index out of range");
        return names_.data() + name_idx_[i];
    }

private:
    void sequence_bounds(size_t i, uint64_t& start_pos, uint64_t& length) const {
        if (i >= n_) throw std::out_of_range("sequence index out of range");
        start_pos = start_positions_[i];
        uint64_t end_pos = start_positions_[i + 1];
        length = end_pos - start_pos;
    }

    sdsl::int_vector<5> data_;                 // Legacy continuous 5-bit codes, used only while loading old files
    std::vector<uint8_t> pages_;               // Page-local 5-bit packed codes
    std::vector<uint64_t> start_positions_;    // start position of each sequence
    std::vector<char> names_;                  // sequence names (null-terminated)
    std::vector<uint64_t> name_idx_;           // start offsets for names_

    size_t n_ = 0;
    uint64_t total_length_ = 0;
    uint64_t page_residues_ = 6553;
    uint64_t packed_page_bytes_ = 4096;
    uint64_t page_count_ = 0;

    static uint64_t packed_bytes_for_residues(uint64_t residues) {
        return (residues * 5 + 7) / 8;
    }

    static uint64_t computed_page_count(uint64_t total_length, uint64_t page_residues) {
        if (page_residues == 0 || total_length == 0) return 0;
        return (total_length + page_residues - 1) / page_residues;
    }

    static uint64_t residues_in_page_for(uint64_t page_id,
                                         uint64_t page_count,
                                         uint64_t total_length,
                                         uint64_t page_residues) {
        if (page_id + 1 < page_count) return page_residues;
        uint64_t tail = total_length % page_residues;
        return tail == 0 ? page_residues : tail;
    }

    uint64_t residues_in_page(uint64_t page_id) const {
        return residues_in_page_for(page_id, page_count_, total_length_, page_residues_);
    }

    void ensure_page(uint64_t page_id) {
        if (page_id < page_count_) return;
        page_count_ = page_id + 1;
        pages_.resize(static_cast<size_t>(page_count_ * packed_page_bytes_), 0);
    }

    const uint8_t* page_data(uint64_t page_id) const {
        return pages_.data() + static_cast<size_t>(page_id * packed_page_bytes_);
    }

    uint8_t* page_data(uint64_t page_id) {
        return pages_.data() + static_cast<size_t>(page_id * packed_page_bytes_);
    }

    void append_sequence_to_pages(std::string_view seq) {
        size_t copied = 0;
        while (copied < seq.size()) {
            uint64_t page_id = total_length_ / page_residues_;
            uint64_t page_offset = total_length_ % page_residues_;
            ensure_page(page_id);

            uint64_t local_offset = page_offset;
            size_t space = static_cast<size_t>(page_residues_ - page_offset);
            size_t take = std::min(space, seq.size() - copied);
            append_ascii_5bit(page_data(page_id), local_offset, seq.data() + copied, take);
            copied += take;
            total_length_ += static_cast<uint64_t>(take);
        }
    }

    void decode_global_range(uint64_t offset, uint64_t length, char* dst) const {
        uint64_t done = 0;
        while (done < length) {
            uint64_t global = offset + done;
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            uint64_t available = residues_in_page(page_id) - page_offset;
            uint64_t take = std::min<uint64_t>(available, length - done);
            decode_packed_range(page_data(page_id), page_offset, take,
                                dst + static_cast<size_t>(done));
            done += take;
        }
    }

    void copy_global_codes_range(uint64_t offset, uint64_t length, uint8_t* dst) const {
        uint64_t done = 0;
        while (done < length) {
            uint64_t global = offset + done;
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            uint64_t available = residues_in_page(page_id) - page_offset;
            uint64_t take = std::min<uint64_t>(available, length - done);
            copy_packed_codes_range(page_data(page_id), page_offset, take,
                                    dst + static_cast<size_t>(done));
            done += take;
        }
    }

    uint64_t checksum_global_range(uint64_t offset, uint64_t length) const {
        uint64_t done = 0;
        uint64_t sum = 0;
        while (done < length) {
            uint64_t global = offset + done;
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            uint64_t available = residues_in_page(page_id) - page_offset;
            uint64_t take = std::min<uint64_t>(available, length - done);
            sum += checksum_packed_range(page_data(page_id), page_offset, take);
            done += take;
        }
        return sum;
    }

    template <typename T>
    static bool write_pod(std::ofstream& out, const T& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return static_cast<bool>(out);
    }

    template <typename T>
    static bool read_pod(std::ifstream& in, T& value) {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return static_cast<bool>(in);
    }

    bool write_meta(const std::string& path) const {
        std::ofstream out(path, std::ios::binary);
        if (!out) return false;

        char magic[8] = {'A', 'A', 'P', 'A', 'G', 'E', '3', '\0'};
        uint64_t version = 3;
        uint64_t seq_count = static_cast<uint64_t>(n_);

        out.write(magic, sizeof(magic));
        if (!write_pod(out, version) ||
            !write_pod(out, seq_count) ||
            !write_pod(out, total_length_) ||
            !write_pod(out, page_residues_) ||
            !write_pod(out, seq_count) ||
            !write_pod(out, page_count_)) {
            return false;
        }

        if (start_positions_.size() != n_ + 1) return false;
        if (!start_positions_.empty()) {
            out.write(reinterpret_cast<const char*>(start_positions_.data()),
                      static_cast<std::streamsize>(start_positions_.size() * sizeof(uint64_t)));
        }
        return static_cast<bool>(out);
    }

    bool read_meta(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        char magic[8] = {};
        uint64_t version = 0;
        uint64_t n = 0;
        uint64_t seq_count = 0;
        uint64_t page_count = 0;

        in.read(magic, sizeof(magic));
        if (std::strncmp(magic, "AAPAGE3", 7) != 0) return false;
        if (!read_pod(in, version) || version != 3) return false;
        if (!read_pod(in, n) ||
            !read_pod(in, total_length_) ||
            !read_pod(in, page_residues_) ||
            !read_pod(in, seq_count) ||
            !read_pod(in, page_count)) {
            return false;
        }
        if (seq_count != n || page_residues_ == 0) return false;

        n_ = static_cast<size_t>(n);
        page_count_ = page_count;
        packed_page_bytes_ = packed_bytes_for_residues(page_residues_);
        if (page_count_ != computed_page_count(total_length_, page_residues_)) return false;

        start_positions_.resize(n_ + 1);
        if (!start_positions_.empty()) {
            in.read(reinterpret_cast<char*>(start_positions_.data()),
                    static_cast<std::streamsize>(start_positions_.size() * sizeof(uint64_t)));
        }
        if (!in) return false;
        return validate_start_positions();
    }

    bool validate_start_positions() const {
        if (start_positions_.size() != n_ + 1) return false;
        if (start_positions_.empty() || start_positions_.front() != 0) return false;
        if (start_positions_.back() != total_length_) return false;
        for (size_t i = 1; i < start_positions_.size(); ++i) {
            if (start_positions_[i] < start_positions_[i - 1]) return false;
        }
        return true;
    }

    bool save_paged_directory(const std::string& output_dir) const {
        if (!validate_start_positions()) return false;

        namespace fs = std::filesystem;
        fs::create_directories(output_dir);
        fs::path dir(output_dir);
        fs::path pages_path = dir / "pages.bin";
        fs::path meta_path = dir / "meta.bin";

        std::ofstream pages_out(pages_path, std::ios::binary);
        if (!pages_out) return false;
        for (uint64_t page_id = 0; page_id < page_count_; ++page_id) {
            uint64_t bytes = packed_bytes_for_residues(residues_in_page(page_id));
            pages_out.write(reinterpret_cast<const char*>(page_data(page_id)),
                            static_cast<std::streamsize>(bytes));
            if (!pages_out) return false;
        }
        pages_out.close();
        if (!pages_out) return false;

        return write_meta(meta_path.string());
    }

    bool load_paged_directory(const std::string& input_dir) {
        namespace fs = std::filesystem;
        fs::path dir(input_dir);
        fs::path meta_path = dir / "meta.bin";
        fs::path pages_path = dir / "pages.bin";

        auto t0 = std::chrono::high_resolution_clock::now();
        if (!read_meta(meta_path.string())) return false;
        std::cerr << "Load: read paged meta & positions (" << start_positions_.size() << " entries) in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t0)
                         .count()
                  << " ms" << std::endl;

        auto t1 = std::chrono::high_resolution_clock::now();
        pages_.assign(static_cast<size_t>(page_count_ * packed_page_bytes_), 0);
        std::ifstream pages_in(pages_path, std::ios::binary);
        if (!pages_in) return false;
        for (uint64_t page_id = 0; page_id < page_count_; ++page_id) {
            uint64_t bytes = packed_bytes_for_residues(residues_in_page(page_id));
            pages_in.read(reinterpret_cast<char*>(page_data(page_id)),
                          static_cast<std::streamsize>(bytes));
            if (!pages_in) return false;
        }
        data_ = sdsl::int_vector<5>();
        std::cerr << "Load: read pages vector in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t1)
                         .count()
                  << " ms" << std::endl;
        return true;
    }

    bool convert_continuous_data_to_pages() {
        if (page_residues_ == 0) return false;
        page_count_ = computed_page_count(total_length_, page_residues_);
        packed_page_bytes_ = packed_bytes_for_residues(page_residues_);
        pages_.assign(static_cast<size_t>(page_count_ * packed_page_bytes_), 0);
        const uint8_t* src = reinterpret_cast<const uint8_t*>(data_.data());
        for (uint64_t global = 0; global < total_length_; ++global) {
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            set_packed_5bit(page_data(page_id), page_offset, packed_code_at(src, global));
        }
        return true;
    }

    sdsl::int_vector<5> to_continuous_int_vector() const {
        sdsl::int_vector<5> continuous(static_cast<size_t>(total_length_), 0);
        uint8_t* dst = reinterpret_cast<uint8_t*>(continuous.data());
        for (uint64_t global = 0; global < total_length_; ++global) {
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            set_packed_5bit(dst, global, packed_code_at(page_data(page_id), page_offset));
        }
        return continuous;
    }

    static inline uint8_t packed_code_at(const uint8_t* packed, uint64_t offset) {
        uint64_t bit_pos = offset * 5;
        size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
        int shift = static_cast<int>(bit_pos & 7);
        uint16_t value = static_cast<uint16_t>(packed[byte_pos] >> shift);
        if (shift > 3) {
            value |= static_cast<uint16_t>(packed[byte_pos + 1]) << (8 - shift);
        }
        return static_cast<uint8_t>(value & 0x1F);
    }

    static inline void set_packed_5bit(uint8_t* packed,
                                       uint64_t offset,
                                       uint8_t code) {
        uint64_t bit_pos = offset * 5;
        size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
        int shift = static_cast<int>(bit_pos & 7);
        uint16_t value = static_cast<uint16_t>(code & 0x1F);
        packed[byte_pos] |= static_cast<uint8_t>(value << shift);
        if (shift > 3) {
            packed[byte_pos + 1] |= static_cast<uint8_t>(value >> (8 - shift));
        }
    }

    static inline void append_ascii_5bit(uint8_t* packed,
                                         uint64_t& offset,
                                         const char* src,
                                         size_t len) {
        size_t pos = 0;
        while (pos < len && (offset & 7ULL) != 0) {
            set_packed_5bit(packed, offset, enc(src[pos]));
            ++offset;
            ++pos;
        }

        const auto kernel = aastore_detail::packed_5bit_encode_kernel();
#if defined(__x86_64__) || defined(__i386__)
        if (kernel == aastore_detail::Packed5BitEncodeKernel::Bmi2_32) {
            while (pos + 32 <= len) {
                size_t byte_pos = static_cast<size_t>((offset >> 3) * 5);
                aastore_detail::pack_32_ascii_5bit_bmi2(src + pos, packed + byte_pos);
                offset += 32;
                pos += 32;
            }
        }
#endif
        while (pos + 8 <= len) {
            size_t byte_pos = static_cast<size_t>((offset >> 3) * 5);
            if (kernel == aastore_detail::Packed5BitEncodeKernel::Bmi2_32) {
#if defined(__x86_64__) || defined(__i386__)
                aastore_detail::store_5_bytes_le(
                    packed + byte_pos,
                    aastore_detail::pack_8_ascii_5bit_bmi2_word(src + pos));
#else
                aastore_detail::pack_8_ascii_5bit_scalar(src + pos, packed + byte_pos);
#endif
            } else {
                aastore_detail::pack_8_ascii_5bit_scalar(src + pos, packed + byte_pos);
            }
            offset += 8;
            pos += 8;
        }

        while (pos < len) {
            set_packed_5bit(packed, offset, enc(src[pos]));
            ++offset;
            ++pos;
        }
    }

    static inline void clear_packed_append_region(uint8_t* packed,
                                                  uint64_t start,
                                                  uint64_t end) {
        if (end <= start) return;
        uint64_t bit_start = start * 5;
        uint64_t bit_end = end * 5;
        size_t byte_start = static_cast<size_t>(bit_start >> 3);
        size_t byte_end = static_cast<size_t>((bit_end + 7) >> 3);
        int keep_bits = static_cast<int>(bit_start & 7);
        if (keep_bits == 0) {
            packed[byte_start] = 0;
        } else {
            packed[byte_start] &= static_cast<uint8_t>((1U << keep_bits) - 1U);
        }
        if (byte_end > byte_start + 1) {
            std::memset(packed + byte_start + 1, 0, byte_end - byte_start - 1);
        }
    }

    static inline void decode_packed_range(const uint8_t* packed,
                                           uint64_t offset,
                                           uint64_t length,
                                           char* dst) {
        aastore_detail::decode_packed_5bit_range(
            packed, offset, length, dst);
    }

    static inline void copy_8_codes_aligned_packed(const uint8_t* packed,
                                                   uint64_t offset,
                                                   uint8_t* dst) {
        const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
        uint64_t x =
            static_cast<uint64_t>(src[0]) |
            (static_cast<uint64_t>(src[1]) << 8) |
            (static_cast<uint64_t>(src[2]) << 16) |
            (static_cast<uint64_t>(src[3]) << 24) |
            (static_cast<uint64_t>(src[4]) << 32);

        dst[0] = static_cast<uint8_t>(x & 0x1FULL);
        dst[1] = static_cast<uint8_t>((x >> 5) & 0x1FULL);
        dst[2] = static_cast<uint8_t>((x >> 10) & 0x1FULL);
        dst[3] = static_cast<uint8_t>((x >> 15) & 0x1FULL);
        dst[4] = static_cast<uint8_t>((x >> 20) & 0x1FULL);
        dst[5] = static_cast<uint8_t>((x >> 25) & 0x1FULL);
        dst[6] = static_cast<uint8_t>((x >> 30) & 0x1FULL);
        dst[7] = static_cast<uint8_t>((x >> 35) & 0x1FULL);
    }

    static inline void copy_packed_codes_range(const uint8_t* packed,
                                               uint64_t offset,
                                               uint64_t length,
                                               uint8_t* dst) {
        uint64_t pos = 0;
        while (pos < length && ((offset + pos) & 7ULL) != 0) {
            dst[static_cast<size_t>(pos)] = packed_code_at(packed, offset + pos);
            ++pos;
        }

        while (pos + 8 <= length) {
            copy_8_codes_aligned_packed(packed, offset + pos,
                                        dst + static_cast<size_t>(pos));
            pos += 8;
        }

        while (pos < length) {
            dst[static_cast<size_t>(pos)] = packed_code_at(packed, offset + pos);
            ++pos;
        }
    }

    static inline uint64_t sum_8_codes_aligned_packed(const uint8_t* packed,
                                                      uint64_t offset) {
        const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
        uint64_t x =
            static_cast<uint64_t>(src[0]) |
            (static_cast<uint64_t>(src[1]) << 8) |
            (static_cast<uint64_t>(src[2]) << 16) |
            (static_cast<uint64_t>(src[3]) << 24) |
            (static_cast<uint64_t>(src[4]) << 32);

        return (x & 0x1FULL) +
               ((x >> 5) & 0x1FULL) +
               ((x >> 10) & 0x1FULL) +
               ((x >> 15) & 0x1FULL) +
               ((x >> 20) & 0x1FULL) +
               ((x >> 25) & 0x1FULL) +
               ((x >> 30) & 0x1FULL) +
               ((x >> 35) & 0x1FULL);
    }

    static inline uint64_t checksum_packed_range(const uint8_t* packed,
                                                 uint64_t offset,
                                                 uint64_t length) {
        uint64_t pos = 0;
        uint64_t sum = 0;
        while (pos < length && ((offset + pos) & 7ULL) != 0) {
            sum += packed_code_at(packed, offset + pos);
            ++pos;
        }

        while (pos + 8 <= length) {
            sum += sum_8_codes_aligned_packed(packed, offset + pos);
            pos += 8;
        }

        while (pos < length) {
            sum += packed_code_at(packed, offset + pos);
            ++pos;
        }
        return sum;
    }

    void add_name(std::string_view name) {
        name_idx_.push_back(static_cast<uint64_t>(names_.size()));
        names_.insert(names_.end(), name.begin(), name.end());
        names_.push_back('\0');
    }
};

#endif
