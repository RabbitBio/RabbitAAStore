#include "ProteinAAStore.hpp"
#include "PagedProteinAAStore.hpp"
#include "CLI11.hpp"
#include "kseq.h"

#include <iostream>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>
#include <filesystem>
#include <random>
#include <memory>
#include <sstream>
#include <string_view>
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
    app.add_option("-o,--output", save_path, "Save paged store directory containing meta.bin and pages.bin");

    std::string load_path;
    app.add_option("-l,--load", load_path, "Load paged store directory, or legacy single-file ProteinAAStore");

    std::string names_output;
    app.add_option("-n,--names-output", names_output, "Output file to save sequence names (one per line, without '>'). Default: protein_names.txt next to output store or in current directory");

    std::string names_index_output;
    app.add_option("--names-index-output", names_index_output, "Output file to save name index offsets (uint64_t per line). Default: protein_names.txt.idx next to names file");

    std::string names_load;
    app.add_option("--names-load", names_load, "Load sequence names file (one per line, without '>'). Optional");

    std::string names_index_load;
    app.add_option("--names-index-load", names_index_load, "Load name index offsets file (uint64_t per entry). Optional; default is names file + .idx");

    uint64_t min_length = 0;
    app.add_option("-L,--min-length", min_length, "Minimum sequence length filter (sequences shorter than this will be skipped, default 0 = no filtering)");

    uint64_t build_threads = 1;
    app.add_option("-j,--threads", build_threads, "Worker threads for FASTA page packing when -o is set, default 1; 0 uses hardware concurrency");

    CLI11_PARSE(app, argc, argv);

    auto resolve_path = [](const std::string& p) -> std::string {
        return p;
    };

    uint64_t page_residues = ProteinAAStore::default_page_residues();
    
    ProteinAAStore store;
    store.set_page_residues(page_residues);
    uint64_t total_seq_len = 0;
    uint64_t num_seqs = 0;
    std::chrono::milliseconds duration_read{0};
    std::chrono::milliseconds duration_finalize{0};
    std::chrono::milliseconds duration_names_load{0};
    std::chrono::milliseconds duration_load_built_store{0};

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

        if (!names_load.empty() || !names_index_load.empty()) {
            std::string names_load_path = names_load;
            if (names_load_path.empty()) {
                const std::string suffix = ".idx";
                if (names_index_load.size() >= suffix.size() &&
                    names_index_load.rfind(suffix) == names_index_load.size() - suffix.size()) {
                    names_load_path = names_index_load.substr(0, names_index_load.size() - suffix.size());
                } else {
                    std::cerr << "Error: --names-load is required when --names-index-load is not a .idx file" << std::endl;
                    return 1;
                }
            }
            std::string names_index_load_path = names_index_load;
            auto start_names_load = std::chrono::high_resolution_clock::now();
            if (!store.load_names(names_load_path, names_index_load_path)) {
                std::cerr << "Error: failed to load names from " << names_load_path << std::endl;
                return 1;
            }
            auto end_names_load = std::chrono::high_resolution_clock::now();
            duration_names_load = std::chrono::duration_cast<std::chrono::milliseconds>(end_names_load - start_names_load);
            std::cout << "Loaded names from " << names_load_path << " in "
                      << duration_names_load.count() << " ms" << std::endl;
        }
    } else {
        // Require fasta if not loading
        if (fasta_file.empty()) {
            std::cerr << "Error: either provide --fasta or --load" << std::endl;
            return 1;
        }
        if (!save_path.empty()) {
            try {
                std::filesystem::create_directories(resolve_path(save_path));
            } catch (...) {
                std::cerr << "Error: failed to create output directory " << save_path << std::endl;
                return 1;
            }
        }

        // Open FASTA file
        FILE* fp = std::fopen(fasta_file.c_str(), "r");
        if (!fp) {
            std::cerr << "Error: Cannot open file " << fasta_file << std::endl;
            return 1;
        }

        std::string names_path;
        if (!names_output.empty()) {
            names_path = names_output;
        } else if (!save_path.empty()) {
            namespace fs = std::filesystem;
            fs::path store_path(resolve_path(save_path));
            names_path = (store_path / "seq_names.txt").string();
        } else {
            names_path = "protein_names.txt";
        }
        std::string names_index_path;
        if (!names_index_output.empty()) {
            names_index_path = names_index_output;
        } else {
            names_index_path = names_path + ".idx";
        }
        std::cout << "Writing sequence names to " << names_path << "..." << std::endl;
        std::cout << "Writing name index to " << names_index_path << "..." << std::endl;
        
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
        
        std::cout << "Reading sequences from " << fasta_file << "..." << std::endl;
        
        // Helper function to draw progress bar based on file bytes read
        auto draw_progress = [file_size](uint64_t bytes_read, uint64_t seqs_processed, uint64_t width = 50) {
            std::ios::fmtflags old_flags = std::cout.flags();
            std::streamsize old_precision = std::cout.precision();
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
            std::cout.flags(old_flags);
            std::cout.precision(old_precision);
        };
        
        // Read sequences and build store
        auto start_read = std::chrono::high_resolution_clock::now();
        uint64_t skipped_seqs = 0;

        auto update_progress = [&](bool force = false) {
            long current_pos = ftell(fp);
            if (current_pos < 0) current_pos = 0;
            uint64_t bytes_read = static_cast<uint64_t>(current_pos);
            if (force || num_seqs % 10000 == 0 || (file_size > 0 && bytes_read >= file_size * 0.99)) {
                draw_progress(bytes_read, num_seqs);
            }
        };

        PagedProteinAAStore::BuildStats build_stats;
        uint64_t names_write_ns = 0;
        uint64_t names_index_write_ns = 0;
        uint64_t names_bytes = 0;
        uint64_t names_flushes = 0;
        uint64_t names_index_flushes = 0;

        if (!save_path.empty()) {
            std::ofstream names_out(names_path, std::ios::binary);
            std::ofstream names_index_out(names_index_path, std::ios::binary);
            if (!names_out || !names_index_out) {
                std::cerr << "Error: failed to open names outputs" << std::endl;
                kseq_destroy(seq);
                std::fclose(fp);
                return 1;
            }

            const size_t names_buffer_limit = 8ULL * 1024ULL * 1024ULL;
            const size_t index_buffer_limit = 1ULL * 1024ULL * 1024ULL;
            std::string names_buffer;
            std::vector<uint64_t> name_index_buffer;
            names_buffer.reserve(names_buffer_limit);
            name_index_buffer.reserve(index_buffer_limit / sizeof(uint64_t));

            auto flush_names = [&]() -> bool {
                if (names_buffer.empty()) return true;
                auto write_start = std::chrono::steady_clock::now();
                names_out.write(names_buffer.data(), static_cast<std::streamsize>(names_buffer.size()));
                names_write_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - write_start).count());
                ++names_flushes;
                names_buffer.clear();
                return static_cast<bool>(names_out);
            };

            auto flush_name_index = [&]() -> bool {
                if (name_index_buffer.empty()) return true;
                auto write_start = std::chrono::steady_clock::now();
                names_index_out.write(reinterpret_cast<const char*>(name_index_buffer.data()),
                                      static_cast<std::streamsize>(
                                          name_index_buffer.size() * sizeof(uint64_t)));
                names_index_write_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - write_start).count());
                ++names_index_flushes;
                name_index_buffer.clear();
                return static_cast<bool>(names_index_out);
            };

            auto append_name = [&](const char* name, size_t name_len) -> bool {
                name_index_buffer.push_back(names_bytes);
                names_buffer.append(name, name_len);
                names_buffer.push_back('\n');
                names_bytes += static_cast<uint64_t>(name_len + 1);
                if (names_buffer.size() >= names_buffer_limit && !flush_names()) return false;
                if (name_index_buffer.size() * sizeof(uint64_t) >= index_buffer_limit &&
                    !flush_name_index()) {
                    return false;
                }
                return true;
            };

            bool names_write_error = false;
            auto next_sequence = [&](std::string_view& out) -> bool {
                while (kseq_read(seq) >= 0) {
                    if (seq->seq.l == 0) {
                        continue;
                    }
                    if (seq->seq.l < min_length) {
                        ++skipped_seqs;
                        update_progress();
                        continue;
                    }

                    std::string_view seq_view(seq->seq.s, seq->seq.l);
                    if (!append_name(seq->name.s, seq->name.l)) {
                        names_write_error = true;
                        return false;
                    }

                    if (validation_samples.size() < 10 || (validation_rng() % 1000) == 0) {
                        validation_samples.emplace_back(num_seqs, std::string(seq_view));
                    }

                    total_seq_len += seq->seq.l;
                    ++num_seqs;
                    update_progress();
                    out = seq_view;
                    return true;
                }
                return false;
            };

            bool ok = PagedProteinAAStore::build_from_sequence_views_parallel(
                resolve_path(save_path), page_residues, next_sequence,
                static_cast<size_t>(build_threads), &build_stats);

            if (!flush_names()) names_write_error = true;
            if (!flush_name_index()) names_write_error = true;
            names_out.close();
            names_index_out.close();
            if (!names_out || !names_index_out) names_write_error = true;

            if (!ok || names_write_error) {
                std::cerr << "Error: failed to stream-build paged AAStore from FASTA" << std::endl;
                kseq_destroy(seq);
                std::fclose(fp);
                return 1;
            }
        } else {
            store.init(file_size);
            while (kseq_read(seq) >= 0) {
                if (seq->seq.l > 0) {
                    // Filter sequences shorter than minimum length
                    if (seq->seq.l < min_length) {
                        skipped_seqs++;
                        continue;
                    }

                    std::string_view seq_view(seq->seq.s, seq->seq.l);
                    store.add(seq_view, seq->name.s);
                    total_seq_len += seq->seq.l;

                    // Sample some sequences for validation (sample rate: ~0.1% or at least 10 samples)
                    if (validation_samples.size() < 10 || (validation_rng() % 1000) == 0) {
                        validation_samples.emplace_back(num_seqs, std::string(seq_view));
                    }

                    num_seqs++;
                    update_progress();
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
        kseq_destroy(seq);
        std::fclose(fp);
        
        // Finalize to record the end position of the last sequence
        std::cout << "Finalizing..." << std::endl;
        auto start_finalize = std::chrono::high_resolution_clock::now();
        if (save_path.empty()) {
            store.finalize();
        } else {
            std::string resolved = resolve_path(save_path);
            auto load_built_start = std::chrono::high_resolution_clock::now();
            if (!store.load(resolved)) {
                std::cerr << "Error: failed to load freshly built store from " << resolved << std::endl;
                return 1;
            }
            duration_load_built_store = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - load_built_start);
            if (!store.load_names(names_path, names_index_path)) {
                std::cerr << "Warning: failed to load names from " << names_path << std::endl;
            }
        }
        auto end_finalize = std::chrono::high_resolution_clock::now();
        duration_finalize = std::chrono::duration_cast<std::chrono::milliseconds>(end_finalize - start_finalize);

        std::cout << "Read " << num_seqs << " sequences";
        if (min_length > 0) {
            std::cout << " (skipped " << skipped_seqs << " sequences shorter than " << min_length << " characters)";
        }
        std::cout << std::endl;

        // Save names and name index in in-memory build mode.
        if (save_path.empty()) {
            if (!store.save_names(names_path, names_index_path)) {
                std::cerr << "Error: failed to save names to " << names_path
                          << " (index: " << names_index_path << ")" << std::endl;
                return 1;
            }
        }

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

        if (!save_path.empty()) {
            std::cout << "Built store directory at " << std::filesystem::absolute(save_path).string()
                      << " in " << duration_read.count() << " ms" << std::endl;
            std::cout << "Build threads: " << build_threads << std::endl;
            std::cout << "Build read_ms_est: " << (build_stats.read_ns / 1000000.0) << std::endl;
            std::cout << "Build raw_copy_ms_est: " << (build_stats.raw_copy_ns / 1000000.0) << std::endl;
            std::cout << "Build producer_ms: " << (build_stats.producer_ns / 1000000.0) << std::endl;
            std::cout << "Build enqueue_wait_ms: " << (build_stats.enqueue_wait_ns / 1000000.0) << std::endl;
            std::cout << "Build pack_worker_ms_total: " << (build_stats.pack_ns / 1000000.0) << std::endl;
            std::cout << "Build write_ms: " << (build_stats.write_ns / 1000000.0) << std::endl;
            std::cout << "Build index_write_ms: " << (build_stats.index_write_ns / 1000000.0) << std::endl;
            std::cout << "Build drain_ms: " << (build_stats.drain_ns / 1000000.0) << std::endl;
            std::cout << "Build finalize_ms: " << (build_stats.finalize_ns / 1000000.0) << std::endl;
            std::cout << "Build index_bytes: " << build_stats.index_bytes << std::endl;
            std::cout << "Build index_flushes: " << build_stats.index_flushes << std::endl;
            std::cout << "Build index_buffer_bytes: " << build_stats.index_buffer_bytes << std::endl;
            std::cout << "Build raw_bytes: " << build_stats.raw_bytes << std::endl;
            std::cout << "Build packed_bytes: " << build_stats.packed_bytes << std::endl;
            std::cout << "Build pages: " << build_stats.pages << std::endl;
            std::cout << "Build tasks: " << build_stats.tasks << std::endl;
            std::cout << "Names write_ms: " << (names_write_ns / 1000000.0) << std::endl;
            std::cout << "Names index_write_ms: " << (names_index_write_ns / 1000000.0) << std::endl;
            std::cout << "Names bytes: " << names_bytes << std::endl;
            std::cout << "Names flushes: " << names_flushes << std::endl;
            std::cout << "Names index_flushes: " << names_index_flushes << std::endl;
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
            std::cout << "Saved store directory to " << resolved << " in "
                      << std::chrono::duration_cast<std::chrono::milliseconds>(end_save - start_save).count()
                      << " ms" << std::endl;
        }
    }
    
    std::cout << "\n=== Statistics ===" << std::endl;
    std::cout << "Number of sequences: " << store.size() << std::endl;
    std::cout << "Total sequence length: " << total_seq_len << " characters" << std::endl;
    std::cout << "Average sequence length: " << (num_seqs > 0 ? total_seq_len / num_seqs : 0) << " characters" << std::endl;
    uint64_t packed_page_bytes = (store.page_residues() * 5 + 7) / 8;
    std::cout << "Packed page bytes: " << packed_page_bytes << std::endl;
    std::cout << "Page residues: " << store.page_residues() << std::endl;
    std::cout << "Page count: " << store.page_count() << std::endl;
    if (store.has_names() && store.size() > 0) {
        std::cout << "First name: " << store.name(0) << std::endl;
        std::cout << "Last name: " << store.name(store.size() - 1) << std::endl;
    }
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
    std::cout << "Pages vector (5-bit encoded): " << data_size << " bytes ("
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
    if (duration_load_built_store.count() > 0) {
        std::cout << "Loading built store for validation/statistics: "
                  << duration_load_built_store.count() << " ms ("
                  << duration_load_built_store.count() / 1000.0 << " s)" << std::endl;
    }
    if (duration_names_load.count() > 0) {
        std::cout << "Loading names/index: " << duration_names_load.count() << " ms ("
                  << duration_names_load.count() / 1000.0 << " s)" << std::endl;
    }
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

        auto start_random_get = std::chrono::high_resolution_clock::now();
        uint64_t checksum_enc_random = 0;
        uint64_t total_chars_random_get = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            size_t idx = sample_idx[i];
            checksum_enc_random += store.checksum(idx);      // touch data (5-bit codes)
            total_chars_random_get += store.length(idx);     // count decoded chars
        }
        auto end_random_get = std::chrono::high_resolution_clock::now();
        auto duration_random_get = std::chrono::duration_cast<std::chrono::milliseconds>(end_random_get - start_random_get);
        std::cout << "Random get() over " << sample_count << " sequences (no sort, checksum only, no string alloc): "
                  << duration_random_get.count() << " ms (" << duration_random_get.count() / 1000.0 << " s), "
                  << "checksum_enc=" << checksum_enc_random << ", chars=" << total_chars_random_get << std::endl;

        double gb_per_sec_random_get = (total_chars_random_get / (1024.0 * 1024.0 * 1024.0)) / (duration_random_get.count() / 1000.0 + 1e-9);
        std::cout << "Memory bandwidth (random get path): " << gb_per_sec_random_get * 1024.0 << " MB/s ("
                  << gb_per_sec_random_get << " GB/s)" << std::endl;

        auto start_random_real_get = std::chrono::high_resolution_clock::now();
        uint64_t checksum_dec_random = 0;
        uint64_t total_chars_random_real_get = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            size_t idx = sample_idx[i];
            std::string seq_out = store.get(idx);
            total_chars_random_real_get += seq_out.size();
            for (char c : seq_out) {
                checksum_dec_random += static_cast<unsigned char>(c);
            }
        }
        auto end_random_real_get = std::chrono::high_resolution_clock::now();
        auto duration_random_real_get = std::chrono::duration_cast<std::chrono::milliseconds>(end_random_real_get - start_random_real_get);
        std::cout << "Random real get() over " << sample_count << " sequences (no sort, decode + string alloc): "
                  << duration_random_real_get.count() << " ms (" << duration_random_real_get.count() / 1000.0 << " s), "
                  << "checksum_dec=" << checksum_dec_random << ", chars=" << total_chars_random_real_get << std::endl;

        double gb_per_sec_random_real_get = (total_chars_random_real_get / (1024.0 * 1024.0 * 1024.0)) / (duration_random_real_get.count() / 1000.0 + 1e-9);
        std::cout << "Memory bandwidth (random real get path): " << gb_per_sec_random_real_get * 1024.0 << " MB/s ("
                  << gb_per_sec_random_real_get << " GB/s)" << std::endl;
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
