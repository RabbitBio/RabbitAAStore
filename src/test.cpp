#include "ProteinAAStore.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>
#include <filesystem>
#include <random>

// Helper function for kseq to read from FILE*
static inline size_t kseq_file_read(FILE* fp, void* buf, size_t size) {
    return fread(buf, 1, size, fp);
}

// Initialize kseq for FILE*
KSEQ_INIT(FILE*, kseq_file_read)

int main(int argc, char** argv) {
    CLI::App app{"Build 5-bit protein sequence store from FASTA file"};
    
    std::string fasta_file;
    app.add_option("-f,--fasta", fasta_file, "Input FASTA file")
       ->check(CLI::ExistingFile);

    std::string save_path;
    app.add_option("-o,--output", save_path, "Save serialized store to file or directory (default protein_store.bin)");

    std::string load_path;
    app.add_option("-l,--load", load_path, "Load serialized store from file or directory (default protein_store.bin)");

    uint64_t min_length = 0;
    app.add_option("-L,--min-length", min_length, "Minimum sequence length filter (sequences shorter than this will be skipped, default 0 = no filtering)");

    CLI11_PARSE(app, argc, argv);

    auto resolve_path = [](const std::string& p) -> std::string {
        namespace fs = std::filesystem;
        fs::path path(p);
        if (fs::is_directory(path)) {
            return (path / "protein_store.bin").string();
        }
        return path.string();
    };
    
    ProteinAAStore store;
    uint64_t total_seq_len = 0;
    uint64_t num_seqs = 0;
    std::chrono::milliseconds duration_read{0};
    std::chrono::milliseconds duration_finalize{0};

    // For sequence validation
    std::vector<std::pair<size_t, std::string>> validation_samples;
    std::mt19937 validation_rng(42); // Fixed seed for reproducibility
    
    // Start timing
    auto start_total = std::chrono::high_resolution_clock::now();
    
    if (!load_path.empty()) {
        std::string resolved = resolve_path(load_path);
        std::cout << "Loading store from " << resolved << "..." << std::endl;
        auto start_load = std::chrono::high_resolution_clock::now();
        if (!store.load(resolved)) {
            std::cerr << "Error: failed to load from " << resolved << std::endl;
            return 1;
        }
        auto end_load = std::chrono::high_resolution_clock::now();
        num_seqs = store.size();
        total_seq_len = store.total_length();
        std::cout << "Loaded " << num_seqs << " sequences in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(end_load - start_load).count()
                  << " ms" << std::endl;
    } else {
        // Require fasta if not loading
        if (fasta_file.empty()) {
            std::cerr << "Error: either provide --fasta or --load" << std::endl;
            return 1;
        }
        // Open FASTA file
        FILE* fp = std::fopen(fasta_file.c_str(), "r");
        if (!fp) {
            std::cerr << "Error: Cannot open file " << fasta_file << std::endl;
            return 1;
        }
        
        kseq_t* seq = kseq_init(fp);
        if (!seq) {
            std::cerr << "Error: Failed to initialize kseq" << std::endl;
            std::fclose(fp);
            return 1;
        }
        
        // Get file size for progress bar
        struct stat file_stat;
        uint64_t file_size = 0;
        if (stat(fasta_file.c_str(), &file_stat) == 0) {
            file_size = file_stat.st_size;
        } else {
            std::cerr << "Warning: Cannot get file size, progress bar will show sequence count only" << std::endl;
        }
        
        store.init(file_size);
        std::cout << "Reading sequences from " << fasta_file << "..." << std::endl;
        
        // Helper function to draw progress bar based on file bytes read
        auto draw_progress = [file_size](uint64_t bytes_read, uint64_t seqs_processed, uint64_t width = 50) {
            std::cout << "\r[";
            double percent = 0.0;
            if (file_size > 0) {
                percent = (double)bytes_read / file_size;
                if (percent > 1.0) percent = 1.0;  // Cap at 100%
            }
            uint64_t filled = (uint64_t)(width * percent);
            for (uint64_t i = 0; i < filled; ++i) {
                std::cout << "=";
            }
            if (filled < width) {
                std::cout << ">";
                for (uint64_t i = filled + 1; i < width; ++i) {
                    std::cout << " ";
                }
            }
            std::cout << "] ";
            if (file_size > 0) {
                std::cout << std::fixed << std::setprecision(1) << (percent * 100.0) << "% ";
            }
            std::cout << "(" << seqs_processed << " sequences)";
            std::cout.flush();
        };
        
        // Read sequences and build store
        auto start_read = std::chrono::high_resolution_clock::now();
        uint64_t skipped_seqs = 0;
        while (kseq_read(seq) >= 0) {
            if (seq->seq.l > 0) {
                // Filter sequences shorter than minimum length
                if (seq->seq.l < min_length) {
                    skipped_seqs++;
                    continue;
                }

                std::string_view seq_view(seq->seq.s, seq->seq.l);
                store.add(seq_view);
                total_seq_len += seq->seq.l;

                // Sample some sequences for validation (sample rate: ~0.1% or at least 10 samples)
                if (validation_samples.size() < 10 || (validation_rng() % 1000) == 0) {
                    validation_samples.emplace_back(num_seqs, std::string(seq_view));
                }

                num_seqs++;
                
                // Get current file position for progress bar (approximate due to buffering)
                long current_pos = ftell(fp);
                if (current_pos < 0) current_pos = 0;
                uint64_t bytes_read = (uint64_t)current_pos;
                
                // Update progress bar (update every 100 sequences or when near end)
                if (num_seqs % 100 == 0 || (file_size > 0 && bytes_read >= file_size * 0.99)) {
                    draw_progress(bytes_read, num_seqs);
                }
            }
        }
        // Final progress bar update
        long final_pos = ftell(fp);
        if (final_pos < 0 && file_size > 0) {
            final_pos = file_size;
        } else if (final_pos < 0) {
            final_pos = 0;
        }
        draw_progress((uint64_t)final_pos, num_seqs);
        std::cout << std::endl;  // Move to next line after completion
        auto end_read = std::chrono::high_resolution_clock::now();
        duration_read = std::chrono::duration_cast<std::chrono::milliseconds>(end_read - start_read);
        
        // Finalize to record the end position of the last sequence
        std::cout << "Finalizing..." << std::endl;
        auto start_finalize = std::chrono::high_resolution_clock::now();
        store.finalize();
        auto end_finalize = std::chrono::high_resolution_clock::now();
        duration_finalize = std::chrono::duration_cast<std::chrono::milliseconds>(end_finalize - start_finalize);

        std::cout << "Read " << num_seqs << " sequences";
        if (min_length > 0) {
            std::cout << " (skipped " << skipped_seqs << " sequences shorter than " << min_length << " characters)";
        }
        std::cout << std::endl;

        // Validate sequence integrity
        if (!validation_samples.empty()) {
            std::cout << "Validating " << validation_samples.size() << " sequences..." << std::endl;
            bool all_valid = true;
            for (const auto& [idx, original] : validation_samples) {
                std::string retrieved = store.get(idx);
                if (retrieved != original) {
                    std::cerr << "ERROR: Sequence " << idx << " mismatch!" << std::endl;
                    std::cerr << "  Original  (" << original.size() << " chars): " << original.substr(0, 50) << (original.size() > 50 ? "..." : "") << std::endl;
                    std::cerr << "  Retrieved (" << retrieved.size() << " chars): " << retrieved.substr(0, 50) << (retrieved.size() > 50 ? "..." : "") << std::endl;
                    all_valid = false;
                }
            }
            if (all_valid) {
                std::cout << "✓ All " << validation_samples.size() << " validation sequences match correctly." << std::endl;
            } else {
                std::cerr << "✗ Sequence validation FAILED!" << std::endl;
                return 1;
            }
        }

        kseq_destroy(seq);
        std::fclose(fp);
        
        // Optionally save
        if (!save_path.empty()) {
            std::string resolved = resolve_path(save_path);
            auto start_save = std::chrono::high_resolution_clock::now();
            if (!store.save(resolved)) {
                std::cerr << "Warning: failed to save to " << resolved << std::endl;
            } else {
                auto end_save = std::chrono::high_resolution_clock::now();
                std::cout << "Saved store to " << resolved << " in "
                          << std::chrono::duration_cast<std::chrono::milliseconds>(end_save - start_save).count()
                          << " ms" << std::endl;
            }
        }
    }
    
    // Calculate and display statistics
    auto start_mem = std::chrono::high_resolution_clock::now();
    uint64_t mem_bytes = store.memory_usage();
    auto end_mem = std::chrono::high_resolution_clock::now();
    auto duration_mem = std::chrono::duration_cast<std::chrono::milliseconds>(end_mem - start_mem);
    
    auto end_total = std::chrono::high_resolution_clock::now();
    auto duration_total = std::chrono::duration_cast<std::chrono::milliseconds>(end_total - start_total);
    uint64_t mem_kb = mem_bytes / 1024;
    uint64_t mem_mb = mem_bytes / (1024 * 1024);

    // If loaded and need to save to another location
    if (!load_path.empty() && !save_path.empty()) {
        std::string resolved = resolve_path(save_path);
        auto start_save = std::chrono::high_resolution_clock::now();
        if (!store.save(resolved)) {
            std::cerr << "Warning: failed to save to " << resolved << std::endl;
        } else {
            auto end_save = std::chrono::high_resolution_clock::now();
            std::cout << "Saved store to " << resolved << " in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(end_save - start_save).count()
                      << " ms" << std::endl;
        }
    }
    
    std::cout << "\n=== Statistics ===" << std::endl;
    std::cout << "Number of sequences: " << store.size() << std::endl;
    std::cout << "Total sequence length: " << total_seq_len << " characters" << std::endl;
    std::cout << "Average sequence length: " << (num_seqs > 0 ? total_seq_len / num_seqs : 0) << " characters" << std::endl;
    std::cout << "\n=== Memory Usage ===" << std::endl;
    std::cout << "Total memory: " << mem_bytes << " bytes";
    if (mem_kb > 0) {
        std::cout << " (" << mem_kb << " KB";
        if (mem_mb > 0) {
            std::cout << ", " << mem_mb << " MB";
        }
        std::cout << ")";
    }
    std::cout << std::endl;
    
    // Calculate compression ratio and memory breakdown
    double original_size = total_seq_len;  // 1 byte per character (original FASTA sequence data)
    double memory_ratio = original_size > 0 ? (double)mem_bytes / original_size : 0.0;
    
    // Get memory breakdown
    auto [data_size, positions_size, unused] = store.memory_breakdown();
    
    std::cout << "\n=== Memory Breakdown ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Original input size: " << original_size << " bytes (" 
              << (original_size / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "Data vector (5-bit encoded): " << data_size << " bytes (" 
              << (data_size / 1024.0 / 1024.0) << " MB, " 
              << (original_size > 0 ? (data_size * 100.0 / original_size) : 0.0) << "% of original)" << std::endl;
    std::cout << "Start positions vector: " << positions_size << " bytes (" 
              << (positions_size / 1024.0 / 1024.0) << " MB, " 
              << (original_size > 0 ? (positions_size * 100.0 / original_size) : 0.0) << "% of original)" << std::endl;
    
    std::cout << "\n=== Memory Ratio ===" << std::endl;
    std::cout << "Total memory / Original size: " << memory_ratio << "x" << std::endl;
    std::cout << "Memory overhead: " << (memory_ratio - 1.0) * 100.0 << "%" << std::endl;
    
    // Theoretical estimate: 5-bit encoding (0.625) + start positions overhead
    // Start positions: 8 bytes per sequence (uint64_t)
    double theoretical_min = 0.625;  // 5-bit data (0.625)
    std::cout << "Theoretical minimum (5-bit data only): ~" << theoretical_min << "x" << std::endl;
    std::cout << "Actual overhead above theoretical min: " << ((memory_ratio - theoretical_min) * 100.0) << "%" << std::endl;
    
    // Display timing information
    std::cout << "\n=== Timing ===" << std::endl;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "Reading and adding sequences: " << duration_read.count() << " ms ("
              << duration_read.count() / 1000.0 << " s)" << std::endl;
    std::cout << "Finalizing: " << duration_finalize.count() << " ms ("
              << duration_finalize.count() / 1000.0 << " s)" << std::endl;
    std::cout << "Memory calculation: " << duration_mem.count() << " ms ("
              << duration_mem.count() / 1000.0 << " s)" << std::endl;
    std::cout << "Total time: " << duration_total.count() << " ms ("
              << duration_total.count() / 1000.0 << " s)" << std::endl;

    // Benchmark get(): random subset to avoid full copy of all sequences
    if (store.size() > 0) {
        size_t sample_count = std::min<size_t>(10000, store.size());
        std::mt19937_64 rng(12345);
        std::uniform_int_distribution<size_t> dist(0, store.size() - 1);
        std::vector<size_t> sample_idx;
        sample_idx.reserve(sample_count);
        for (size_t i = 0; i < sample_count; ++i) sample_idx.push_back(dist(rng));

        auto start_get = std::chrono::high_resolution_clock::now();
        uint64_t checksum_enc = 0;
        uint64_t total_chars_get = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            size_t idx = sample_idx[i];
            checksum_enc += store.checksum(idx);      // touch data (5-bit codes)
            total_chars_get += store.length(idx);     // count decoded chars
        }
        auto end_get = std::chrono::high_resolution_clock::now();
        auto duration_get = std::chrono::duration_cast<std::chrono::milliseconds>(end_get - start_get);
        std::cout << "Random get() over " << sample_count << " sequences (checksum only, no string alloc): "
                  << duration_get.count() << " ms (" << duration_get.count() / 1000.0 << " s), "
                  << "checksum_enc=" << checksum_enc << ", chars=" << total_chars_get << std::endl;

        // Equivalent memory bandwidth for get() (approximate, counts decoded bytes)
        double gb_per_sec_get = (total_chars_get / (1024.0 * 1024.0 * 1024.0)) / (duration_get.count() / 1000.0 + 1e-9);
        std::cout << "Memory bandwidth (get path): " << gb_per_sec_get * 1024.0 << " MB/s ("
                  << gb_per_sec_get << " GB/s)" << std::endl;
    }
    
    // Performance metrics
    if (duration_read.count() > 0) {
        double seqs_per_sec = (num_seqs * 1000.0) / duration_read.count();
        double mb_per_sec = (total_seq_len / (1024.0 * 1024.0)) / (duration_read.count() / 1000.0);
        std::cout << "\n=== Performance ===" << std::endl;
        std::cout << "Sequences per second: " << std::setprecision(0) << seqs_per_sec << std::endl;
        std::cout << std::setprecision(2) << "Throughput: " << mb_per_sec << " MB/s" << std::endl;

        // Memory access bandwidth during reading (approximate)
        double gb_per_sec = (total_seq_len / (1024.0 * 1024.0 * 1024.0)) / (duration_read.count() / 1000.0);
        std::cout << "Memory bandwidth (read path): " << gb_per_sec * 1024.0 << " MB/s (" << gb_per_sec << " GB/s)" << std::endl;
    }
    
    return 0;
}

