#include "kseq.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

static uint64_t bytes_read = 0;
static uint64_t read_limit_bytes = 0;

static inline size_t capped_read_size(size_t requested) {
    if (read_limit_bytes == 0) return requested;
    if (bytes_read >= read_limit_bytes) return 0;
    const uint64_t remaining = read_limit_bytes - bytes_read;
    return static_cast<size_t>(
        std::min<uint64_t>(static_cast<uint64_t>(requested), remaining));
}

static inline size_t kseq_file_read(FILE* fp, void* buffer, size_t size) {
    size = capped_read_size(size);
    if (size == 0) return 0;
    size_t read_bytes = std::fread(buffer, 1, size, fp);
    bytes_read += static_cast<uint64_t>(read_bytes);
    return read_bytes;
}

KSEQ_INIT(FILE*, kseq_file_read)

static inline bool fasta_header_name_delim(unsigned char c) {
    return c <= ' ';
}

static inline size_t direct_file_read(FILE* fp, void* buffer, size_t size) {
    size = capped_read_size(size);
    if (size == 0) return 0;
#if defined(__GLIBC__) || defined(__linux__)
    return fread_unlocked(buffer, 1, size, fp);
#else
    return std::fread(buffer, 1, size, fp);
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
    int fd_ = -1;
    uint64_t dropped_until_ = 0;
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

template <typename OnHeader, typename OnResidues, typename OnSequenceEnd>
static bool stream_fasta_direct(FILE* fp,
                                OnHeader&& on_header,
                                OnResidues&& on_residues,
                                OnSequenceEnd&& on_sequence_end,
                                bool drop_input_cache,
                                DirectFastaParseStats* stats = nullptr) {
    constexpr size_t buffer_bytes = 16ULL * 1024ULL * 1024ULL;
    std::vector<char> buffer(buffer_bytes);
    std::string header_name;
    bool sequence_open = false;
    bool in_header = false;
    bool header_name_done = false;
    bool at_line_start = true;
    bool pending_sequence_cr = false;
    FastaInputCacheAdvice input_cache(fp, drop_input_cache);

    auto finish_sequence_if_open = [&]() -> bool {
        if (!sequence_open) return true;
        sequence_open = false;
        return on_sequence_end();
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
                if (!on_residues(&cr, 1)) return false;
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
        return len == 0 || on_residues(data, len);
    };

    while (true) {
        const auto read_start = std::chrono::steady_clock::now();
        size_t read_bytes = direct_file_read(fp, buffer.data(), buffer.size());
        const auto read_end = std::chrono::steady_clock::now();
        if (stats) {
            stats->read_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    read_end - read_start).count());
            ++stats->read_calls;
            if (read_bytes != 0) ++stats->chunks;
        }
        bytes_read += static_cast<uint64_t>(read_bytes);
        const auto advice_start = std::chrono::steady_clock::now();
        input_cache.drop_old_ranges(bytes_read);
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

        const auto parse_start = std::chrono::steady_clock::now();
        size_t pos = 0;
        while (pos < read_bytes) {
            const char* chunk = buffer.data() + pos;
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
    }

    if (in_header && !finish_header()) return false;
    pending_sequence_cr = false;
    return finish_sequence_if_open();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0]
                  << " <input.fa> [--drop-page-cache]"
                     " [--drop-only]"
                     " [--mode parse|names|copy|index|producer]"
                     " [--parser kseq|direct]"
                     " [--max-read-mb N]\n";
        return 1;
    }

    const std::string input_path = argv[1];
    bool drop_page_cache = false;
    bool drop_input_cache = false;
    bool drop_only = false;
    std::string mode = "parse";
    std::string parser = "kseq";
    uint64_t max_read_mb = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--drop-page-cache") {
            drop_page_cache = true;
        } else if (arg == "--drop-input-cache") {
            drop_input_cache = true;
        } else if (arg == "--drop-only") {
            drop_page_cache = true;
            drop_only = true;
        } else if (arg == "--mode" && i + 1 < argc) {
            mode = argv[++i];
        } else if (arg == "--parser" && i + 1 < argc) {
            parser = argv[++i];
        } else if (arg == "--max-read-mb" && i + 1 < argc) {
            max_read_mb = std::stoull(argv[++i]);
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
    if (parser != "kseq" && parser != "direct") {
        std::cerr << "Invalid parser: " << parser << "\n";
        return 1;
    }
    const bool do_names = mode == "names" || mode == "producer";
    const bool do_copy = mode == "copy" || mode == "producer";
    const bool do_index = mode == "index" || mode == "producer";
    read_limit_bytes = max_read_mb * 1024ULL * 1024ULL;

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

    bytes_read = 0;
    uint64_t sequence_count = 0;
    uint64_t total_residues = 0;
    uint64_t total_name_bytes = 0;
    uint64_t checksum = 0;
    uint64_t total_position = 0;
    uint64_t current_sequence_length = 0;
    unsigned char current_first = 0;
    unsigned char current_last = 0;
    bool current_has_residue = false;

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
    bool ok = true;
    DirectFastaParseStats direct_stats;
    if (parser == "kseq") {
        kseq_t* seq = kseq_init(fp);
        if (!seq) {
            std::fclose(fp);
            std::cerr << "failed to initialize kseq\n";
            return 1;
        }
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
        kseq_destroy(seq);
    } else {
        auto on_header = [&](const char* name, size_t name_len) -> bool {
            ++sequence_count;
            total_name_bytes += static_cast<uint64_t>(name_len);
            current_sequence_length = 0;
            current_first = 0;
            current_last = 0;
            current_has_residue = false;
            if (do_names) {
                names_buffer.append(name, name_len);
                names_buffer.push_back('\n');
                if (names_buffer.size() >= names_buffer_limit) {
                    names_buffer.clear();
                }
            }
            return true;
        };
        auto on_residues = [&](const char* data, size_t len) -> bool {
            if (len == 0) return true;
            if (!current_has_residue) {
                current_first = static_cast<unsigned char>(data[0]);
                current_has_residue = true;
            }
            current_last = static_cast<unsigned char>(data[len - 1]);
            current_sequence_length += static_cast<uint64_t>(len);
            total_residues += static_cast<uint64_t>(len);
            if (do_copy) {
                size_t copied = 0;
                while (copied < len) {
                    if (raw_buffer.size() == raw_buffer_limit) {
                        raw_buffer.clear();
                    }
                    size_t take = std::min(raw_buffer_limit - raw_buffer.size(),
                                           len - copied);
                    raw_buffer.insert(raw_buffer.end(), data + copied,
                                      data + copied + take);
                    copied += take;
                }
            }
            return true;
        };
        auto on_sequence_end = [&]() -> bool {
            if (current_has_residue) {
                checksum += current_first;
                checksum += current_last;
            }
            if (do_index) {
                total_position += current_sequence_length;
                index_buffer.push_back(total_position);
                if (index_buffer.size() >= index_buffer_entries) {
                    index_buffer.clear();
                }
            }
            if (mode == "producer" && sequence_count % 10000 == 0) {
                checksum += bytes_read & 1ULL;
            }
            return true;
        };
        ok = stream_fasta_direct(fp, on_header, on_residues, on_sequence_end,
                                 drop_input_cache,
                                 &direct_stats);
    }
    auto end = std::chrono::steady_clock::now();

    std::fclose(fp);
    if (!ok) {
        std::cerr << "failed to parse FASTA\n";
        return 1;
    }

    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double elapsed_seconds = elapsed_ms / 1000.0;
    const double file_mib =
        static_cast<double>(bytes_read) / (1024.0 * 1024.0);
    const double residue_mib =
        static_cast<double>(total_residues) / (1024.0 * 1024.0);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "input=" << input_path << "\n";
    std::cout << "parser=" << parser << "\n";
    std::cout << "mode=" << mode << "\n";
    std::cout << "drop_page_cache=" << (drop_page_cache ? 1 : 0) << "\n";
    std::cout << "drop_input_cache=" << (drop_input_cache ? 1 : 0) << "\n";
    std::cout << "drop_page_cache_ms=" << drop_page_cache_ms << "\n";
    std::cout << "max_read_mb=" << max_read_mb << "\n";
    std::cout << "sequences=" << sequence_count << "\n";
    std::cout << "total_residues=" << total_residues << "\n";
    std::cout << "total_name_bytes=" << total_name_bytes << "\n";
    std::cout << "file_bytes_read=" << bytes_read << "\n";
    std::cout << "elapsed_ms=" << elapsed_ms << "\n";
    if (parser == "direct") {
        std::cout << "direct_read_ms=" << (direct_stats.read_ns / 1000000.0) << "\n";
        std::cout << "direct_parse_ms=" << (direct_stats.parse_ns / 1000000.0) << "\n";
        std::cout << "direct_fadvise_ms=" << (direct_stats.advice_ns / 1000000.0) << "\n";
        std::cout << "direct_read_calls=" << direct_stats.read_calls << "\n";
        std::cout << "direct_chunks=" << direct_stats.chunks << "\n";
        std::cout << "direct_fragments=" << direct_stats.fragments << "\n";
        std::cout << "direct_header_fragments=" << direct_stats.header_fragments << "\n";
        std::cout << "direct_sequence_fragments=" << direct_stats.sequence_fragments << "\n";
        std::cout << "direct_header_fragment_bytes="
                  << direct_stats.header_fragment_bytes << "\n";
        std::cout << "direct_sequence_fragment_bytes="
                  << direct_stats.sequence_fragment_bytes << "\n";
    }
    std::cout << "file_mib_per_sec="
              << (elapsed_seconds > 0.0 ? file_mib / elapsed_seconds : 0.0)
              << "\n";
    std::cout << "residue_mib_per_sec="
              << (elapsed_seconds > 0.0 ? residue_mib / elapsed_seconds : 0.0)
              << "\n";
    std::cout << "checksum=" << checksum << "\n";
    return 0;
}
