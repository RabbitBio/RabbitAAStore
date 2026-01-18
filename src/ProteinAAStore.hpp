#ifndef __PROTEIN_AASTORE__
#define __PROTEIN_AASTORE__

#include <sdsl/int_vector.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <fstream>

class ProteinAAStore {
public:
	void init(uint64_t reserve_size)
	{
		data_.reserve(reserve_size);
	}
    // 5-bit codes:
    // 1..26 = letters A..Z or a..z (using bit operation: c & 0x1F)
    // Note: Lowercase (0b011...) and uppercase (0b010...) have same lower 5 bits
    static inline uint8_t enc(char c) {
        return uint8_t(c & 0x1F);  // Keep lower 5 bits
    }

    static inline char dec(uint8_t code) {
        return char(0x40 | code);  // Inverse operation of enc: reconstruct character from 5-bit code
    }

    // Build from in-memory sequences.
    void build(const std::vector<std::string_view>& seqs) {
        n_ = seqs.size();

        // total length = sum of all sequence lengths
        uint64_t total = 0;
        for (auto s : seqs) total += s.size();

        // 5 bits is enough for 0..31
        data_ = sdsl::int_vector<5>(total, 0);
        start_positions_.clear();
        start_positions_.reserve(seqs.size() + 1);

        uint64_t p = 0;
        for (size_t i = 0; i < seqs.size(); ++i) {
            start_positions_.push_back(p);
            auto s = seqs[i];
            for (char c : s) {
                data_[p++] = enc(c);
            }
        }
        // Record the end position of the last sequence
        start_positions_.push_back(p);
    }

    size_t size() const { return n_; }

    // Add a sequence after build
    void add(std::string_view seq) {
        uint64_t old_size = data_.size();
        uint64_t new_size = old_size + seq.size();

        // Resize data_ (only updates size if capacity is sufficient)
        data_.resize(new_size);

        // Record start position
        start_positions_.push_back(old_size);

        // Append sequence data
        uint64_t p = old_size;
        for (char c : seq) {
            data_[p++] = enc(c);
        }

        names_.emplace_back("");
        ++n_;
    }

    // Add a sequence with name after build
    void add(std::string_view seq, std::string_view name) {
        uint64_t old_size = data_.size();
        uint64_t new_size = old_size + seq.size();

        // Resize data_ (only updates size if capacity is sufficient)
        data_.resize(new_size);

        // Record start position
        start_positions_.push_back(old_size);

        // Append sequence data
        uint64_t p = old_size;
        for (char c : seq) {
            data_[p++] = enc(c);
        }

        names_.emplace_back(name);
        ++n_;
    }

    // Save names and name index (uint64_t offsets) to separate files.
    // Returns false if names are missing or file I/O fails.
    bool save_names(const std::string& names_path, const std::string& index_path = "") const {
        if (names_.empty() || names_.size() != n_) return false;
        std::string idx_path = index_path.empty() ? (names_path + ".idx") : index_path;

        std::ofstream names_out(names_path, std::ios::binary);
        if (!names_out) return false;
        std::ofstream index_out(idx_path, std::ios::binary);
        if (!index_out) return false;

        for (const auto& name : names_) {
            std::streampos pos = names_out.tellp();
            uint64_t offset = pos < 0 ? 0 : static_cast<uint64_t>(pos);
            index_out.write(reinterpret_cast<const char*>(&offset), sizeof(uint64_t));
            names_out << name << "\n";
        }
        return true;
    }

    // Load names and name index (uint64_t offsets) from separate files.
    // Returns false if file I/O fails or count mismatch with existing sequences.
    bool load_names(const std::string& names_path, const std::string& index_path = "") {
        std::string idx_path = index_path.empty() ? (names_path + ".idx") : index_path;

        std::ifstream index_in(idx_path, std::ios::binary | std::ios::ate);
        if (!index_in) return false;
        std::streamsize idx_size = index_in.tellg();
        if (idx_size < 0 || (idx_size % static_cast<std::streamsize>(sizeof(uint64_t))) != 0) return false;
        size_t count = static_cast<size_t>(idx_size / sizeof(uint64_t));
        index_in.seekg(0);

        std::vector<uint64_t> offsets(count);
        if (count > 0) {
            index_in.read(reinterpret_cast<char*>(offsets.data()), idx_size);
            if (!index_in) return false;
        }

        if (n_ != 0 && count != n_) return false;

        std::ifstream names_in(names_path, std::ios::binary);
        if (!names_in) return false;

        names_.clear();
        names_.resize(count);
        for (size_t i = 0; i < count; ++i) {
            names_in.seekg(static_cast<std::streamoff>(offsets[i]));
            if (!names_in) return false;
            std::string line;
            if (!std::getline(names_in, line)) return false;
            names_[i] = std::move(line);
        }
        return true;
    }

    // Finalize: record the end position of the last sequence
    void finalize() {
        if (!start_positions_.empty() && start_positions_.size() == n_) {
            start_positions_.push_back(data_.size());
        }
    }

    // Save to binary file
    bool save(const std::string& filepath) const {
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
        // write data_ via sdsl serialize
        data_.serialize(out);
        return true;
    }

    // Load from binary file
    bool load(const std::string& filepath) {
        std::ifstream in(filepath, std::ios::binary);
        if (!in) return false;
        auto t0 = std::chrono::high_resolution_clock::now();
        // read n_
        in.read(reinterpret_cast<char*>(&n_), sizeof(n_));
        // read start_positions_
        uint64_t pos_size = 0;
        in.read(reinterpret_cast<char*>(&pos_size), sizeof(pos_size));
        start_positions_.resize(pos_size);
        if (pos_size) {
            in.read(reinterpret_cast<char*>(start_positions_.data()), pos_size * sizeof(uint64_t));
        }
        std::cerr << "Load: read meta & positions (" << pos_size << " entries) in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t0)
                         .count()
                  << " ms" << std::endl;
        auto t1 = std::chrono::high_resolution_clock::now();
        // read data_
        data_.load(in);
        std::cerr << "Load: read data vector in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::high_resolution_clock::now() - t1)
                         .count()
                  << " ms" << std::endl;
        // basic validation
        if (start_positions_.size() != n_ + 1) return false;
        return true;
    }

    // Get i-th sequence (0-based)
    std::string get(size_t i) {
        if (i >= n_) throw std::out_of_range("sequence index out of range");
        
        uint64_t start_pos = start_positions_[i];
        uint64_t end_pos = start_positions_[i + 1];

        // decode [start_pos, end_pos)
        std::string out;
        out.reserve(end_pos - start_pos);

        for (uint64_t p = start_pos; p < end_pos; ++p) {
            uint8_t code = uint8_t(data_[p]);
            out.push_back(dec(code));
        }
        return out;
    }
    size_t get_raw(size_t i, std::vector<uint8_t>& out) {
        if (i >= n_) throw std::out_of_range("sequence index out of range");
        
        uint64_t start_pos = start_positions_[i];
        uint64_t end_pos = start_positions_[i + 1];

        // decode [start_pos, end_pos)
        out.resize(end_pos - start_pos);
        for (uint64_t i = 0; i < end_pos - start_pos; ++i) {
            out[i] = data_[start_pos + i];
        }
        return end_pos - start_pos;
    }
    // Length of i-th sequence
    uint64_t length(size_t i) {
        if (i >= n_) throw std::out_of_range("sequence index out of range");
        uint64_t start_pos = start_positions_[i];
        uint64_t end_pos = start_positions_[i + 1];
        return end_pos - start_pos;
    }

    // Compute a lightweight checksum of the encoded sequence without allocating strings.
    uint64_t checksum(size_t i) const {
        if (i >= n_) throw std::out_of_range("sequence index out of range");
        uint64_t start_pos = start_positions_[i];
        uint64_t end_pos = start_positions_[i + 1];
        uint64_t sum = 0;
        for (uint64_t p = start_pos; p < end_pos; ++p) {
            sum += static_cast<uint8_t>(data_[p]);
        }
        return sum;
    }

    // Calculate memory usage in bytes (actual memory allocated)
    uint64_t memory_usage() const {
        uint64_t total = 0;
        // Actual memory: (bit_size() + 63) / 64 * 8 bytes (aligned to 64-bit words)
        // This is the same as bit_data_size() * sizeof(uint64_t)
        total += ((data_.bit_size() + 63) >> 6) * sizeof(uint64_t);
        // Memory for start_positions_ vector
        total += start_positions_.capacity() * sizeof(uint64_t);
        return total;
    }

    // Get memory breakdown: returns {data_size, positions_size, unused}
    std::tuple<uint64_t, uint64_t, uint64_t> memory_breakdown() const {
        // Actual memory allocated: number of 64-bit words * 8 bytes
        // bit_data_size = (bit_size() + 63) >> 6
        uint64_t data_size = ((data_.bit_size() + 63) >> 6) * sizeof(uint64_t);
        uint64_t positions_size = start_positions_.capacity() * sizeof(uint64_t);
        return std::make_tuple(data_size, positions_size, 0);
    }

    uint64_t total_length() const {
        if (start_positions_.empty()) return 0;
        return start_positions_.back();
    }

    bool has_names() const {
        return !names_.empty() && names_.size() == n_;
    }

    const std::string& name(size_t i) const {
        if (i >= names_.size()) throw std::out_of_range("name index out of range");
        return names_[i];
    }

private:
    sdsl::int_vector<5> data_;                 // 5-bit packed codes
    std::vector<uint64_t> start_positions_;    // start position of each sequence
    std::vector<std::string> names_;           // sequence names (optional)

    size_t n_ = 0;
};

#endif
