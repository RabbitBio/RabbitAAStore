#include "ProteinAAStore.hpp"
#include "kseq.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <sys/stat.h>

// name is the name in kseq_t, comment is not included
struct Sequence {
    std::string name;
    std::string sequence;
};

// Helper function for kseq to read from FILE*
static inline size_t kseq_file_read(FILE* fp, void* buf, size_t size) {
    return fread(buf, 1, size, fp);
}

// Initialize kseq for FILE*
KSEQ_INIT(FILE*, kseq_file_read)

static uint64_t file_size_bytes(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return static_cast<uint64_t>(st.st_size);
    }
    return 0;
}

static uint64_t vector_memory_usage(const std::vector<Sequence>& sequences) {
    uint64_t total = 0;
    total += sequences.capacity() * sizeof(Sequence);
    for (const auto& s : sequences) {
        total += s.name.capacity() * sizeof(char);
        total += s.sequence.capacity() * sizeof(char);
    }
    return total;
}

static bool load_fasta_to_vector(const std::string& fasta_path,
                                 std::vector<Sequence>& sequences,
                                 uint64_t& total_len,
                                 std::chrono::milliseconds& duration) {
    FILE* fp = std::fopen(fasta_path.c_str(), "r");
    if (!fp) return false;

    kseq_t* seq = kseq_init(fp);
    if (!seq) {
        std::fclose(fp);
        return false;
    }

    auto start = std::chrono::high_resolution_clock::now();
    while (kseq_read(seq) >= 0) {
        Sequence s;
        s.name.assign(seq->name.s, seq->name.l);
        s.sequence.assign(seq->seq.s, seq->seq.l);
        total_len += seq->seq.l;
        sequences.emplace_back(std::move(s));
    }
    auto end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    kseq_destroy(seq);
    std::fclose(fp);
    return true;
}

static bool load_fasta_to_store(const std::string& fasta_path,
                                ProteinAAStore& store,
                                uint64_t& total_len,
                                std::chrono::milliseconds& duration) {
    FILE* fp = std::fopen(fasta_path.c_str(), "r");
    if (!fp) return false;

    kseq_t* seq = kseq_init(fp);
    if (!seq) {
        std::fclose(fp);
        return false;
    }

    store.init(file_size_bytes(fasta_path));
    auto start = std::chrono::high_resolution_clock::now();
    while (kseq_read(seq) >= 0) {
        std::string_view seq_view(seq->seq.s, seq->seq.l);
        std::string_view name_view(seq->name.s, seq->name.l);
        store.add(seq_view, name_view);
        total_len += seq->seq.l;
    }
    store.finalize();
    auto end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    kseq_destroy(seq);
    std::fclose(fp);
    return true;
}

static void print_result(const std::string& label,
                         uint64_t seq_count,
                         uint64_t total_len,
                         std::chrono::milliseconds duration,
                         uint64_t mem_bytes) {
    double secs = duration.count() / 1000.0;
    double mb = total_len / (1024.0 * 1024.0);
    double mb_per_sec = secs > 0 ? (mb / secs) : 0.0;
    std::cout << label << ":\n";
    std::cout << "  sequences: " << seq_count << "\n";
    std::cout << "  total_len: " << total_len << " bp\n";
    std::cout << "  time: " << duration.count() << " ms\n";
    std::cout << "  throughput: " << std::fixed << std::setprecision(2) << mb_per_sec << " MB/s\n";
    std::cout << "  memory: " << mem_bytes << " bytes ("
              << std::fixed << std::setprecision(2) << (mem_bytes / (1024.0 * 1024.0)) << " MB)\n";
}

static bool load_store_binary(const std::string& bin_path,
                              ProteinAAStore& store,
                              std::chrono::milliseconds& duration) {
    auto start = std::chrono::high_resolution_clock::now();
    if (!store.load(bin_path)) return false;
    auto end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    return true;
}

static bool load_store_names(const std::string& names_path,
                             ProteinAAStore& store,
                             std::chrono::milliseconds& duration,
                             const std::string& index_path = "") {
    auto start = std::chrono::high_resolution_clock::now();
    if (!store.load_names(names_path, index_path)) return false;
    auto end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    return true;
}

static void print_load_result(const std::string& label,
                              uint64_t seq_count,
                              uint64_t total_len,
                              std::chrono::milliseconds duration,
                              uint64_t mem_bytes,
                              uint64_t file_bytes) {
    double secs = duration.count() / 1000.0;
    double file_mb = file_bytes / (1024.0 * 1024.0);
    double mb_per_sec = secs > 0 ? (file_mb / secs) : 0.0;
    std::cout << label << ":\n";
    std::cout << "  sequences: " << seq_count << "\n";
    std::cout << "  total_len: " << total_len << " bp\n";
    std::cout << "  file_size: " << file_bytes << " bytes ("
              << std::fixed << std::setprecision(2) << file_mb << " MB)\n";
    std::cout << "  time: " << duration.count() << " ms\n";
    std::cout << "  throughput: " << std::fixed << std::setprecision(2) << mb_per_sec << " MB/s\n";
    std::cout << "  memory: " << mem_bytes << " bytes ("
              << std::fixed << std::setprecision(2) << (mem_bytes / (1024.0 * 1024.0)) << " MB)\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <fasta_file> <bin_path> [names_path]\n";
        std::cerr << "  fasta_file: FASTA file for parsing benchmark (kseq -> vector<Sequence>)\n";
        std::cerr << "  bin_path: AAStore binary file for load benchmark\n";
        std::cerr << "  names_path: AAStore names file for load benchmark (optional)\n";
        return 1;
    }

    std::string fasta_path = argv[1];
    std::string bin_path = argv[2];
    std::string names_path = (argc > 3) ? argv[3] : "";

    std::cout << "\n=== Load Benchmark ===\n";

    // FASTA -> vector<Sequence>
    {
        std::vector<Sequence> sequences;
        sequences.reserve(1024);
        uint64_t vec_total_len = 0;
        std::chrono::milliseconds vec_duration{0};
        if (!load_fasta_to_vector(fasta_path, sequences, vec_total_len, vec_duration)) {
            std::cerr << "Error: failed to load fasta into vector\n";
            return 1;
        }
        uint64_t vec_mem = vector_memory_usage(sequences);
        uint64_t fasta_size = file_size_bytes(fasta_path);
        print_load_result("FASTA -> vector<Sequence>", sequences.size(), vec_total_len, vec_duration, vec_mem, fasta_size);
    }

    // Binary -> ProteinAAStore
    { 
        ProteinAAStore store;
        std::chrono::milliseconds bin_duration{0};
        if (!load_store_binary(bin_path, store, bin_duration)) {
            std::cerr << "Error: failed to load AAStore binary from " << bin_path << "\n";
            return 1;
        }
        uint64_t bin_size = file_size_bytes(bin_path);
        uint64_t store_mem = store.memory_usage();

        // Load names if provided
        std::chrono::milliseconds names_duration{0};
        if (!names_path.empty()) {
            if (!load_store_names(names_path, store, names_duration)) {
                std::cerr << "Error: failed to load names from " << names_path << "\n";
                return 1;
            }
            store_mem += store.names_memory_usage();
        }

        uint64_t total_len = store.total_length();
        uint64_t total_file_size = bin_size + (names_path.empty() ? 0 : file_size_bytes(names_path) + file_size_bytes(names_path + ".idx"));
        std::chrono::milliseconds total_duration{bin_duration.count() + names_duration.count()};

        std::cout << "\nBinary -> ProteinAAStore:\n";
        std::cout << "  sequences: " << store.size() << "\n";
        std::cout << "  total_len: " << total_len << " bp\n";
        std::cout << "  bin_file_size: " << bin_size << " bytes ("
                  << std::fixed << std::setprecision(2) << (bin_size / (1024.0 * 1024.0)) << " MB)\n";
        std::cout << "  bin_load_time: " << bin_duration.count() << " ms ("
                  << std::fixed << std::setprecision(2) << (bin_size / (1024.0 * 1024.0)) / (bin_duration.count() / 1000.0 + 1e-9) << " MB/s)\n";
        if (!names_path.empty()) {
            uint64_t names_size = file_size_bytes(names_path);
            uint64_t idx_size = file_size_bytes(names_path + ".idx");
            std::cout << "  names_file_size: " << names_size << " bytes ("
                      << std::fixed << std::setprecision(2) << (names_size / (1024.0 * 1024.0)) << " MB)\n";
            std::cout << "  idx_file_size: " << idx_size << " bytes ("
                      << std::fixed << std::setprecision(2) << (idx_size / (1024.0 * 1024.0)) << " MB)\n";
            std::cout << "  names_load_time: " << names_duration.count() << " ms ("
                      << std::fixed << std::setprecision(2) << ((names_size + idx_size) / (1024.0 * 1024.0)) / (names_duration.count() / 1000.0 + 1e-9) << " MB/s)\n";
        }
        std::cout << "  total_load_time: " << total_duration.count() << " ms\n";
        std::cout << "  total_file_size: " << total_file_size << " bytes ("
                  << std::fixed << std::setprecision(2) << (total_file_size / (1024.0 * 1024.0)) << " MB)\n";
        std::cout << "  total_throughput: " << std::fixed << std::setprecision(2)
                  << (total_file_size / (1024.0 * 1024.0)) / (total_duration.count() / 1000.0 + 1e-9) << " MB/s\n";
        std::cout << "  memory: " << store_mem << " bytes ("
                  << std::fixed << std::setprecision(2) << (store_mem / (1024.0 * 1024.0)) << " MB)\n";
    
    }

    return 0;
}