#include "kseq.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

static uint64_t bytes_read = 0;

static inline size_t kseq_file_read(FILE* fp, void* buffer, size_t size) {
    size_t read_bytes = std::fread(buffer, 1, size, fp);
    bytes_read += static_cast<uint64_t>(read_bytes);
    return read_bytes;
}

KSEQ_INIT(FILE*, kseq_file_read)

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input.fa> [--drop-page-cache]"
                     " [--drop-only]"
                     " [--mode parse|names|copy|index|producer]\n";
        return 1;
    }

    const std::string input_path = argv[1];
    bool drop_page_cache = false;
    bool drop_only = false;
    std::string mode = "parse";
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--drop-page-cache") {
            drop_page_cache = true;
        } else if (arg == "--drop-only") {
            drop_page_cache = true;
            drop_only = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            return 1;
        }
    }
    if (mode != "parse" && mode != "names" && mode != "copy" &&
        mode != "index" && mode != "producer") {
        std::cerr << "Invalid mode: " << mode << "\n";
        return 1;
    }
    const bool do_names = mode == "names" || mode == "producer";
    const bool do_copy = mode == "copy" || mode == "producer";
    const bool do_index = mode == "index" || mode == "producer";

    FILE* fp = std::fopen(input_path.c_str(), "rb");
    if (!fp) {
        std::perror("failed to open FASTA");
        return 1;
    }

    uint64_t drop_page_cache_ms = 0;
#if defined(POSIX_FADV_DONTNEED)
    if (drop_page_cache) {
        auto drop_start = std::chrono::steady_clock::now();
        if (::posix_fadvise(::fileno(fp), 0, 0, POSIX_FADV_DONTNEED) != 0) {
            std::cerr << "Warning: posix_fadvise(POSIX_FADV_DONTNEED) failed\n";
        }
        drop_page_cache_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - drop_start)
                .count());
    }
#else
    if (drop_page_cache) {
        std::cerr << "Warning: POSIX_FADV_DONTNEED is unavailable\n";
    }
#endif

    if (drop_only) {
        std::fclose(fp);
        std::cout << "drop_page_cache=1\n";
        std::cout << "drop_page_cache_ms=" << drop_page_cache_ms << "\n";
        return 0;
    }

    kseq_t* seq = kseq_init(fp);
    if (!seq) {
        std::fclose(fp);
        std::cerr << "failed to initialize kseq\n";
        return 1;
    }

    bytes_read = 0;
    uint64_t sequence_count = 0;
    uint64_t total_residues = 0;
    uint64_t total_name_bytes = 0;
    uint64_t checksum = 0;
    uint64_t total_position = 0;

    constexpr size_t names_buffer_limit = 8ULL * 1024ULL * 1024ULL;
    constexpr size_t index_buffer_entries =
        (8ULL * 1024ULL * 1024ULL) / sizeof(uint64_t);
    constexpr uint64_t page_residues = 6553;
    constexpr size_t pages_per_task =
        (4ULL * 1024ULL * 1024ULL) / page_residues;
    constexpr size_t raw_buffer_limit =
        pages_per_task * static_cast<size_t>(page_residues);
    std::string names_buffer;
    std::vector<uint64_t> index_buffer;
    std::vector<char> raw_buffer;
    if (do_names) {
        names_buffer.reserve(names_buffer_limit);
    }
    if (do_index) {
        index_buffer.reserve(index_buffer_entries);
        index_buffer.push_back(0);
    }
    if (do_copy) {
        raw_buffer.reserve(raw_buffer_limit);
    }

    auto start = std::chrono::steady_clock::now();
    while (kseq_read(seq) >= 0) {
        ++sequence_count;
        total_residues += static_cast<uint64_t>(seq->seq.l);
        total_name_bytes += static_cast<uint64_t>(seq->name.l);
        if (seq->seq.l != 0) {
            checksum += static_cast<unsigned char>(seq->seq.s[0]);
            checksum += static_cast<unsigned char>(seq->seq.s[seq->seq.l - 1]);
        }
        if (do_names) {
            names_buffer.append(seq->name.s, seq->name.l);
            names_buffer.push_back('\n');
            if (names_buffer.size() >= names_buffer_limit) {
                names_buffer.clear();
            }
        }
        if (do_copy) {
            size_t copied = 0;
            while (copied < seq->seq.l) {
                if (raw_buffer.size() == raw_buffer_limit) {
                    raw_buffer.clear();
                }
                size_t take = std::min(
                    raw_buffer_limit - raw_buffer.size(),
                    static_cast<size_t>(seq->seq.l) - copied);
                raw_buffer.insert(raw_buffer.end(),
                                  seq->seq.s + copied,
                                  seq->seq.s + copied + take);
                copied += take;
            }
        }
        if (do_index) {
            total_position += static_cast<uint64_t>(seq->seq.l);
            index_buffer.push_back(total_position);
            if (index_buffer.size() >= index_buffer_entries) {
                index_buffer.clear();
            }
        }
        if (mode == "producer") {
            if (sequence_count % 10000 == 0) {
                checksum += bytes_read & 1ULL;
            }
        }
    }
    auto end = std::chrono::steady_clock::now();

    kseq_destroy(seq);
    std::fclose(fp);

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double elapsed_seconds = elapsed_ms / 1000.0;
    const double file_mib =
        static_cast<double>(bytes_read) / (1024.0 * 1024.0);
    const double residue_mib =
        static_cast<double>(total_residues) / (1024.0 * 1024.0);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "input=" << input_path << "\n";
    std::cout << "mode=" << mode << "\n";
    std::cout << "drop_page_cache=" << (drop_page_cache ? 1 : 0) << "\n";
    std::cout << "drop_page_cache_ms=" << drop_page_cache_ms << "\n";
    std::cout << "sequences=" << sequence_count << "\n";
    std::cout << "total_residues=" << total_residues << "\n";
    std::cout << "total_name_bytes=" << total_name_bytes << "\n";
    std::cout << "file_bytes_read=" << bytes_read << "\n";
    std::cout << "elapsed_ms=" << elapsed_ms << "\n";
    std::cout << "file_mib_per_sec="
              << (elapsed_seconds > 0.0 ? file_mib / elapsed_seconds : 0.0)
              << "\n";
    std::cout << "residue_mib_per_sec="
              << (elapsed_seconds > 0.0 ? residue_mib / elapsed_seconds : 0.0)
              << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return 0;
}
