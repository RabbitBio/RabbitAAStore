#include "CLI11.hpp"
#include "kseq.h"
#include "PagedProteinAAStore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

static std::atomic<uint64_t> g_kseq_bytes_read{0};

static inline size_t kseq_file_read(FILE* fp, void* buf, size_t size) {
    size_t read_bytes = fread(buf, 1, size, fp);
    g_kseq_bytes_read.fetch_add(static_cast<uint64_t>(read_bytes),
                                std::memory_order_relaxed);
    return read_bytes;
}

KSEQ_INIT(FILE*, kseq_file_read)

static inline bool fasta_header_name_delim(unsigned char c) {
    return c <= ' ';
}

static inline size_t fasta_direct_read(FILE* fp, void* buf, size_t size) {
#if defined(__GLIBC__) || defined(__linux__)
    return fread_unlocked(buf, 1, size, fp);
#else
    return std::fread(buf, 1, size, fp);
#endif
}

static uint64_t drop_input_file_cache(FILE* fp) {
    if (fp == nullptr) return 0;
#if defined(POSIX_FADV_DONTNEED)
    auto start = std::chrono::steady_clock::now();
    (void)::posix_fadvise(::fileno(fp), 0, 0, POSIX_FADV_DONTNEED);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count());
#else
    return 0;
#endif
}

class FastaInputCacheAdvice {
public:
    explicit FastaInputCacheAdvice(FILE* fp, bool drop_old_input_cache)
        : drop_old_input_cache_(drop_old_input_cache) {
#if defined(POSIX_FADV_SEQUENTIAL) || defined(POSIX_FADV_DONTNEED)
        fd_ = ::fileno(fp);
#endif
#if defined(POSIX_FADV_SEQUENTIAL)
        if (fd_ >= 0) {
            (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
        }
#endif
    }

    void prefetch_ahead(uint64_t consumed_bytes) {
#if defined(POSIX_FADV_WILLNEED)
        if (fd_ < 0) return;
        const uint64_t target = consumed_bytes + prefetch_window_bytes_;
        if (target <= prefetched_until_ + prefetch_step_bytes_) return;
        const uint64_t start = prefetched_until_;
        const uint64_t bytes = target - prefetched_until_;
        (void)::posix_fadvise(fd_, static_cast<off_t>(start),
                              static_cast<off_t>(bytes),
                              POSIX_FADV_WILLNEED);
        prefetched_until_ = target;
#else
        (void)consumed_bytes;
#endif
    }

    void drop_old_ranges(uint64_t consumed_bytes) {
#if defined(POSIX_FADV_DONTNEED)
        if (!drop_old_input_cache_ || fd_ < 0 ||
            consumed_bytes <= keep_tail_bytes_) {
            return;
        }
        const uint64_t drop_end = consumed_bytes - keep_tail_bytes_;
        if (drop_end < dropped_until_ + drop_step_bytes_) return;
        const uint64_t drop_bytes = drop_end - dropped_until_;
        (void)::posix_fadvise(fd_, static_cast<off_t>(dropped_until_),
                              static_cast<off_t>(drop_bytes),
                              POSIX_FADV_DONTNEED);
        dropped_until_ = drop_end;
#else
        (void)consumed_bytes;
#endif
    }

private:
    static constexpr uint64_t keep_tail_bytes_ = 64ULL * 1024ULL * 1024ULL;
    static constexpr uint64_t drop_step_bytes_ = 256ULL * 1024ULL * 1024ULL;
    static constexpr uint64_t prefetch_window_bytes_ = 256ULL * 1024ULL * 1024ULL;
    static constexpr uint64_t prefetch_step_bytes_ = 64ULL * 1024ULL * 1024ULL;
    int fd_ = -1;
    uint64_t dropped_until_ = 0;
    uint64_t prefetched_until_ = 0;
    bool drop_old_input_cache_ = false;
};

struct DirectFastaParseStats {
    uint64_t read_ns = 0;
    uint64_t parse_ns = 0;
    uint64_t advice_ns = 0;
    uint64_t read_calls = 0;
    uint64_t chunks = 0;
    uint64_t fragments = 0;
    uint64_t header_fragments = 0;
    uint64_t sequence_fragments = 0;
    uint64_t header_fragment_bytes = 0;
    uint64_t sequence_fragment_bytes = 0;
};

template <typename BeginSequence, typename AppendResidues,
          typename EndSequence, typename OnHeader>
static bool stream_fasta_direct(FILE* fp,
                                BeginSequence&& begin_sequence,
                                AppendResidues&& append_residues,
                                EndSequence&& end_sequence,
                                OnHeader&& on_header,
                                bool async_read,
                                DirectFastaParseStats* stats = nullptr) {
    constexpr size_t buffer_bytes = 16ULL * 1024ULL * 1024ULL;
    std::string header_name;
    bool sequence_open = false;
    bool in_header = false;
    bool header_name_done = false;
    bool at_line_start = true;
    bool pending_sequence_cr = false;

    auto finish_sequence_if_open = [&]() -> bool {
        if (!sequence_open) return true;
        sequence_open = false;
        return end_sequence();
    };

    auto start_header = [&]() -> bool {
        if (!finish_sequence_if_open()) return false;
        header_name.clear();
        header_name_done = false;
        in_header = true;
        return true;
    };

    auto append_header_fragment = [&](const char* data, size_t len) {
        if (header_name_done) return;
        size_t pos = 0;
        while (pos < len &&
               !fasta_header_name_delim(static_cast<unsigned char>(data[pos]))) {
            ++pos;
        }
        header_name.append(data, pos);
        if (pos < len) header_name_done = true;
    };

    auto finish_header = [&]() -> bool {
        if (!on_header(header_name.data(), header_name.size())) return false;
        if (!begin_sequence()) return false;
        sequence_open = true;
        in_header = false;
        header_name.clear();
        header_name_done = false;
        return true;
    };

    auto append_sequence_fragment = [&](const char* data, size_t len,
                                        bool line_ended) -> bool {
        if (!sequence_open) return true;
        if (pending_sequence_cr) {
            if (!(line_ended && len == 0)) {
                const char cr = '\r';
                if (!append_residues(&cr, 1)) return false;
            }
            pending_sequence_cr = false;
        }
        if (len == 0) return true;
        if (line_ended) {
            if (data[len - 1] == '\r') --len;
        } else if (data[len - 1] == '\r') {
            --len;
            pending_sequence_cr = true;
        }
        return len == 0 || append_residues(data, len);
    };

    auto parse_buffer = [&](const char* buffer, size_t read_bytes) -> bool {
        const auto parse_start = std::chrono::steady_clock::now();
        size_t pos = 0;
        while (pos < read_bytes) {
            const char* chunk = buffer + pos;
            const void* newline_ptr =
                std::memchr(chunk, '\n', read_bytes - pos);
            const bool line_ended = newline_ptr != nullptr;
            const size_t fragment_len = line_ended
                ? static_cast<size_t>(static_cast<const char*>(newline_ptr) - chunk)
                : read_bytes - pos;

            if (stats) ++stats->fragments;
            if (in_header) {
                if (stats) {
                    ++stats->header_fragments;
                    stats->header_fragment_bytes += static_cast<uint64_t>(fragment_len);
                }
                append_header_fragment(chunk, fragment_len);
                if (line_ended && !finish_header()) return false;
            } else if (at_line_start && fragment_len != 0 && chunk[0] == '>') {
                if (stats) {
                    ++stats->header_fragments;
                    stats->header_fragment_bytes +=
                        static_cast<uint64_t>(fragment_len - 1);
                }
                if (!start_header()) return false;
                append_header_fragment(chunk + 1, fragment_len - 1);
                if (line_ended && !finish_header()) return false;
            } else {
                if (stats) {
                    ++stats->sequence_fragments;
                    stats->sequence_fragment_bytes += static_cast<uint64_t>(fragment_len);
                }
                if (!append_sequence_fragment(chunk, fragment_len, line_ended)) {
                    return false;
                }
            }

            at_line_start = line_ended;
            if (!line_ended) break;
            pos += fragment_len + 1;
        }
        if (stats) {
            stats->parse_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - parse_start).count());
        }
        return true;
    };

    if (!async_read) {
        std::vector<char> buffer(buffer_bytes);
        FastaInputCacheAdvice input_cache(fp, false);
        uint64_t consumed_bytes = 0;
        auto advice_start = std::chrono::steady_clock::now();
        input_cache.prefetch_ahead(consumed_bytes);
        if (stats) {
            stats->advice_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - advice_start).count());
        }
        while (true) {
            const auto read_start = std::chrono::steady_clock::now();
            size_t read_bytes = fasta_direct_read(fp, buffer.data(), buffer.size());
            const auto read_end = std::chrono::steady_clock::now();
            if (stats) {
                stats->read_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        read_end - read_start).count());
                ++stats->read_calls;
                if (read_bytes != 0) ++stats->chunks;
            }
            consumed_bytes += static_cast<uint64_t>(read_bytes);
            g_kseq_bytes_read.fetch_add(static_cast<uint64_t>(read_bytes),
                                        std::memory_order_relaxed);
            advice_start = std::chrono::steady_clock::now();
            input_cache.prefetch_ahead(consumed_bytes);
            input_cache.drop_old_ranges(consumed_bytes);
            const auto advice_end = std::chrono::steady_clock::now();
            if (stats) {
                stats->advice_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        advice_end - advice_start).count());
            }
            if (read_bytes == 0) {
                if (std::ferror(fp)) return false;
                break;
            }
            if (!parse_buffer(buffer.data(), read_bytes)) return false;
        }

        if (in_header && !finish_header()) {
            return false;
        }
        pending_sequence_cr = false;
        return finish_sequence_if_open();
    }

    struct ReadSlot {
        std::vector<char> buffer;
        size_t bytes = 0;
        bool ready = false;
        bool free = true;
    };

    std::array<ReadSlot, 2> slots;
    for (ReadSlot& slot : slots) slot.buffer.resize(buffer_bytes);

    std::mutex reader_mutex;
    std::condition_variable reader_cv;
    bool reader_stop = false;
    bool reader_eof = false;
    bool reader_failed = false;
    uint64_t reader_consumed_bytes = 0;
    uint64_t reader_read_ns = 0;
    uint64_t reader_advice_ns = 0;
    uint64_t reader_read_calls = 0;
    uint64_t reader_chunks = 0;

    auto reader_thread = std::thread([&]() {
        FastaInputCacheAdvice input_cache(fp, false);
        size_t fill_slot = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(reader_mutex);
                reader_cv.wait(lock, [&]() {
                    return reader_stop || slots[fill_slot].free;
                });
                if (reader_stop) break;
                slots[fill_slot].free = false;
                slots[fill_slot].ready = false;
                slots[fill_slot].bytes = 0;
            }

            const auto read_start = std::chrono::steady_clock::now();
            size_t read_bytes = fasta_direct_read(
                fp, slots[fill_slot].buffer.data(), slots[fill_slot].buffer.size());
            const auto read_end = std::chrono::steady_clock::now();
            reader_read_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    read_end - read_start).count());
            ++reader_read_calls;
            if (read_bytes != 0) ++reader_chunks;

            if (read_bytes == 0) {
                const bool failed = std::ferror(fp) != 0;
                {
                    std::lock_guard<std::mutex> lock(reader_mutex);
                    slots[fill_slot].free = true;
                    reader_failed = failed;
                    reader_eof = true;
                }
                reader_cv.notify_all();
                break;
            }

            reader_consumed_bytes += static_cast<uint64_t>(read_bytes);
            g_kseq_bytes_read.fetch_add(static_cast<uint64_t>(read_bytes),
                                        std::memory_order_relaxed);

            const auto advice_start = std::chrono::steady_clock::now();
            input_cache.prefetch_ahead(reader_consumed_bytes);
            input_cache.drop_old_ranges(reader_consumed_bytes);
            const auto advice_end = std::chrono::steady_clock::now();
            reader_advice_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    advice_end - advice_start).count());

            {
                std::lock_guard<std::mutex> lock(reader_mutex);
                slots[fill_slot].bytes = read_bytes;
                slots[fill_slot].ready = true;
            }
            reader_cv.notify_all();
            fill_slot ^= 1;
        }
    });

    bool reader_joined = false;
    auto finish_reader = [&]() {
        {
            std::lock_guard<std::mutex> lock(reader_mutex);
            reader_stop = true;
        }
        reader_cv.notify_all();
        if (reader_thread.joinable()) reader_thread.join();
        if (!reader_joined && stats) {
            stats->read_ns += reader_read_ns;
            stats->advice_ns += reader_advice_ns;
            stats->read_calls += reader_read_calls;
            stats->chunks += reader_chunks;
        }
        reader_joined = true;
    };

    size_t parse_slot = 0;
    while (true) {
        size_t read_bytes = 0;
        bool failed_without_ready = false;
        bool done_without_ready = false;
        {
            std::unique_lock<std::mutex> lock(reader_mutex);
            reader_cv.wait(lock, [&]() {
                return slots[parse_slot].ready || reader_eof || reader_failed;
            });
            if (!slots[parse_slot].ready) {
                failed_without_ready = reader_failed;
                done_without_ready = !reader_failed;
            } else {
                read_bytes = slots[parse_slot].bytes;
                slots[parse_slot].ready = false;
            }
        }
        if (failed_without_ready) {
            finish_reader();
            return false;
        }
        if (done_without_ready) break;

        const bool parsed =
            parse_buffer(slots[parse_slot].buffer.data(), read_bytes);
        {
            std::lock_guard<std::mutex> lock(reader_mutex);
            slots[parse_slot].bytes = 0;
            slots[parse_slot].free = true;
        }
        reader_cv.notify_all();
        if (!parsed) {
            finish_reader();
            return false;
        }
        parse_slot ^= 1;
    }

    finish_reader();

    if (in_header && !finish_header()) {
        return false;
    }
    pending_sequence_cr = false;
    return finish_sequence_if_open();
}

static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static std::vector<size_t> make_random_ids(size_t store_size, uint64_t samples, uint64_t seed) {
    std::vector<size_t> ids;
    ids.reserve(static_cast<size_t>(samples));
    uint64_t state = seed;
    for (uint64_t i = 0; i < samples; ++i) {
        ids.push_back(static_cast<size_t>(splitmix64(state) % store_size));
    }
    return ids;
}

static uint64_t checksum_strings(const std::vector<std::string>& seqs, uint64_t& total_chars) {
    uint64_t checksum = 0;
    total_chars = 0;
    for (const std::string& seq : seqs) {
        total_chars += seq.size();
        for (unsigned char c : seq) checksum += c;
    }
    return checksum;
}

static uint64_t fixed_packed_page_bytes() {
    return 4096;
}

static uint64_t fixed_page_residues() {
    return (fixed_packed_page_bytes() * 8) / 5;
}

class ProgressBar {
public:
    ProgressBar(const std::string& label, uint64_t total, bool bytes_mode)
        : label_(label), total_(total), bytes_mode_(bytes_mode),
          start_(std::chrono::steady_clock::now()), last_print_(start_) {
        update(0, "", true);
    }

    void update(uint64_t done, const std::string& detail = "", bool force = false) {
        if (total_ == 0) return;
        if (done > total_) done = total_;

        auto now = std::chrono::steady_clock::now();
        if (!force &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_).count() < 200) {
            return;
        }
        last_print_ = now;

        const size_t bar_width = 36;
        double ratio = static_cast<double>(done) / static_cast<double>(total_);
        size_t filled = static_cast<size_t>(ratio * static_cast<double>(bar_width));
        if (filled > bar_width) filled = bar_width;

        std::string bar(filled, '#');
        bar.append(bar_width - filled, '-');

        std::ostringstream line;
        line << label_ << " [" << bar << "] "
             << std::fixed << std::setprecision(1) << (ratio * 100.0) << "% "
             << format_value(done) << "/" << format_value(total_);
        if (!detail.empty()) {
            line << " " << detail;
        }

        std::string text = line.str();
        std::cerr << '\r' << text;
        if (text.size() < last_line_len_) {
            std::cerr << std::string(last_line_len_ - text.size(), ' ');
        }
        std::cerr << std::flush;
        last_line_len_ = text.size();
    }

    void finish(uint64_t done, const std::string& detail = "") {
        update(done, detail, true);
        std::cerr << "\n";
        last_line_len_ = 0;
    }

private:
    std::string label_;
    uint64_t total_ = 0;
    bool bytes_mode_ = false;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point last_print_;
    size_t last_line_len_ = 0;

    std::string format_value(uint64_t value) const {
        if (!bytes_mode_) return std::to_string(value);

        std::ostringstream out;
        out << std::fixed << std::setprecision(1)
            << (static_cast<double>(value) / (1024.0 * 1024.0)) << "MiB";
        return out.str();
    }
};

int main(int argc, char** argv) {
    CLI::App app{"Build and query paged 5-bit ProteinAAStore"};

    std::string input_path;
    std::string output_dir;

    auto build = app.add_subcommand("build", "Build paged store from an existing ProteinAAStore binary");
    build->add_option("-i,--input", input_path, "Input ProteinAAStore binary")->required()->check(CLI::ExistingFile);
    build->add_option("-o,--output", output_dir, "Output directory for meta.bin and pages.bin")->required();

    std::string fasta_path;
    std::string names_output;
    std::string name_index_output;
    bool no_names = false;
    bool build_name_index = false;
    bool stop_aware = true;
    bool fast_encoding = false;
    bool build_direct_output = false;
    bool build_reader_thread = true;
    bool drop_input_cache = false;
    uint64_t min_length = 0;
    uint64_t build_threads = 2;
    std::string build_io_policy = "balanced";
    uint64_t build_write_buffer_mb =
        PagedProteinAAStore::default_build_write_buffer_bytes() / (1024ULL * 1024ULL);
    uint64_t build_page_buffer_mb =
        PagedProteinAAStore::BufferedPageWriter::default_buffer_bytes /
        (1024ULL * 1024ULL);
    uint64_t build_writeback_step_mb =
        PagedProteinAAStore::BufferedPageWriter::default_writeback_step_bytes /
        (1024ULL * 1024ULL);
    auto build_fa = app.add_subcommand("build-fa", "Stream-build paged store directly from FASTA");
    build_fa->add_option("-f,--fasta", fasta_path, "Input FASTA file")->required()->check(CLI::ExistingFile);
    build_fa->add_option("-o,--output", output_dir, "Output directory for meta.bin and pages.bin")->required();
    build_fa->add_option("--names-output", names_output, "Output FASTA sequence names, one kept sequence per line. Default: seq_names.txt in output directory");
    build_fa->add_flag("--no-names", no_names, "Do not write seq_names.txt during FASTA build");
    build_fa->add_flag("--name-index", build_name_index, "Also build seq_names.index.bin for fast name loading");
    build_fa->add_option("--name-index-output", name_index_output, "Output binary name index. Default: seq_names.index.bin next to seq_names.txt");
    build_fa->add_flag("--stop-aware", stop_aware, "Preserve '*' by encoding it as code 27; enabled by default");
    build_fa->add_flag("--fast-encoding", fast_encoding, "Use legacy low-5-bit encoding; '*' collides with J");
    build_fa->add_flag("--build-direct-output", build_direct_output, "Write pages.bin with O_DIRECT using an aligned buffer; experimental");
    build_fa->add_flag("--build-reader-thread,!--no-build-reader-thread",
                       build_reader_thread,
                       "Use a dedicated FASTA reader thread when -j >= 2; "
                       "enabled by default. Disable to use input prefetch in the producer thread");
    build_fa->add_flag("--drop-input-cache", drop_input_cache,
                       "Ask the kernel to evict the input FASTA from OS page cache before building");
    build_fa->add_option("-L,--min-length", min_length, "Minimum sequence length filter, default 0");
    build_fa->add_option(
        "-j,--threads", build_threads,
        "Total build threads, default 2; 1 uses producer-side input prefetch, "
        "N>=2 reserves one thread for double-buffered FASTA reading unless "
        "--no-build-reader-thread is set; "
        "0 uses hardware concurrency");
    build_fa->add_option(
        "--build-io-policy", build_io_policy,
        "Build write policy: balanced writes packed pages as soon as ready; "
        "read-first buffers packed pages and writes only when the buffer is near full or FASTA reading is done")
        ->check(CLI::IsMember({"balanced", "read-first", "read_first"}));
    build_fa->add_option(
        "--build-write-buffer-mb", build_write_buffer_mb,
        "Packed page result buffer budget for --build-io-policy read-first, default 4096 MiB");
    build_fa->add_option(
        "--build-page-buffer-mb", build_page_buffer_mb,
        "Buffered pages.bin write buffer in MiB, default 4");
    build_fa->add_option(
        "--build-writeback-step-mb", build_writeback_step_mb,
        "sync_file_range writeback step in MiB for pages.bin; 0 disables explicit writeback hints, default 512");

    std::string store_dir;
    uint64_t memory_mb = 0;
    uint64_t samples = 10000;
    uint64_t benchmark_rounds = 1;
    uint64_t seed = 12345;
    uint64_t prefetch_mb =
        PagedProteinAAStore::default_prefetch_memory_cap_bytes() / (1024ULL * 1024ULL);
    uint64_t preload_threads = 0;
    uint64_t prefetch_threads = 2;
    uint64_t decode_threads = 1;
    uint64_t prefetch_depth = 0;
    uint64_t single_thread_aio_completions = 64;
    uint64_t aio_completion_min = 4;
    std::string io_backend = "auto";
    std::string cache_policy = "lru";
    bool batch_mode = false;
    bool batch_into_mode = false;
    bool bench_by_name = false;
    bool preload_cache = false;
    bool no_preload_cache = false;
    bool drop_page_cache = false;
    bool single_thread_aio = false;
    bool uring_sqpoll = false;
    bool load_names = false;
    std::string bench_names_path;

    auto bench = app.add_subcommand("bench", "Benchmark random access on a paged store");
    bench->add_option("-i,--input", store_dir, "Paged store directory")->required()->check(CLI::ExistingDirectory);
    bench->add_option("-n,--num", samples, "Number of random sequence ids");
    bench->add_option("--rounds", benchmark_rounds, "Repeat the same random batch in one store instance, default 1")
        ->check(CLI::PositiveNumber);
    bench->add_option("--seed", seed, "Random seed");
    bench->add_option("--memory-mb,--cache-mb", memory_mb, "Resident page-cache memory budget in MiB; 0/default loads all pages; --cache-mb is a legacy alias");
    bench->add_flag("--batch", batch_mode, "Force batch_get even when --prefetch-mb 0; default nonzero prefetch already uses batch_get");
    bench->add_flag("--batch-into", batch_into_mode, "Benchmark batch_get_into with caller-preallocated output buffers");
    bench->add_flag("--by-name", bench_by_name, "Benchmark name lookup plus sequence access using batch_get_by_name/get_by_name");
    bench->add_option("--prefetch-mb", prefetch_mb, "Temporary miss-page buffer upper bound in MiB for half-memory access; effective value is capped by the library default, 0 disables batch prefetch");
    bench->add_option("--prefetch-threads", prefetch_threads, "Total I/O streams including the caller thread, default 2; 1 or 0 uses caller only");
    bench->add_option("--decode-threads", decode_threads, "Full-memory batch_get_into decode threads, default 1")
        ->check(CLI::PositiveNumber);
    bench->add_option("--prefetch-depth", prefetch_depth, "Advanced override: outstanding async reads per I/O stream; 0/default lets the library choose from --prefetch-mb");
    bench->add_option("--io-backend", io_backend, "Advanced override: auto prefers io_uring, then native AIO, then pread");
    bench->add_option("--cache-policy", cache_policy, "Resident cache policy: lru uses 2Q-lite and promotes only recent second misses; prefix keeps only the preloaded prefix")
        ->check(CLI::IsMember({"lru", "lru_2q_lite", "2q_lite", "prefix", "non-lru", "non_lru"}));
    bench->add_flag("--uring-sqpoll", uring_sqpoll, "Use io_uring SQPOLL for uring backends");
    bench->add_flag("--single-thread-aio", single_thread_aio, "Legacy alias for caller-only async I/O; equivalent to --prefetch-threads 0");
    bench->add_option("--single-thread-aio-completions,--aio-completions", single_thread_aio_completions, "Advanced override: max completions drained before I/O refill, default 64");
    bench->add_option("--aio-completion-min", aio_completion_min, "Min Linux AIO completions to wait for before non-blocking drain, default 4");
    bench->add_flag("--drop-page-cache", drop_page_cache, "Ask the kernel to evict pages.bin from OS page cache before timed access");
    bench->add_flag("--preload-cache", preload_cache, "Legacy flag; preload is automatic for --memory-mb > 0 and full preload is automatic when memory fits pages.bin");
    bench->add_flag("--no-preload-cache", no_preload_cache, "Benchmark only: do not preload resident PageCache before timed access");
    bench->add_option("--preload-threads", preload_threads, "Advanced override: preload threads; default 0 chooses up to 16 automatically");
    bench->add_flag("--load-names", load_names, "Load seq_names.txt into memory and build name->id index");
    bench->add_option("--names-path", bench_names_path, "Override sequence names path; default is seq_names.txt in the store directory");

    uint64_t seq_id = 0;
    std::string seq_name;
    std::string get_names_path;
    auto get = app.add_subcommand("get", "Print one sequence from a paged store");
    get->add_option("-i,--input", store_dir, "Paged store directory")->required()->check(CLI::ExistingDirectory);
    auto get_id_option = get->add_option("--id", seq_id, "Sequence id");
    auto get_name_option = get->add_option("--name", seq_name, "Sequence name");
    get->add_option("--names-path", get_names_path, "Override sequence names path; default is seq_names.txt in the store directory");
    get->add_option("--memory-mb,--cache-mb", memory_mb, "Resident page-cache memory budget in MiB; 0/default loads all pages; --cache-mb is a legacy alias");
    get->add_option("--cache-policy", cache_policy, "Resident cache policy: lru (2Q-lite) or prefix")
        ->check(CLI::IsMember({"lru", "lru_2q_lite", "2q_lite", "prefix", "non-lru", "non_lru"}));

    app.require_subcommand(1);
    CLI11_PARSE(app, argc, argv);

    if (*get) {
        const bool has_id = get_id_option->count() != 0;
        const bool has_name = get_name_option->count() != 0;
        if (has_id == has_name) {
            std::cerr << "Error: get requires exactly one of --id or --name\n";
            return 1;
        }
    }

    bool use_single_thread_aio = single_thread_aio || prefetch_threads == 0;
    if (*bench && use_single_thread_aio) {
        if (prefetch_mb == 0 || prefetch_depth == 1) {
            std::cerr << "Error: caller-thread AIO requires --prefetch-mb > 0 and --prefetch-depth 0(auto) or >1\n";
            return 1;
        }
        if (single_thread_aio_completions == 0) {
            std::cerr << "Error: --single-thread-aio-completions must be at least 1\n";
            return 1;
        }
    }
    if (*bench && aio_completion_min == 0) {
        std::cerr << "Error: --aio-completion-min must be at least 1\n";
        return 1;
    }
    uint64_t effective_prefetch_threads = use_single_thread_aio ? 1 : prefetch_threads;
    bool caller_only_io = effective_prefetch_threads <= 1;

    if (*build) {
        uint64_t page_residues = fixed_page_residues();
        std::unique_ptr<ProgressBar> progress;
        uint64_t progress_total = 0;
        uint64_t progress_done = 0;
        auto on_progress = [&](uint64_t done, uint64_t total) {
            if (!progress) {
                progress.reset(new ProgressBar("Building pages", total, false));
                progress_total = total;
            }
            progress_done = done;
            progress->update(done);
        };

        auto start = std::chrono::high_resolution_clock::now();
        if (!PagedProteinAAStore::build_from_store(input_path, output_dir, page_residues, on_progress)) {
            if (progress) progress->finish(progress_done);
            std::cerr << "Error: failed to build paged store\n";
            std::cerr << "Input must be an existing ProteinAAStore binary, not a FASTA file.\n";
            std::cerr << "Build one first, for example:\n";
            std::cerr << "  ./build/aatools -f input.fa -o /tmp/store.bin\n";
            std::cerr << "Then run:\n";
            std::cerr << "  ./build/paged_aatools build -i /tmp/store.bin -o " << output_dir << "\n";
            return 1;
        }
        auto end = std::chrono::high_resolution_clock::now();
        if (progress) progress->finish(progress_total);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Built paged store in " << ms << " ms\n";
        std::cout << "Output: " << std::filesystem::absolute(output_dir).string() << "\n";
        std::cout << "Packed page bytes: " << fixed_packed_page_bytes() << "\n";
        std::cout << "Page residues: " << page_residues << "\n";
        std::cout << "Build encode_kernel: "
                  << PagedProteinAAStore::encode_kernel_name() << "\n";
        return 0;
    }

    if (*build_fa) {
        uint64_t page_residues = fixed_page_residues();
        namespace fs = std::filesystem;
        try {
            fs::create_directories(output_dir);
        } catch (...) {
            std::cerr << "Error: failed to create output directory: " << output_dir << "\n";
            return 1;
        }
        if (!no_names && names_output.empty()) {
            names_output = (fs::path(output_dir) / "seq_names.txt").string();
        }
        if (build_name_index && no_names) {
            std::cerr << "Error: --name-index requires sequence names; remove --no-names\n";
            return 1;
        }
        if (build_name_index && name_index_output.empty()) {
            name_index_output =
                PagedProteinAAStore::default_name_index_path(names_output);
        }
        std::ofstream names_out;
        if (!no_names) {
            names_out.open(names_output, std::ios::binary);
        }
        if (!no_names && !names_out) {
            std::cerr << "Error: failed to open names output: " << names_output << "\n";
            return 1;
        }

        uint64_t fasta_size = 0;
        try {
            fasta_size = static_cast<uint64_t>(fs::file_size(fasta_path));
        } catch (...) {
            fasta_size = 0;
        }

        g_kseq_bytes_read.store(0, std::memory_order_relaxed);

        uint64_t read_count = 0;
        uint64_t kept_count = 0;
        const uint64_t progress_interval = 10000;
        ProgressBar progress("Reading FASTA", fasta_size, true);
        auto progress_detail = [&]() {
            std::ostringstream detail;
            detail << "read_seqs=" << read_count << " kept_seqs=" << kept_count;
            return detail.str();
        };
        auto update_progress = [&](bool force = false) {
            if (!force && read_count % progress_interval != 0) return;
            progress.update(g_kseq_bytes_read.load(std::memory_order_relaxed),
                            progress_detail(), force);
        };
        update_progress(true);

        bool names_write_error = false;
        uint64_t names_append_ns = 0;
        uint64_t names_append_sample_ns = 0;
        uint64_t names_append_sample_bytes = 0;
        uint64_t names_write_ns = 0;
        uint64_t names_bytes = 0;
        uint64_t names_flushes = 0;
        const size_t names_buffer_limit = 8ULL * 1024ULL * 1024ULL;
        std::string names_buffer;
        if (!no_names) {
            names_buffer.reserve(names_buffer_limit);
        }
        auto flush_names = [&]() -> bool {
            if (no_names) return true;
            if (names_buffer.empty()) return true;
            auto write_start = std::chrono::steady_clock::now();
            names_out.write(names_buffer.data(), static_cast<std::streamsize>(names_buffer.size()));
            names_write_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_start).count());
            ++names_flushes;
            if (!names_out) {
                names_write_error = true;
                return false;
            }
            names_buffer.clear();
            return true;
        };
        auto append_name = [&](const char* name, size_t name_len) -> bool {
            if (no_names) return true;
            names_buffer.append(name, name_len);
            names_buffer.push_back('\n');
            names_bytes += static_cast<uint64_t>(name_len + 1);
            if (names_buffer.size() >= names_buffer_limit) {
                return flush_names();
            }
            return true;
        };

        auto start = std::chrono::high_resolution_clock::now();
        PagedProteinAAStore::BuildStats build_stats;
        DirectFastaParseStats direct_parse_stats;
        uint64_t drop_input_cache_ns = 0;
        if (fast_encoding) stop_aware = false;
        const auto encoding_mode = stop_aware
            ? PagedProteinAAStore::EncodingMode::StopAware
            : PagedProteinAAStore::EncodingMode::Fast;
        const size_t effective_build_threads = static_cast<size_t>(
            build_threads == 0 ? std::max(1u, std::thread::hardware_concurrency())
                               : build_threads);
        const bool async_direct_read = build_reader_thread && effective_build_threads >= 2;
        const size_t reserved_reader_threads = async_direct_read ? 1 : 0;
        const size_t pack_worker_threads =
            effective_build_threads > 1 + reserved_reader_threads
                ? effective_build_threads - 1 - reserved_reader_threads
                : 0;
        const bool direct_task_parser = min_length == 0;
        const auto build_policy =
            (build_io_policy == "read-first" || build_io_policy == "read_first")
                ? PagedProteinAAStore::BuildIoPolicy::ReadFirst
                : PagedProteinAAStore::BuildIoPolicy::Balanced;
        const uint64_t build_write_buffer_bytes =
            build_write_buffer_mb * 1024ULL * 1024ULL;
        const uint64_t build_page_buffer_bytes =
            build_page_buffer_mb * 1024ULL * 1024ULL;
        const uint64_t build_writeback_step_bytes =
            build_writeback_step_mb * 1024ULL * 1024ULL;
        bool ok = false;
        if (direct_task_parser) {
            FILE* fp = std::fopen(fasta_path.c_str(), "rb");
            if (!fp) {
                std::cerr << "Error: failed to open FASTA: " << fasta_path << "\n";
                return 1;
            }
            if (drop_input_cache) {
                drop_input_cache_ns = drop_input_file_cache(fp);
            }

            auto direct_stream = [&](auto&& begin_sequence,
                                     auto&& append_residues,
                                     auto&& end_sequence) -> bool {
                auto on_header = [&](const char* header, size_t header_len) -> bool {
                    ++read_count;
                    if (!append_name(header, header_len)) return false;
                    ++kept_count;
                    update_progress();
                    return true;
                };
                return stream_fasta_direct(fp, begin_sequence, append_residues,
                                           end_sequence, on_header,
                                           async_direct_read,
                                           &direct_parse_stats);
            };
            if (pack_worker_threads == 0) {
                ok = PagedProteinAAStore::build_from_sequence_stream(
                    output_dir, page_residues, direct_stream, &build_stats,
                    encoding_mode, build_page_buffer_bytes,
                    build_writeback_step_bytes, build_direct_output);
            } else {
                ok = PagedProteinAAStore::build_from_sequence_stream_parallel(
                    output_dir, page_residues, direct_stream,
                    pack_worker_threads, &build_stats, encoding_mode,
                    build_policy, build_write_buffer_bytes,
                    build_page_buffer_bytes, build_writeback_step_bytes,
                    build_direct_output);
            }
            std::fclose(fp);
        } else {
            FILE* fp = std::fopen(fasta_path.c_str(), "rb");
            if (!fp) {
                std::cerr << "Error: failed to open FASTA: " << fasta_path << "\n";
                return 1;
            }
            if (drop_input_cache) {
                drop_input_cache_ns = drop_input_file_cache(fp);
            }
            kseq_t* seq = kseq_init(fp);
            if (!seq) {
                std::fclose(fp);
                std::cerr << "Error: failed to initialize FASTA reader\n";
                return 1;
            }

            auto next_sequence = [&](std::string_view& out) -> bool {
                while (kseq_read(seq) >= 0) {
                    ++read_count;
                    if (seq->seq.l < min_length) {
                        update_progress();
                        continue;
                    }
                    if (!append_name(seq->name.s, seq->name.l)) {
                        return false;
                    }
                    out = std::string_view(seq->seq.s, seq->seq.l);
                    ++kept_count;
                    update_progress();
                    return true;
                }
                return false;
            };
            if (effective_build_threads <= 1) {
                ok = PagedProteinAAStore::build_from_sequence_views_parallel(
                    output_dir, page_residues, next_sequence,
                    size_t{1}, &build_stats, encoding_mode);
            } else {
                ok = PagedProteinAAStore::build_from_sequence_views_parallel(
                    output_dir, page_residues, next_sequence,
                    effective_build_threads - 1, &build_stats,
                    encoding_mode);
            }
            kseq_destroy(seq);
            std::fclose(fp);
        }
        progress.finish(fasta_size, progress_detail());

        if (!names_write_error) {
            flush_names();
        }
        if (!no_names) {
            names_out.close();
        }
        PagedProteinAAStore::NameIndexStats name_index_stats;
        bool name_index_ok = true;
        if (build_name_index) {
            name_index_ok = PagedProteinAAStore::build_name_index(
                names_output, name_index_output, kept_count, &name_index_stats);
        }
        auto end = std::chrono::high_resolution_clock::now();
        if (names_append_sample_bytes != 0) {
            names_append_ns = static_cast<uint64_t>(
                static_cast<long double>(names_append_sample_ns) *
                static_cast<long double>(names_bytes) /
                static_cast<long double>(names_append_sample_bytes));
        }

        if (!ok || names_write_error || (!no_names && !names_out) ||
            !name_index_ok) {
            if (names_write_error || (!no_names && !names_out)) {
                std::cerr << "Error: failed to write sequence names to " << names_output << "\n";
            }
            if (!name_index_ok) {
                std::cerr << "Error: failed to build sequence name index: "
                          << name_index_output << "\n";
            }
            std::cerr << "Error: failed to stream-build paged store from FASTA\n";
            return 1;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Built paged store from FASTA in " << ms << " ms\n";
        std::cout << "Output: " << fs::absolute(output_dir).string() << "\n";
        if (no_names) {
            std::cout << "Names output: disabled\n";
        } else {
            std::cout << "Names output: " << fs::absolute(names_output).string() << "\n";
        }
        if (build_name_index) {
            std::cout << "Name index output: "
                      << fs::absolute(name_index_output).string() << "\n";
        } else {
            std::cout << "Name index output: disabled\n";
        }
        std::cout << "Read seqs: " << read_count << "\n";
        std::cout << "Kept seqs: " << kept_count << "\n";
        std::cout << "Packed page bytes: " << fixed_packed_page_bytes() << "\n";
        std::cout << "Page residues: " << page_residues << "\n";
        std::cout << "Build FASTA parser: "
                  << (direct_task_parser ? "direct_task" : "kseq") << "\n";
        std::cout << "Build encode_kernel: "
                  << PagedProteinAAStore::encode_kernel_name() << "\n";
        std::cout << "Build encoding_mode: "
                  << PagedProteinAAStore::encoding_mode_name(encoding_mode) << "\n";
        std::cout << "Build threads: " << effective_build_threads << "\n";
        std::cout << "Build drop_input_cache: " << (drop_input_cache ? 1 : 0) << "\n";
        std::cout << "Build drop_input_cache_ms: "
                  << (drop_input_cache_ns / 1000000.0) << "\n";
        std::cout << "Build reader_threads: "
                  << (direct_task_parser && async_direct_read ? 1 : 0) << "\n";
        std::cout << "Build input_prefetch: "
                  << (direct_task_parser && !async_direct_read ? 1 : 0) << "\n";
        std::cout << "Build pack_threads: "
                  << (direct_task_parser
                          ? pack_worker_threads
                          : (effective_build_threads <= 1
                                 ? size_t{0}
                                 : effective_build_threads - 1))
                  << "\n";
        std::cout << "Build direct_page_fast_path: "
                  << (direct_task_parser && pack_worker_threads == 0 ? 1 : 0)
                  << "\n";
        std::cout << "Build io_policy: "
                  << (build_policy == PagedProteinAAStore::BuildIoPolicy::ReadFirst
                          ? "read-first"
                          : "balanced")
                  << "\n";
        std::cout << "Build write_buffer_mb: " << build_write_buffer_mb << "\n";
        std::cout << "Build page_buffer_mb: " << build_page_buffer_mb << "\n";
        std::cout << "Build writeback_step_mb: " << build_writeback_step_mb << "\n";
        std::cout << "Build direct_output: " << (build_direct_output ? 1 : 0) << "\n";
        if (direct_task_parser) {
            std::cout << "Build read_ms_est: n/a (direct parser; included in producer_ms)\n";
            std::cout << "Build raw_copy_ms_est: n/a (direct parser; included in producer_ms)\n";
        } else {
            std::cout << "Build read_ms_est: " << (build_stats.read_ns / 1000000.0) << "\n";
            std::cout << "Build raw_copy_ms_est: " << (build_stats.raw_copy_ns / 1000000.0) << "\n";
        }
        std::cout << "Build producer_ms: " << (build_stats.producer_ns / 1000000.0) << "\n";
        if (direct_task_parser) {
            std::cout << "Build direct_read_ms: "
                      << (direct_parse_stats.read_ns / 1000000.0) << "\n";
            std::cout << "Build direct_parse_ms: "
                      << (direct_parse_stats.parse_ns / 1000000.0) << "\n";
            std::cout << "Build direct_fadvise_ms: "
                      << (direct_parse_stats.advice_ns / 1000000.0) << "\n";
            std::cout << "Build direct_read_calls: "
                      << direct_parse_stats.read_calls << "\n";
            std::cout << "Build direct_chunks: "
                      << direct_parse_stats.chunks << "\n";
            std::cout << "Build direct_fragments: "
                      << direct_parse_stats.fragments << "\n";
            std::cout << "Build direct_header_fragments: "
                      << direct_parse_stats.header_fragments << "\n";
            std::cout << "Build direct_sequence_fragments: "
                      << direct_parse_stats.sequence_fragments << "\n";
            std::cout << "Build direct_header_fragment_bytes: "
                      << direct_parse_stats.header_fragment_bytes << "\n";
            std::cout << "Build direct_sequence_fragment_bytes: "
                      << direct_parse_stats.sequence_fragment_bytes << "\n";
        }
        std::cout << "Build enqueue_wait_ms: " << (build_stats.enqueue_wait_ns / 1000000.0) << "\n";
        if (direct_task_parser) {
            std::cout << "Build names_append_ms_est: n/a (included in producer_ms)\n";
        } else {
            std::cout << "Build names_append_ms_est: " << (names_append_ns / 1000000.0) << "\n";
        }
        std::cout << "Build names_write_ms: " << (names_write_ns / 1000000.0) << "\n";
        std::cout << "Build names_bytes: " << names_bytes << "\n";
        std::cout << "Build names_flushes: " << names_flushes << "\n";
        std::cout << "Build name_index_read_ms: "
                  << (name_index_stats.read_ns / 1000000.0) << "\n";
        std::cout << "Build name_index_scan_ms: "
                  << (name_index_stats.scan_offsets_ns / 1000000.0) << "\n";
        std::cout << "Build name_index_hash_ms: "
                  << (name_index_stats.build_hash_ns / 1000000.0) << "\n";
        std::cout << "Build name_index_write_ms: "
                  << (name_index_stats.write_ns / 1000000.0) << "\n";
        std::cout << "Build name_index_bytes: " << name_index_stats.index_bytes << "\n";
        std::cout << "Build name_index_slots: " << name_index_stats.hash_slots << "\n";
        std::cout << "Build pack_worker_ms_total: " << (build_stats.pack_ns / 1000000.0) << "\n";
        std::cout << "Build write_ms: " << (build_stats.write_ns / 1000000.0) << "\n";
        std::cout << "Build page_write_buffer_bytes: "
                  << build_stats.page_write_buffer_bytes << "\n";
        std::cout << "Build page_writeback_step_bytes: "
                  << build_stats.page_writeback_step_bytes << "\n";
        std::cout << "Build page_output_direct: "
                  << build_stats.page_output_direct << "\n";
        std::cout << "Build page_output_file_bytes: "
                  << build_stats.page_output_file_bytes << "\n";
        std::cout << "Build page_writeback_ms: "
                  << (build_stats.page_writeback_ns / 1000000.0) << "\n";
        std::cout << "Build page_writeback_bytes: " << build_stats.page_writeback_bytes << "\n";
        std::cout << "Build page_writeback_calls: " << build_stats.page_writeback_calls << "\n";
        std::cout << "Build result_buffer_limit_bytes: "
                  << build_stats.build_result_buffer_limit_bytes << "\n";
        std::cout << "Build result_buffer_peak_bytes: "
                  << build_stats.build_result_buffer_peak_bytes << "\n";
        std::cout << "Build result_buffer_wait_ms: "
                  << (build_stats.build_result_buffer_wait_ns / 1000000.0) << "\n";
        std::cout << "Build page_close_ms: "
                  << (build_stats.page_close_ns / 1000000.0) << "\n";
        std::cout << "Build index_write_ms: " << (build_stats.index_write_ns / 1000000.0) << "\n";
        std::cout << "Build drain_ms: " << (build_stats.drain_ns / 1000000.0) << "\n";
        std::cout << "Build finalize_ms: " << (build_stats.finalize_ns / 1000000.0) << "\n";
        std::cout << "Build index_bytes: " << build_stats.index_bytes << "\n";
        std::cout << "Build index_flushes: " << build_stats.index_flushes << "\n";
        std::cout << "Build index_buffer_bytes: " << build_stats.index_buffer_bytes << "\n";
        std::cout << "Build raw_bytes: " << build_stats.raw_bytes << "\n";
        std::cout << "Build packed_bytes: " << build_stats.packed_bytes << "\n";
        std::cout << "Build pages: " << build_stats.pages << "\n";
        std::cout << "Build tasks: " << build_stats.tasks << "\n";
        return 0;
    }

    uint64_t memory_bytes = memory_mb == 0 ? 0 : memory_mb * 1024ULL * 1024ULL;
    uint64_t prefetch_bytes = prefetch_mb * 1024ULL * 1024ULL;
    PagedProteinAAStore::Options store_options;
    store_options.memory_bytes = memory_bytes;
    store_options.prefetch_bytes = prefetch_bytes;
    store_options.io_threads = static_cast<size_t>(effective_prefetch_threads);
    store_options.decode_threads = static_cast<size_t>(decode_threads);
    store_options.preload_threads = static_cast<size_t>(preload_threads);
    store_options.prefetch_depth = static_cast<size_t>(prefetch_depth);
    store_options.uring_sqpoll = uring_sqpoll;
    store_options.caller_runs_async_io = caller_only_io;
    store_options.aio_completion_batch =
        static_cast<size_t>(single_thread_aio_completions);
    store_options.aio_completion_min = static_cast<size_t>(aio_completion_min);
    store_options.auto_preload = false;
    const bool get_by_name_mode = *get && get_name_option->count() != 0;
    store_options.load_names = load_names || get_by_name_mode || bench_by_name;
    store_options.names_path = get_by_name_mode ? get_names_path : bench_names_path;
    try {
        store_options.io_backend =
            PagedProteinAAStore::parse_prefetch_io_backend(io_backend);
        store_options.cache_policy =
            PagedProteinAAStore::parse_resident_cache_policy(cache_policy);
    } catch (const std::exception& exc) {
        std::cerr << "Error: " << exc.what() << "\n";
        return 1;
    }

    PagedProteinAAStore store;
    auto open_start = std::chrono::high_resolution_clock::now();
    if (!store.open(store_dir, store_options.memory_bytes)) {
        std::cerr << "Error: failed to open paged store: " << store_dir << "\n";
        return 1;
    }
    store.configure(store_options);
    if (store_options.load_names) {
        const std::string names_path = store_options.names_path.empty()
            ? (std::filesystem::path(store_dir) / "seq_names.txt").string()
            : store_options.names_path;
        if (!store.load_names(names_path)) {
            std::cerr << "Error: failed to load sequence names: " << names_path << "\n";
            return 1;
        }
    }
    auto open_end = std::chrono::high_resolution_clock::now();
    auto open_ms = std::chrono::duration_cast<std::chrono::milliseconds>(open_end - open_start).count();
    PagedProteinAAStore::MemoryBreakdown open_mem = store.memory_breakdown();
    bool full_cache_fits = *bench &&
                           (memory_mb == 0 ||
                            (open_mem.disk_pages_bytes > 0 &&
                             memory_bytes >= open_mem.disk_pages_bytes));
    bool auto_full_cache = !no_preload_cache && full_cache_fits;
    bool use_batch_access = *bench &&
                            (auto_full_cache || batch_mode || prefetch_bytes > 0);

    uint64_t preload_pages = 0;
    uint64_t preload_bytes = 0;
    std::chrono::milliseconds preload_ms{0};
    bool auto_preload_cache = *bench && !no_preload_cache &&
                              (preload_cache || auto_full_cache || memory_mb > 0);
    const size_t effective_preload_threads = preload_threads == 0
        ? PagedProteinAAStore::automatic_preload_thread_count()
        : static_cast<size_t>(preload_threads);
    if (auto_preload_cache) {
        auto preload_start = std::chrono::high_resolution_clock::now();
        uint64_t preload_limit = memory_mb != 0 ? memory_bytes : 0;
        preload_pages = store.preload_cache_parallel(preload_limit,
                                                     effective_preload_threads);
        auto preload_end = std::chrono::high_resolution_clock::now();
        preload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(preload_end - preload_start);
        preload_bytes = store.cache_bytes();
        store.reset_stats();
    }

    std::chrono::milliseconds drop_page_cache_ms{0};
    if (*bench && drop_page_cache) {
        auto drop_start = std::chrono::high_resolution_clock::now();
        store.drop_pages_file_cache();
        auto drop_end = std::chrono::high_resolution_clock::now();
        drop_page_cache_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            drop_end - drop_start);
    }

    if (*get) {
        if (get_by_name_mode) {
            std::cout << store.get_by_name(seq_name) << "\n";
        } else {
            std::cout << store.get(static_cast<size_t>(seq_id)) << "\n";
        }
        return 0;
    }

    std::vector<size_t> ids = make_random_ids(store.size(), samples, seed);
    std::vector<std::string_view> names;
    if (bench_by_name) {
        names.reserve(ids.size());
        for (size_t id : ids) names.push_back(store.name(id));
    }

    store.reset_stats();
    uint64_t checksum = 0;
    uint64_t total_chars = 0;
    std::chrono::nanoseconds store_get_ns{0};
    std::chrono::nanoseconds checksum_ns{0};
    std::vector<std::chrono::nanoseconds> round_store_get_ns;
    std::vector<std::chrono::nanoseconds> round_checksum_ns;
    round_store_get_ns.reserve(static_cast<size_t>(benchmark_rounds));
    round_checksum_ns.reserve(static_cast<size_t>(benchmark_rounds));
    std::vector<uint64_t> output_offsets;
    std::vector<char> output_buffer;
    std::vector<char*> output_ptrs;
    std::chrono::nanoseconds output_prepare_ns{0};
    if (use_batch_access && batch_into_mode) {
        auto output_prepare_start = std::chrono::high_resolution_clock::now();
        output_offsets.resize(ids.size() + 1, 0);
        for (size_t i = 0; i < ids.size(); ++i) {
            output_offsets[i + 1] =
                output_offsets[i] + store.sequence_length(ids[i]);
        }
        output_buffer.resize(static_cast<size_t>(output_offsets.back()));
        output_ptrs.resize(ids.size(), nullptr);
        for (size_t i = 0; i < ids.size(); ++i) {
            output_ptrs[i] = output_offsets[i] == output_offsets[i + 1]
                ? nullptr
                : output_buffer.data() + static_cast<size_t>(output_offsets[i]);
        }
        output_prepare_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - output_prepare_start);
    }
    auto start = std::chrono::high_resolution_clock::now();
    for (uint64_t round = 0; round < benchmark_rounds; ++round) {
        if (use_batch_access) {
            auto get_start = std::chrono::high_resolution_clock::now();
            std::vector<std::string> seqs;
            if (batch_into_mode) {
                if (bench_by_name) {
                    store.batch_get_by_name_into(names, output_ptrs);
                } else {
                    store.batch_get_into(ids, output_ptrs);
                }
            } else {
                seqs = bench_by_name
                    ? store.batch_get_by_name(names)
                    : store.batch_get(ids);
            }
            auto get_end = std::chrono::high_resolution_clock::now();
            const auto round_get_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                get_end - get_start);
            store_get_ns += round_get_ns;
            round_store_get_ns.push_back(round_get_ns);
            auto checksum_start = std::chrono::high_resolution_clock::now();
            if (batch_into_mode) {
                total_chars += static_cast<uint64_t>(output_buffer.size());
                for (unsigned char c : output_buffer) checksum += c;
            } else {
                uint64_t round_chars = 0;
                checksum += checksum_strings(seqs, round_chars);
                total_chars += round_chars;
            }
            const auto round_checksum = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - checksum_start);
            checksum_ns += round_checksum;
            round_checksum_ns.push_back(round_checksum);
        } else {
            auto get_start = std::chrono::high_resolution_clock::now();
            for (size_t i = 0; i < ids.size(); ++i) {
                std::string seq = bench_by_name
                    ? store.get_by_name(names[i])
                    : store.get(ids[i]);
                total_chars += seq.size();
                for (unsigned char c : seq) checksum += c;
            }
            const auto round_get_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::high_resolution_clock::now() - get_start);
            store_get_ns += round_get_ns;
            round_store_get_ns.push_back(round_get_ns);
            round_checksum_ns.push_back(std::chrono::nanoseconds{0});
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    double seconds = std::chrono::duration<double>(end - start).count();
    double store_seconds = store_get_ns.count() > 0
        ? store_get_ns.count() / 1000000000.0
        : seconds;
    double real_get_mbps = (total_chars / (1024.0 * 1024.0)) / (store_seconds + 1e-12);
    double end_to_end_mbps = (total_chars / (1024.0 * 1024.0)) / (seconds + 1e-12);
    PagedProteinAAStore::CacheStats stats = store.stats();
    PagedProteinAAStore::MemoryBreakdown mem = store.memory_breakdown();
    uint64_t prefetch_window_est_bytes = 0;
    if (use_batch_access && prefetch_mb > 0) {
        prefetch_window_est_bytes = stats.prefetch_active_peak_bytes;
    }
    std::string access_mode = "random_no_sort";
    std::string strategy = "random_get";
    if (auto_full_cache) {
        access_mode = "full_cache_random_no_sort";
        strategy = "full_raw_cache";
    } else if (use_batch_access) {
        access_mode = preload_pages >= store.page_count()
            ? "batch_full_cache_no_sort"
            : "batch_page_sorted";
        strategy = prefetch_mb > 0 ? "batch_prefetch" : "batch";
    }

    std::cout << "backend=paged_aastore\n";
    std::cout << "strategy=" << strategy << "\n";
    std::cout << "output_mode=" << (batch_into_mode ? "batch_into" : "strings") << "\n";
    std::cout << "lookup_mode=" << (bench_by_name ? "name" : "id") << "\n";
    std::cout << "store_size=" << store.size() << "\n";
    std::cout << "total_length=" << store.total_length() << "\n";
    std::cout << "page_residues=" << store.page_residues() << "\n";
    std::cout << "page_count=" << store.page_count() << "\n";
    std::cout << "decode_kernel=" << store.decode_kernel_name() << "\n";
    std::cout << "encoding_mode="
              << PagedProteinAAStore::encoding_mode_name(store.encoding_mode())
              << "\n";
    std::cout << "memory_mb=" << memory_mb << "\n";
    std::cout << "cache_policy=" << store.resident_cache_policy_name() << "\n";
    std::cout << "names_loaded=" << (store.names_loaded() ? 1 : 0) << "\n";
    std::cout << "samples=" << samples << "\n";
    std::cout << "rounds=" << benchmark_rounds << "\n";
    std::cout << "output_prepare_ms=" << (output_prepare_ns.count() / 1000000.0) << "\n";
    std::cout << "output_buffer_bytes=" << output_buffer.size() << "\n";
    std::cout << "seed=" << seed << "\n";
    std::cout << "open_meta_ms=" << open_ms << "\n";
    std::cout << "preload_ms=" << preload_ms.count() << "\n";
    std::cout << "preload_pages=" << preload_pages << "\n";
    std::cout << "preload_bytes=" << preload_bytes << "\n";
    std::cout << "preload_threads=" << (auto_preload_cache ? effective_preload_threads : 0) << "\n";
    std::cout << "drop_page_cache=" << (drop_page_cache ? 1 : 0) << "\n";
    std::cout << "drop_page_cache_ms=" << drop_page_cache_ms.count() << "\n";
    std::cout << "auto_full_cache=" << (auto_full_cache ? 1 : 0) << "\n";
    std::cout << "auto_preload_cache=" << (auto_preload_cache ? 1 : 0) << "\n";
    std::cout << "no_preload_cache=" << (no_preload_cache ? 1 : 0) << "\n";
    std::cout << "prefetch_mb=" << prefetch_mb << "\n";
    std::cout << "prefetch_effective_mb=" << (store.prefetch_memory_bytes() / (1024ULL * 1024ULL)) << "\n";
    std::cout << "prefetch_threads=" << (use_batch_access && prefetch_mb > 0 ? store.io_threads() : 0) << "\n";
    std::cout << "decode_threads=" << stats.full_cache_decode_threads << "\n";
    std::cout << "prefetch_depth=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_depth : 1) << "\n";
    std::cout << "prefetch_io=" << PagedProteinAAStore::prefetch_io_backend_name(
        use_batch_access && prefetch_mb > 0
            ? stats.prefetch_io_backend
            : static_cast<uint64_t>(PagedProteinAAStore::PrefetchIoBackend::Pread)) << "\n";
    std::cout << "uring_sqpoll=" << stats.uring_sqpoll << "\n";
    std::cout << "prefetch_completion_batch=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_completion_batch : 0) << "\n";
    std::cout << "prefetch_completion_min=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_completion_min : 0) << "\n";
    std::cout << "single_thread_aio=" << (use_batch_access && prefetch_mb > 0 && caller_only_io ? 1 : 0) << "\n";
    std::cout << "access_mode=" << access_mode << "\n";
    std::cout << "split_timing=" << (use_batch_access ? 1 : 0) << "\n";
    std::cout << "real_get_ms=" << seconds * 1000.0 << "\n";
    std::cout << "store_get_ms=" << (store_get_ns.count() / 1000000.0) << "\n";
    std::cout << "checksum_ms=" << (checksum_ns.count() / 1000000.0) << "\n";
    for (size_t round = 0; round < round_store_get_ns.size(); ++round) {
        std::cout << "round_" << (round + 1) << "_store_get_ms="
                  << (round_store_get_ns[round].count() / 1000000.0) << "\n";
        std::cout << "round_" << (round + 1) << "_checksum_ms="
                  << (round_checksum_ns[round].count() / 1000000.0) << "\n";
    }
    std::cout << "real_get_chars=" << total_chars << "\n";
    std::cout << "checksum_dec=" << checksum << "\n";
    std::cout << "real_get_mbps=" << real_get_mbps << "\n";
    std::cout << "end_to_end_mbps=" << end_to_end_mbps << "\n";
    std::cout << "cache_hits=" << stats.hits << "\n";
    std::cout << "cache_misses=" << stats.misses << "\n";
    std::cout << "cache_evictions=" << stats.evictions << "\n";
    std::cout << "lru_admission_first_misses=" << stats.lru_admission_first_misses << "\n";
    std::cout << "lru_admission_promotions=" << stats.lru_admission_promotions << "\n";
    std::cout << "prefetch_miss_pages=" << stats.prefetch_miss_pages << "\n";
    std::cout << "prefetch_ranges=" << stats.prefetch_ranges << "\n";
    std::cout << "prefetch_read_bytes=" << stats.prefetch_read_bytes << "\n";
    std::cout << "prefetch_workers=" << stats.prefetch_workers << "\n";
    std::cout << "prefetch_background_workers=" << stats.prefetch_background_workers << "\n";
    std::cout << "prefetch_caller_worker=" << stats.prefetch_caller_worker << "\n";
    std::cout << "prefetch_slots=" << stats.prefetch_slots << "\n";
    std::cout << "prefetch_queue_depth=" << stats.prefetch_queue_depth << "\n";
    std::cout << "prefetch_pool_bytes=" << stats.prefetch_pool_bytes << "\n";
    std::cout << "prefetch_active_peak_bytes=" << stats.prefetch_active_peak_bytes << "\n";
    std::cout << "prefetch_active_peak_ranges=" << stats.prefetch_active_peak_ranges << "\n";
    std::cout << "batch_chunk_build_ms=" << (stats.batch_chunk_build_ns / 1000000.0) << "\n";
    std::cout << "batch_sort_ms=" << (stats.batch_sort_ns / 1000000.0) << "\n";
    std::cout << "batch_group_ms=" << (stats.batch_group_ns / 1000000.0) << "\n";
    std::cout << "prefetch_miss_scan_ms=" << (stats.prefetch_miss_scan_ns / 1000000.0) << "\n";
    std::cout << "prefetch_range_build_ms=" << (stats.prefetch_range_build_ns / 1000000.0) << "\n";
    std::cout << "prefetch_setup_ms=" << (stats.prefetch_setup_ns / 1000000.0) << "\n";
    std::cout << "cache_hit_decode_ms=" << (stats.cache_hit_decode_ns / 1000000.0) << "\n";
    std::cout << "no_prefetch_decode_ms=" << (stats.no_prefetch_decode_ns / 1000000.0) << "\n";
    std::cout << "prefetch_io_wall_ms=" << (stats.prefetch_io_wall_ns / 1000000.0) << "\n";
    std::cout << "prefetch_wait_ms=" << (stats.prefetch_wait_ns / 1000000.0) << "\n";
    std::cout << "prefetch_pool_wait_ms=" << (stats.prefetch_pool_wait_ns / 1000000.0) << "\n";
    std::cout << "prefetch_submit_ms_total=" << (stats.prefetch_submit_ns_total / 1000000.0) << "\n";
    std::cout << "prefetch_submit_calls=" << stats.prefetch_submit_calls << "\n";
    std::cout << "prefetch_submitted_requests=" << stats.prefetch_submitted_requests << "\n";
    std::cout << "prefetch_submit_batch_avg="
              << (stats.prefetch_submit_calls == 0
                      ? 0.0
                      : static_cast<double>(stats.prefetch_submitted_requests) /
                            static_cast<double>(stats.prefetch_submit_calls))
              << "\n";
    std::cout << "prefetch_getevents_calls=" << stats.prefetch_getevents_calls << "\n";
    std::cout << "prefetch_completed_events=" << stats.prefetch_completed_events << "\n";
    std::cout << "prefetch_completion_batch_avg="
              << (stats.prefetch_getevents_calls == 0
                      ? 0.0
                      : static_cast<double>(stats.prefetch_completed_events) /
                            static_cast<double>(stats.prefetch_getevents_calls))
              << "\n";
    std::cout << "prefetch_pread_ms_total=" << (stats.prefetch_pread_ns_total / 1000000.0) << "\n";
    std::cout << "prefetch_ready_push_ms=" << (stats.prefetch_ready_push_ns / 1000000.0) << "\n";
    std::cout << "prefetch_release_ms=" << (stats.prefetch_release_ns / 1000000.0) << "\n";
    std::cout << "prefetch_miss_decode_ms=" << (stats.prefetch_miss_decode_ns / 1000000.0) << "\n";
    std::cout << "cache_bytes_after=" << store.cache_bytes() << "\n";
    std::cout << "memory_object_bytes=" << mem.object_bytes << "\n";
    std::cout << "memory_start_positions_bytes=" << mem.start_positions_bytes << "\n";
    std::cout << "memory_start_positions_capacity_bytes=" << mem.start_positions_capacity_bytes << "\n";
    std::cout << "memory_seq_meta_bytes=" << mem.seq_meta_bytes << "\n";
    std::cout << "memory_seq_meta_capacity_bytes=" << mem.seq_meta_capacity_bytes << "\n";
    std::cout << "memory_page_meta_bytes=" << mem.page_meta_bytes << "\n";
    std::cout << "memory_page_cache_payload_bytes=" << mem.page_cache_payload_bytes << "\n";
    std::cout << "memory_page_cache_capacity_bytes=" << mem.page_cache_capacity_bytes << "\n";
    std::cout << "memory_page_cache_entries=" << mem.page_cache_entries << "\n";
    std::cout << "memory_page_cache_direct_entries=" << mem.page_cache_direct_entries << "\n";
    std::cout << "memory_page_cache_direct_vector_bytes=" << mem.page_cache_direct_vector_bytes << "\n";
    std::cout << "memory_page_cache_lru_entries=" << mem.page_cache_lru_entries << "\n";
    std::cout << "memory_page_cache_hash_buckets=" << mem.page_cache_hash_buckets << "\n";
    std::cout << "memory_page_cache_hash_bucket_bytes=" << mem.page_cache_hash_bucket_bytes << "\n";
    std::cout << "memory_page_cache_hash_table_est_bytes=" << mem.page_cache_hash_table_est_bytes << "\n";
    std::cout << "memory_page_cache_lru_est_bytes=" << mem.page_cache_lru_est_bytes << "\n";
    std::cout << "memory_page_cache_admission_bytes=" << mem.page_cache_admission_bytes << "\n";
    std::cout << "memory_prefetch_window_est_bytes=" << prefetch_window_est_bytes << "\n";
    std::cout << "memory_names_blob_bytes=" << mem.names_blob_bytes << "\n";
    std::cout << "memory_names_offsets_bytes=" << mem.names_offsets_bytes << "\n";
    std::cout << "memory_names_hash_slots=" << mem.names_hash_slots << "\n";
    std::cout << "memory_names_hash_est_bytes=" << mem.names_hash_est_bytes << "\n";
    std::cout << "memory_path_string_bytes=" << mem.path_string_bytes << "\n";
    std::cout << "memory_total_resident_est_bytes=" << mem.total_resident_est_bytes << "\n";
    std::cout << "disk_meta_bytes=" << mem.disk_meta_bytes << "\n";
    std::cout << "disk_pages_bytes=" << mem.disk_pages_bytes << "\n";

    return 0;
}
