#ifndef __PAGED_PROTEIN_AASTORE__
#define __PAGED_PROTEIN_AASTORE__

#include "Packed5BitDecoder.hpp"
#include "Packed5BitEncoder.hpp"
#include "ProteinAAStore.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <time.h>
#ifdef __linux__
#include <linux/aio_abi.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#endif
#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
#include <liburing.h>
#endif
#include <unistd.h>

#if defined(__linux__) && !defined(O_DIRECT)
#define O_DIRECT 040000
#endif

class PagedProteinAAStore {
public:
    enum class PrefetchIoBackend : uint8_t {
        Auto = 0,
        Pread = 1,
        LinuxAio = 2,
        IoUring = 3,
        IoUringFixed = 4,
        IoUringRegisteredFile = 5
    };

    enum class EncodingMode : uint8_t {
        Fast = 0,
        StopAware = 1
    };

    enum class ResidentCachePolicy : uint8_t {
        Lru = 0,
        Prefix = 1
    };

    enum class BuildIoPolicy : uint8_t {
        Balanced = 0,
        ReadFirst = 1
    };

    struct Options {
        // 0 means use enough resident cache capacity for all pages.
        uint64_t memory_bytes = 0;

        // Temporary miss-page buffer for batch prefetch. 0 disables prefetch.
        uint64_t prefetch_bytes = 16ULL * 1024ULL * 1024ULL;

        // Half-memory random access: total I/O streams, including the caller.
        size_t io_threads = 2;

        // Full-memory batch_get_into: decode/access worker threads.
        size_t decode_threads = 1;

        // 0 means auto.
        size_t preload_threads = 0;
        size_t prefetch_depth = 0;

        PrefetchIoBackend io_backend = PrefetchIoBackend::Auto;
        ResidentCachePolicy cache_policy = ResidentCachePolicy::Lru;

        bool auto_preload = true;
        bool uring_sqpoll = false;
        bool caller_runs_async_io = false;

        size_t aio_completion_batch = 64;
        size_t aio_completion_min = 4;

        bool load_names = false;
        std::string names_path;

        static constexpr uint64_t mib(uint64_t value) {
            return value * 1024ULL * 1024ULL;
        }
    };

    PagedProteinAAStore() = default;
    PagedProteinAAStore(const PagedProteinAAStore&) = delete;
    PagedProteinAAStore& operator=(const PagedProteinAAStore&) = delete;
    ~PagedProteinAAStore() {
        close_pages_fd();
    }

    struct CacheStats {
        uint64_t hits = 0;
        uint64_t misses = 0;
        uint64_t evictions = 0;
        uint64_t lru_admission_first_misses = 0;
        uint64_t lru_admission_promotions = 0;
        uint64_t prefetch_miss_pages = 0;
        uint64_t prefetch_ranges = 0;
        uint64_t prefetch_read_bytes = 0;
        uint64_t prefetch_workers = 0;
        uint64_t prefetch_background_workers = 0;
        uint64_t prefetch_caller_worker = 0;
        uint64_t prefetch_slots = 0;
        uint64_t prefetch_queue_depth = 0;
        uint64_t prefetch_depth = 1;
        uint64_t prefetch_io_backend = static_cast<uint64_t>(PrefetchIoBackend::Pread);
        uint64_t uring_sqpoll = 0;
        uint64_t prefetch_completion_batch = 0;
        uint64_t prefetch_completion_min = 0;
        uint64_t prefetch_pool_bytes = 0;
        uint64_t prefetch_active_peak_bytes = 0;
        uint64_t prefetch_active_peak_ranges = 0;
        uint64_t prefetch_io_wall_ns = 0;
        uint64_t prefetch_wait_ns = 0;
        uint64_t prefetch_pool_wait_ns = 0;
        uint64_t prefetch_submit_ns_total = 0;
        uint64_t prefetch_submit_calls = 0;
        uint64_t prefetch_submitted_requests = 0;
        uint64_t prefetch_getevents_calls = 0;
        uint64_t prefetch_completed_events = 0;
        uint64_t prefetch_pread_ns_total = 0;
        uint64_t prefetch_ready_push_ns = 0;
        uint64_t prefetch_release_ns = 0;
        uint64_t prefetch_miss_decode_ns = 0;
        uint64_t cache_hit_decode_ns = 0;
        uint64_t no_prefetch_decode_ns = 0;
        uint64_t batch_chunk_build_ns = 0;
        uint64_t batch_sort_ns = 0;
        uint64_t batch_group_ns = 0;
        uint64_t prefetch_miss_scan_ns = 0;
        uint64_t prefetch_range_build_ns = 0;
        uint64_t prefetch_setup_ns = 0;
        uint64_t full_cache_decode_threads = 1;
    };

    struct MemoryBreakdown {
        uint64_t object_bytes = 0;
        uint64_t start_positions_bytes = 0;
        uint64_t start_positions_capacity_bytes = 0;
        uint64_t seq_meta_bytes = 0;
        uint64_t seq_meta_capacity_bytes = 0;
        uint64_t page_meta_bytes = 0;
        uint64_t page_cache_payload_bytes = 0;
        uint64_t page_cache_capacity_bytes = 0;
        uint64_t page_cache_entries = 0;
        uint64_t page_cache_direct_entries = 0;
        uint64_t page_cache_direct_vector_bytes = 0;
        uint64_t page_cache_lru_entries = 0;
        uint64_t page_cache_hash_buckets = 0;
        uint64_t page_cache_hash_bucket_bytes = 0;
        uint64_t page_cache_hash_table_est_bytes = 0;
        uint64_t page_cache_lru_est_bytes = 0;
        uint64_t page_cache_admission_bytes = 0;
        uint64_t path_string_bytes = 0;
        uint64_t names_blob_bytes = 0;
        uint64_t names_offsets_bytes = 0;
        uint64_t names_hash_slots = 0;
        uint64_t names_hash_est_bytes = 0;
        uint64_t total_resident_est_bytes = 0;
        uint64_t disk_meta_bytes = 0;
        uint64_t disk_pages_bytes = 0;
    };

    struct PageView {
        const sdsl::int_vector<5>* codes = nullptr;
        uint64_t base = 0;
        uint64_t size = 0;
        const uint8_t* packed = nullptr;

        bool valid() const {
            return packed != nullptr || codes != nullptr;
        }

        uint8_t operator[](uint64_t offset) const {
            if (packed != nullptr) {
                uint64_t bit_pos = offset * 5;
                size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
                int shift = static_cast<int>(bit_pos & 7);
                uint16_t value = static_cast<uint16_t>(packed[byte_pos] >> shift);
                if (shift > 3) {
                    value |= static_cast<uint16_t>(packed[byte_pos + 1]) << (8 - shift);
                }
                return static_cast<uint8_t>(value & 0x1F);
            }
            return static_cast<uint8_t>((*codes)[static_cast<size_t>(base + offset)]);
        }
    };

    struct BuildTask {
        uint64_t task_id = 0;
        uint64_t page_residues = 0;
        EncodingMode encoding_mode = EncodingMode::StopAware;
        std::unique_ptr<char[]> raw;
        size_t raw_size = 0;
    };

    struct BuildTaskResult {
        uint64_t task_id = 0;
        std::vector<uint8_t> packed_block;
    };

    struct BuildStats {
        uint64_t read_ns = 0;
        uint64_t raw_copy_ns = 0;
        uint64_t producer_ns = 0;
        uint64_t enqueue_wait_ns = 0;
        uint64_t pack_ns = 0;
        uint64_t write_ns = 0;
        uint64_t page_writeback_ns = 0;
        uint64_t page_close_ns = 0;
        uint64_t index_write_ns = 0;
        uint64_t drain_ns = 0;
        uint64_t finalize_ns = 0;
        uint64_t page_write_buffer_bytes = 0;
        uint64_t page_writeback_step_bytes = 0;
        uint64_t page_output_direct = 0;
        uint64_t page_output_file_bytes = 0;
        uint64_t raw_bytes = 0;
        uint64_t packed_bytes = 0;
        uint64_t page_writeback_bytes = 0;
        uint64_t page_writeback_calls = 0;
        uint64_t build_result_buffer_limit_bytes = 0;
        uint64_t build_result_buffer_peak_bytes = 0;
        uint64_t build_result_buffer_wait_ns = 0;
        uint64_t index_bytes = 0;
        uint64_t index_flushes = 0;
        uint64_t index_buffer_bytes = 0;
        uint64_t pages = 0;
        uint64_t tasks = 0;
    };

    static constexpr uint64_t default_build_write_buffer_bytes() {
        return 4ULL * 1024ULL * 1024ULL;
    }

    struct NameIndexStats {
        uint64_t read_ns = 0;
        uint64_t scan_offsets_ns = 0;
        uint64_t build_hash_ns = 0;
        uint64_t write_ns = 0;
        uint64_t names_bytes = 0;
        uint64_t names = 0;
        uint64_t hash_slots = 0;
        uint64_t index_bytes = 0;
    };

    class StreamingMetaWriter {
    public:
        static constexpr size_t default_buffer_bytes = 8ULL * 1024ULL * 1024ULL;

        explicit StreamingMetaWriter(const std::string& final_path,
                                     size_t buffer_bytes = default_buffer_bytes,
                                     EncodingMode encoding_mode = EncodingMode::StopAware)
            : final_path_(final_path),
              temp_path_(final_path + ".tmp"),
              encoding_mode_(encoding_mode),
              version_(encoding_mode == EncodingMode::StopAware ? 4 : 3),
              header_bytes_(8 + (version_ == 4 ? 7 : 6) * sizeof(uint64_t)),
              buffer_limit_entries_(std::max<size_t>(
                  1, buffer_bytes / sizeof(uint64_t))) {
            buffer_.reserve(buffer_limit_entries_);
            out_.open(temp_path_,
                      std::ios::binary | std::ios::in | std::ios::out |
                          std::ios::trunc);
            if (!out_) return;

            std::vector<char> placeholder(header_bytes_, 0);
            out_.write(placeholder.data(),
                       static_cast<std::streamsize>(placeholder.size()));
        }

        StreamingMetaWriter(const StreamingMetaWriter&) = delete;
        StreamingMetaWriter& operator=(const StreamingMetaWriter&) = delete;

        ~StreamingMetaWriter() {
            if (out_.is_open()) out_.close();
            if (!finalized_) {
                std::error_code ec;
                std::filesystem::remove(temp_path_, ec);
            }
        }

        bool good() const {
            return static_cast<bool>(out_);
        }

        bool append(uint64_t position) {
            if (!out_ || (position_count_ != 0 && position < last_position_)) {
                return false;
            }
            buffer_.push_back(position);
            last_position_ = position;
            ++position_count_;
            if (buffer_.size() >= buffer_limit_entries_) {
                return flush();
            }
            return true;
        }

        bool finalize(uint64_t n,
                      uint64_t total_length,
                      uint64_t page_residues,
                      uint64_t page_count) {
            if (!out_ || position_count_ != n + 1 ||
                last_position_ != total_length || !flush()) {
                return false;
            }

            auto write_start = std::chrono::steady_clock::now();
            out_.seekp(0, std::ios::beg);
            char magic[8] = {'A', 'A', 'P', 'A', 'G', 'E',
                             static_cast<char>('0' + version_), '\0'};
            uint64_t version = version_;
            uint64_t seq_count = n;
            uint64_t encoding_flags =
                encoding_mode_ == EncodingMode::StopAware ? 1ULL : 0ULL;
            out_.write(magic, sizeof(magic));
            write_value(version);
            write_value(n);
            write_value(total_length);
            write_value(page_residues);
            write_value(seq_count);
            write_value(page_count);
            if (version_ == 4) {
                write_value(encoding_flags);
            }
            out_.flush();
            write_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_start).count());
            if (!out_) return false;
            out_.close();
            if (!out_) return false;

            std::error_code ec;
            std::filesystem::rename(temp_path_, final_path_, ec);
            if (ec) {
                std::error_code remove_ec;
                std::filesystem::remove(final_path_, remove_ec);
                ec.clear();
                std::filesystem::rename(temp_path_, final_path_, ec);
            }
            if (ec) return false;
            finalized_ = true;
            return true;
        }

        uint64_t write_ns() const { return write_ns_; }
        uint64_t bytes_written() const {
            return position_count_ * sizeof(uint64_t);
        }
        uint64_t flushes() const { return flushes_; }
        uint64_t buffer_bytes() const {
            return buffer_limit_entries_ * sizeof(uint64_t);
        }

    private:
        template <typename T>
        void write_value(const T& value) {
            out_.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        bool flush() {
            if (buffer_.empty()) return true;
            auto write_start = std::chrono::steady_clock::now();
            out_.write(
                reinterpret_cast<const char*>(buffer_.data()),
                static_cast<std::streamsize>(buffer_.size() * sizeof(uint64_t)));
            write_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_start).count());
            if (!out_) return false;
            ++flushes_;
            buffer_.clear();
            return true;
        }

        std::string final_path_;
        std::string temp_path_;
        EncodingMode encoding_mode_ = EncodingMode::StopAware;
        uint64_t version_ = 3;
        size_t header_bytes_ = 8 + 6 * sizeof(uint64_t);
        std::fstream out_;
        std::vector<uint64_t> buffer_;
        size_t buffer_limit_entries_ = 0;
        uint64_t position_count_ = 0;
        uint64_t last_position_ = 0;
        uint64_t write_ns_ = 0;
        uint64_t flushes_ = 0;
        bool finalized_ = false;
    };

    class BufferedPageWriter {
    public:
        static constexpr size_t default_buffer_bytes = 4ULL * 1024ULL * 1024ULL;
        static constexpr uint64_t default_writeback_step_bytes =
            512ULL * 1024ULL * 1024ULL;

        explicit BufferedPageWriter(const std::string& path,
                                    size_t buffer_bytes = default_buffer_bytes,
                                    uint64_t writeback_step_bytes =
                                        default_writeback_step_bytes,
                                    bool direct_output = false)
            : buffer_limit_bytes_(std::max<size_t>(1, buffer_bytes)),
              writeback_step_bytes_(writeback_step_bytes),
              direct_output_(direct_output) {
            if (direct_output_) {
                buffer_limit_bytes_ = align_up(
                    std::max<size_t>(direct_alignment_, buffer_limit_bytes_),
                    direct_alignment_);
                void* ptr = nullptr;
                if (::posix_memalign(&ptr, direct_alignment_, buffer_limit_bytes_) != 0) {
                    failed_ = true;
                    return;
                }
                direct_buffer_.reset(static_cast<uint8_t*>(ptr));
            } else {
                buffer_.reserve(buffer_limit_bytes_);
            }
            int flags = O_WRONLY | O_CREAT | O_TRUNC;
#if defined(O_DIRECT)
            if (direct_output_) flags |= O_DIRECT;
#endif
            fd_ = ::open(path.c_str(), flags, 0666);
#if defined(POSIX_FADV_SEQUENTIAL)
            if (fd_ >= 0) {
                (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
            }
#endif
        }

        BufferedPageWriter(const BufferedPageWriter&) = delete;
        BufferedPageWriter& operator=(const BufferedPageWriter&) = delete;

        ~BufferedPageWriter() {
            if (fd_ >= 0) ::close(fd_);
        }

        bool good() const {
            return fd_ >= 0 && !failed_;
        }

        bool append(const uint8_t* data, size_t len) {
            if (!good() || (data == nullptr && len != 0)) return false;
            if (direct_output_) return append_direct(data, len);
            size_t copied = 0;
            while (copied < len) {
                if (buffer_.empty() && len - copied >= buffer_limit_bytes_) {
                    if (!write_chunk(data + copied, len - copied)) return false;
                    break;
                }
                size_t space = buffer_limit_bytes_ - buffer_.size();
                size_t take = std::min(space, len - copied);
                buffer_.insert(buffer_.end(), data + copied, data + copied + take);
                copied += take;
                if (buffer_.size() == buffer_limit_bytes_ && !flush()) return false;
            }
            return true;
        }

        bool flush() {
            if (!good()) return false;
            if (direct_output_) return flush_direct();
            if (buffer_.empty()) return true;
            if (!write_chunk(buffer_.data(), buffer_.size())) return false;
            buffer_.clear();
            return true;
        }

        bool close() {
            if (fd_ < 0) return !failed_;
            if (!flush()) return false;
            issue_background_writeback(true);
            auto close_start = std::chrono::steady_clock::now();
            const int close_result = ::close(fd_);
            close_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - close_start).count());
            fd_ = -1;
            if (close_result != 0) {
                failed_ = true;
                return false;
            }
            return true;
        }

        uint64_t write_ns() const { return write_ns_; }
        uint64_t writeback_ns() const { return writeback_ns_; }
        uint64_t writeback_bytes() const { return writeback_bytes_; }
        uint64_t writeback_calls() const { return writeback_calls_; }
        uint64_t close_ns() const { return close_ns_; }
        uint64_t buffer_bytes() const {
            return static_cast<uint64_t>(buffer_limit_bytes_);
        }
        uint64_t writeback_step_bytes() const { return writeback_step_bytes_; }
        bool direct_output() const { return direct_output_; }
        uint64_t file_bytes_written() const { return bytes_written_; }

    private:
        struct FreeDeleter {
            void operator()(uint8_t* ptr) const {
                std::free(ptr);
            }
        };

        static constexpr size_t direct_alignment_ = 4096;

        static size_t align_up(size_t value, size_t alignment) {
            return ((value + alignment - 1) / alignment) * alignment;
        }

        bool append_direct(const uint8_t* data, size_t len) {
            size_t copied = 0;
            while (copied < len) {
                size_t space = buffer_limit_bytes_ - direct_buffer_size_;
                size_t take = std::min(space, len - copied);
                std::memcpy(direct_buffer_.get() + direct_buffer_size_,
                            data + copied, take);
                direct_buffer_size_ += take;
                copied += take;
                if (direct_buffer_size_ == buffer_limit_bytes_ && !flush_direct()) {
                    return false;
                }
            }
            return true;
        }

        bool flush_direct() {
            if (direct_buffer_size_ == 0) return true;
            size_t write_bytes = direct_buffer_size_;
            size_t padded_bytes = align_up(write_bytes, direct_alignment_);
            if (padded_bytes > buffer_limit_bytes_) {
                failed_ = true;
                return false;
            }
            if (padded_bytes != write_bytes) {
                std::memset(direct_buffer_.get() + write_bytes, 0,
                            padded_bytes - write_bytes);
            }
            if (!write_chunk(direct_buffer_.get(), padded_bytes)) return false;
            direct_buffer_size_ = 0;
            return true;
        }

        bool write_chunk(const uint8_t* data, size_t len) {
            const uint64_t chunk_offset = bytes_written_;
            size_t written = 0;
            auto write_start = std::chrono::steady_clock::now();
            while (written < len) {
                ssize_t result = ::write(fd_, data + written, len - written);
                if (result <= 0) {
                    failed_ = true;
                    return false;
                }
                written += static_cast<size_t>(result);
            }
            bytes_written_ = chunk_offset + static_cast<uint64_t>(written);
            write_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_start).count());
            issue_background_writeback(false);
            return true;
        }

        void issue_background_writeback(bool force) {
#if defined(__linux__) && defined(SYS_sync_file_range)
#ifndef SYNC_FILE_RANGE_WRITE
#define SYNC_FILE_RANGE_WRITE 2
#endif
            if (fd_ < 0 || writeback_disabled_) return;
            if (direct_output_) return;
            if (writeback_step_bytes_ == 0) return;
            const uint64_t pending = bytes_written_ - writeback_submitted_until_;
            if (pending == 0) return;
            if (!force && pending < writeback_step_bytes_) return;
            auto start = std::chrono::steady_clock::now();
            const long rc = ::syscall(SYS_sync_file_range, fd_,
                                      static_cast<off_t>(writeback_submitted_until_),
                                      static_cast<off_t>(pending),
                                      SYNC_FILE_RANGE_WRITE);
            writeback_ns_ += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count());
            if (rc == 0) {
                writeback_submitted_until_ = bytes_written_;
                writeback_bytes_ += pending;
                ++writeback_calls_;
                return;
            }
            if (errno == ENOSYS || errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP) {
                writeback_disabled_ = true;
            }
#else
            (void)force;
#endif
        }

        int fd_ = -1;
        std::vector<uint8_t> buffer_;
        std::unique_ptr<uint8_t, FreeDeleter> direct_buffer_;
        size_t buffer_limit_bytes_ = 0;
        size_t direct_buffer_size_ = 0;
        uint64_t writeback_step_bytes_ = default_writeback_step_bytes;
        uint64_t bytes_written_ = 0;
        uint64_t writeback_submitted_until_ = 0;
        uint64_t write_ns_ = 0;
        uint64_t writeback_ns_ = 0;
        uint64_t writeback_bytes_ = 0;
        uint64_t writeback_calls_ = 0;
        uint64_t close_ns_ = 0;
        bool failed_ = false;
        bool writeback_disabled_ = false;
        bool direct_output_ = false;
    };

    static bool build_from_store(const std::string& source_store_path,
                                 const std::string& output_dir,
                                 uint64_t page_residues,
                                 const std::function<void(uint64_t, uint64_t)>& progress = {}) {
        if (page_residues == 0) return false;

        ProteinAAStore source;
        if (!source.load(source_store_path)) return false;

        namespace fs = std::filesystem;
        fs::create_directories(output_dir);
        const fs::path out_dir(output_dir);
        const fs::path pages_path = out_dir / "pages.bin";
        const fs::path meta_path = out_dir / "meta.bin";

        BufferedPageWriter pages_out(pages_path.string());
        if (!pages_out.good()) return false;

        StreamingMetaWriter meta_writer(meta_path.string());
        if (!meta_writer.good() || !meta_writer.append(0)) return false;

        std::vector<uint8_t> page_codes;
        std::vector<uint8_t> packed;
        std::vector<uint8_t> seq_raw;
        page_codes.reserve(static_cast<size_t>(page_residues));

        uint64_t page_count = 0;
        uint64_t total_length = 0;
        auto flush_page = [&]() -> bool {
            if (page_codes.empty()) return true;
            pack_5bit(page_codes, packed);
            if (!pages_out.append(packed.data(), packed.size())) return false;
            ++page_count;
            page_codes.clear();
            return true;
        };

        const uint64_t total_sequences = static_cast<uint64_t>(source.size());
        for (size_t seq_id = 0; seq_id < source.size(); ++seq_id) {
            size_t seq_len = source.get_raw(seq_id, seq_raw);
            if (page_codes.size() == page_residues) {
                if (!flush_page()) return false;
            }

            size_t copied = 0;
            while (copied < seq_len) {
                if (page_codes.size() == page_residues) {
                    if (!flush_page()) return false;
                }

                size_t space = static_cast<size_t>(page_residues - page_codes.size());
                size_t take = std::min(space, seq_len - copied);
                page_codes.insert(page_codes.end(),
                                  seq_raw.begin() + static_cast<std::ptrdiff_t>(copied),
                                  seq_raw.begin() + static_cast<std::ptrdiff_t>(copied + take));
                copied += take;
            }
            total_length += static_cast<uint64_t>(seq_len);
            if (!meta_writer.append(total_length)) return false;
            if (progress) progress(static_cast<uint64_t>(seq_id + 1), total_sequences);
        }
        if (!flush_page()) return false;
        if (!pages_out.close()) return false;

        return meta_writer.finalize(source.size(), total_length, page_residues,
                                    page_count);
    }

    template <typename NextSequence>
    static bool build_from_sequences(const std::string& output_dir,
                                     uint64_t page_residues,
                                     NextSequence&& next_sequence,
                                     EncodingMode encoding_mode = EncodingMode::StopAware) {
        std::string seq_storage;
        auto next_view = [&](std::string_view& seq) -> bool {
            if (!next_sequence(seq_storage)) return false;
            seq = std::string_view(seq_storage.data(), seq_storage.size());
            return true;
        };
        return build_from_sequence_views(output_dir, page_residues, next_view,
                                         nullptr, encoding_mode);
    }

    template <typename NextSequenceView>
    static bool build_from_sequence_views(const std::string& output_dir,
                                          uint64_t page_residues,
                                          NextSequenceView&& next_sequence,
                                          BuildStats* stats = nullptr,
                                          EncodingMode encoding_mode = EncodingMode::StopAware) {
        auto produce_sequence_views = [&](auto&& begin_sequence,
                                          auto&& append_residues,
                                          auto&& end_sequence) -> bool {
            std::string_view seq;
            while (true) {
                auto read_start = std::chrono::steady_clock::now();
                bool has_sequence = next_sequence(seq);
                if (stats) {
                    stats->read_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - read_start).count());
                }
                if (!has_sequence) return true;
                if (!begin_sequence() ||
                    !append_residues(seq.data(), seq.size()) ||
                    !end_sequence()) {
                    return false;
                }
            }
        };
        return build_from_sequence_stream(output_dir, page_residues,
                                          produce_sequence_views, stats,
                                          encoding_mode);
    }

    template <typename ProduceStream>
    static bool build_from_sequence_stream(const std::string& output_dir,
                                           uint64_t page_residues,
                                           ProduceStream&& produce_stream,
                                           BuildStats* stats = nullptr,
                                           EncodingMode encoding_mode = EncodingMode::StopAware,
                                           uint64_t page_write_buffer_bytes =
                                               BufferedPageWriter::default_buffer_bytes,
                                           uint64_t page_writeback_step_bytes =
                                               BufferedPageWriter::default_writeback_step_bytes,
                                           bool direct_output = false) {
        if (stats) *stats = BuildStats{};
        if (page_residues == 0) return false;
        if (page_write_buffer_bytes == 0) {
            page_write_buffer_bytes = BufferedPageWriter::default_buffer_bytes;
        }

        namespace fs = std::filesystem;
        fs::create_directories(output_dir);
        const fs::path out_dir(output_dir);
        const fs::path pages_path = out_dir / "pages.bin";
        const fs::path meta_path = out_dir / "meta.bin";

        BufferedPageWriter pages_out(pages_path.string(),
                                     static_cast<size_t>(page_write_buffer_bytes),
                                     page_writeback_step_bytes,
                                     direct_output);
        if (!pages_out.good()) return false;

        StreamingMetaWriter meta_writer(meta_path.string(),
                                        StreamingMetaWriter::default_buffer_bytes,
                                        encoding_mode);
        if (!meta_writer.good() || !meta_writer.append(0)) return false;
        const uint64_t packed_page_bytes = packed_bytes_for_residues(page_residues);
        std::vector<uint8_t> page_packed(static_cast<size_t>(packed_page_bytes), 0);
        uint64_t page_fill = 0;

        uint64_t sequence_count = 0;
        uint64_t page_count = 0;
        uint64_t total_length = 0;
        auto flush_page = [&]() -> bool {
            if (page_fill == 0) return true;
            uint64_t bytes_to_write = packed_bytes_for_residues(page_fill);
            if (!pages_out.append(page_packed.data(), static_cast<size_t>(bytes_to_write))) {
                return false;
            }
            if (stats) {
                ++stats->pages;
                stats->raw_bytes += page_fill;
                stats->packed_bytes += bytes_to_write;
            }
            ++page_count;
            page_fill = 0;
            std::fill(page_packed.begin(), page_packed.end(), 0);
            return true;
        };

        uint64_t current_sequence_length = 0;
        bool sequence_open = false;
        auto begin_sequence = [&]() -> bool {
            if (sequence_open) return false;
            sequence_open = true;
            current_sequence_length = 0;
            return true;
        };
        auto append_residues = [&](const char* data, size_t len) -> bool {
            if (!sequence_open || (data == nullptr && len != 0)) return false;
            size_t copied = 0;
            while (copied < len) {
                if (page_fill == page_residues) {
                    if (!flush_page()) return false;
                }
                size_t space = static_cast<size_t>(page_residues - page_fill);
                size_t take = std::min(space, len - copied);
                append_ascii_5bit(page_packed, page_fill, data + copied, take,
                                  encoding_mode);
                copied += take;
            }
            current_sequence_length += static_cast<uint64_t>(len);
            return true;
        };
        auto end_sequence = [&]() -> bool {
            if (!sequence_open) return false;
            total_length += current_sequence_length;
            ++sequence_count;
            sequence_open = false;
            return meta_writer.append(total_length);
        };

        auto producer_start = std::chrono::steady_clock::now();
        bool producer_ok = false;
        try {
            producer_ok = produce_stream(begin_sequence, append_residues, end_sequence);
        } catch (...) {
            producer_ok = false;
        }
        if (!producer_ok || sequence_open) return false;
        if (stats) {
            stats->producer_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - producer_start).count());
        }

        if (!flush_page()) return false;
        if (!pages_out.close()) return false;

        bool meta_ok = meta_writer.finalize(sequence_count, total_length,
                                            page_residues, page_count);
        if (stats) {
            stats->write_ns = pages_out.write_ns();
            stats->page_write_buffer_bytes = pages_out.buffer_bytes();
            stats->page_writeback_step_bytes = pages_out.writeback_step_bytes();
            stats->page_output_direct = pages_out.direct_output() ? 1 : 0;
            stats->page_output_file_bytes = pages_out.file_bytes_written();
            stats->page_writeback_ns = pages_out.writeback_ns();
            stats->page_writeback_bytes = pages_out.writeback_bytes();
            stats->page_writeback_calls = pages_out.writeback_calls();
            stats->page_close_ns = pages_out.close_ns();
            stats->index_write_ns = meta_writer.write_ns();
            stats->index_bytes = meta_writer.bytes_written();
            stats->index_flushes = meta_writer.flushes();
            stats->index_buffer_bytes = meta_writer.buffer_bytes();
        }
        return meta_ok;
    }

    template <typename NextSequenceView>
    static bool build_from_sequence_views_parallel(const std::string& output_dir,
                                                   uint64_t page_residues,
                                                   NextSequenceView&& next_sequence,
                                                   size_t thread_count,
                                                   BuildStats* stats = nullptr,
                                                   EncodingMode encoding_mode = EncodingMode::StopAware) {
        if (thread_count == 0) {
            thread_count = std::max(1u, std::thread::hardware_concurrency());
        }
        if (thread_count <= 1) {
            return build_from_sequence_views(output_dir, page_residues,
                                             next_sequence, stats,
                                             encoding_mode);
        }

        auto produce_sequence_views = [&](auto&& begin_sequence,
                                          auto&& append_residues,
                                          auto&& end_sequence) -> bool {
            uint64_t read_calls = 0;
            uint64_t sampled_read_calls = 0;
            uint64_t sampled_read_ns = 0;
            uint64_t sampled_copy_bytes = 0;
            uint64_t sampled_copy_ns = 0;
            constexpr uint64_t timing_sample_stride = 1024;

            std::string_view seq;
            while (true) {
                const bool sample_read =
                    stats && (read_calls % timing_sample_stride == 0);
                std::chrono::steady_clock::time_point read_start;
                if (sample_read) read_start = std::chrono::steady_clock::now();
                bool has_sequence = next_sequence(seq);
                ++read_calls;
                if (sample_read) {
                    sampled_read_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - read_start).count());
                    ++sampled_read_calls;
                }
                if (!has_sequence) break;

                const bool sample_copy =
                    stats && ((read_calls - 1) % timing_sample_stride == 0);
                std::chrono::steady_clock::time_point copy_start;
                if (sample_copy) copy_start = std::chrono::steady_clock::now();
                if (!begin_sequence() ||
                    !append_residues(seq.data(), seq.size()) ||
                    !end_sequence()) {
                    return false;
                }
                if (sample_copy) {
                    sampled_copy_ns += static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - copy_start).count());
                    sampled_copy_bytes += static_cast<uint64_t>(seq.size());
                }
            }

            if (stats) {
                if (sampled_read_calls != 0) {
                    stats->read_ns = static_cast<uint64_t>(
                        static_cast<long double>(sampled_read_ns) *
                        static_cast<long double>(read_calls) /
                        static_cast<long double>(sampled_read_calls));
                }
                if (sampled_copy_bytes != 0 && stats->raw_bytes != 0) {
                    stats->raw_copy_ns = static_cast<uint64_t>(
                        static_cast<long double>(sampled_copy_ns) *
                        static_cast<long double>(stats->raw_bytes) /
                        static_cast<long double>(sampled_copy_bytes));
                }
            }
            return true;
        };

        return build_from_sequence_stream_parallel(
            output_dir, page_residues, produce_sequence_views, thread_count,
            stats, encoding_mode);
    }

    // The producer hands residue chunks to this pipeline while keeping ownership of its input buffer.
    // This permits FASTA parsers to append directly to build tasks without assembling each sequence.
    template <typename ProduceStream>
    static bool build_from_sequence_stream_parallel(const std::string& output_dir,
                                                    uint64_t page_residues,
                                                    ProduceStream&& produce_stream,
                                                    size_t thread_count,
                                                    BuildStats* stats = nullptr,
                                                    EncodingMode encoding_mode = EncodingMode::StopAware,
                                                    BuildIoPolicy io_policy = BuildIoPolicy::Balanced,
                                                    uint64_t write_buffer_limit_bytes =
                                                        default_build_write_buffer_bytes(),
                                                    uint64_t page_write_buffer_bytes =
                                                        BufferedPageWriter::default_buffer_bytes,
                                                    uint64_t page_writeback_step_bytes =
                                                        BufferedPageWriter::default_writeback_step_bytes,
                                                    bool direct_output = false) {
        if (stats) *stats = BuildStats{};
        if (thread_count == 0) return false;
        if (page_residues == 0) return false;
        const bool read_first_io = io_policy == BuildIoPolicy::ReadFirst;
        if (write_buffer_limit_bytes == 0) {
            write_buffer_limit_bytes = default_build_write_buffer_bytes();
        }
        if (page_write_buffer_bytes == 0) {
            page_write_buffer_bytes = BufferedPageWriter::default_buffer_bytes;
        }

        namespace fs = std::filesystem;
        fs::create_directories(output_dir);
        const fs::path out_dir(output_dir);
        const fs::path pages_path = out_dir / "pages.bin";
        const fs::path meta_path = out_dir / "meta.bin";

        BufferedPageWriter pages_out(pages_path.string(),
                                     static_cast<size_t>(page_write_buffer_bytes),
                                     page_writeback_step_bytes,
                                     direct_output);
        if (!pages_out.good()) return false;
        StreamingMetaWriter meta_writer(meta_path.string(),
                                        StreamingMetaWriter::default_buffer_bytes,
                                        encoding_mode);
        if (!meta_writer.good() || !meta_writer.append(0)) return false;

        std::deque<BuildTask> task_queue;
        std::map<uint64_t, BuildTaskResult> completed;
        std::mutex mutex;
        std::condition_variable task_cv;
        std::condition_variable result_cv;
        std::condition_variable space_cv;
        std::condition_variable raw_buffer_cv;
        std::condition_variable result_space_cv;
        std::exception_ptr error = nullptr;
        bool producer_done = false;
        bool stop_workers = false;
        size_t active_tasks = 0;
        uint64_t total_tasks = 0;
        uint64_t completed_bytes = 0;
        uint64_t completed_peak_bytes = 0;
        std::atomic<uint64_t> pack_ns{0};
        std::atomic<uint64_t> result_buffer_wait_ns{0};

        const size_t target_raw_batch_bytes = 4ULL * 1024ULL * 1024ULL;
        const size_t pages_per_task = std::max<size_t>(
            1, target_raw_batch_bytes / std::max<uint64_t>(page_residues, 1));
        const size_t max_inflight_tasks = std::max<size_t>(4, thread_count * 2);
        const size_t task_raw_capacity = static_cast<size_t>(
            std::min<uint64_t>(
                static_cast<uint64_t>(pages_per_task) * page_residues,
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())));
        const size_t raw_pool_size = max_inflight_tasks + 1;
        std::vector<std::unique_ptr<char[]>> free_raw_buffers;
        free_raw_buffers.reserve(raw_pool_size);
        for (size_t i = 0; i < raw_pool_size; ++i) {
            free_raw_buffers.emplace_back(new char[task_raw_capacity]);
        }

        auto set_error = [&](std::exception_ptr ex) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!error) error = ex;
            stop_workers = true;
            producer_done = true;
        };

        auto acquire_raw_buffer = [&]() -> std::unique_ptr<char[]> {
            std::unique_lock<std::mutex> lock(mutex);
            raw_buffer_cv.wait(lock, [&]() {
                return error || !free_raw_buffers.empty();
            });
            if (error) return nullptr;
            std::unique_ptr<char[]> buffer = std::move(free_raw_buffers.back());
            free_raw_buffers.pop_back();
            return buffer;
        };

        auto release_raw_buffer = [&](std::unique_ptr<char[]> buffer) {
            if (!buffer) return;
            {
                std::lock_guard<std::mutex> lock(mutex);
                free_raw_buffers.push_back(std::move(buffer));
            }
            raw_buffer_cv.notify_one();
        };

        auto inflight_locked = [&]() -> size_t {
            return task_queue.size() + active_tasks +
                   (read_first_io ? size_t{0} : completed.size());
        };

        auto enqueue_task = [&](BuildTask&& task) -> bool {
            auto wait_start = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(mutex);
            space_cv.wait(lock, [&]() {
                return error || inflight_locked() < max_inflight_tasks;
            });
            if (stats) {
                stats->enqueue_wait_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - wait_start).count());
            }
            if (error) return false;
            task_queue.push_back(std::move(task));
            ++total_tasks;
            task_cv.notify_one();
            return true;
        };

        std::vector<std::thread> workers;
        workers.reserve(thread_count);
        for (size_t worker = 0; worker < thread_count; ++worker) {
            workers.emplace_back([&]() {
                while (true) {
                    BuildTask task;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        task_cv.wait(lock, [&]() {
                            return stop_workers || error || producer_done || !task_queue.empty();
                        });
                        if ((stop_workers || error) && task_queue.empty()) return;
                        if (task_queue.empty()) {
                            if (producer_done) return;
                            continue;
                        }
                        task = std::move(task_queue.front());
                        task_queue.pop_front();
                        ++active_tasks;
                    }

                    try {
                        auto pack_start = std::chrono::steady_clock::now();
                        BuildTaskResult result = pack_build_task(task);
                        std::unique_ptr<char[]> raw_buffer = std::move(task.raw);
                        pack_ns.fetch_add(static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - pack_start).count()),
                            std::memory_order_relaxed);
                        release_raw_buffer(std::move(raw_buffer));
                        {
                            auto result_wait_start = std::chrono::steady_clock::now();
                            std::unique_lock<std::mutex> lock(mutex);
                            if (read_first_io && !result.packed_block.empty()) {
                                const uint64_t result_bytes =
                                    static_cast<uint64_t>(result.packed_block.size());
                                result_space_cv.wait(lock, [&]() {
                                    return error ||
                                           completed_bytes + result_bytes <= write_buffer_limit_bytes ||
                                           completed.empty();
                                });
                            }
                            if (read_first_io) {
                                result_buffer_wait_ns.fetch_add(static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - result_wait_start).count()),
                                    std::memory_order_relaxed);
                            }
                            if (error) {
                                --active_tasks;
                                return;
                            }
                            --active_tasks;
                            completed_bytes +=
                                static_cast<uint64_t>(result.packed_block.size());
                            completed_peak_bytes =
                                std::max(completed_peak_bytes, completed_bytes);
                            completed.emplace(result.task_id, std::move(result));
                        }
                        result_cv.notify_all();
                        space_cv.notify_all();
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            --active_tasks;
                            if (!error) error = std::current_exception();
                            stop_workers = true;
                            producer_done = true;
                        }
                        task_cv.notify_all();
                        result_cv.notify_all();
                        space_cv.notify_all();
                        raw_buffer_cv.notify_all();
                        return;
                    }
                }
            });
        }

        std::thread writer([&]() {
            uint64_t next_task_to_write = 0;
            const uint64_t high_water_bytes =
                write_buffer_limit_bytes - write_buffer_limit_bytes / 8;
            const uint64_t low_water_bytes =
                write_buffer_limit_bytes - write_buffer_limit_bytes / 4;
            try {
                while (true) {
                    BuildTaskResult result;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        result_cv.wait(lock, [&]() {
                            if (error) return true;
                            if (producer_done && next_task_to_write == total_tasks) {
                                return true;
                            }
                            const bool next_ready =
                                completed.find(next_task_to_write) != completed.end();
                            if (!next_ready) return false;
                            if (!read_first_io) return true;
                            return producer_done ||
                                   completed_bytes >= high_water_bytes ||
                                   (producer_done && next_task_to_write == total_tasks);
                        });
                        if (error) return;
                        if (producer_done && next_task_to_write == total_tasks) return;

                        auto it = completed.find(next_task_to_write);
                        if (it == completed.end()) continue;
                        result = std::move(it->second);
                        completed_bytes -=
                            static_cast<uint64_t>(result.packed_block.size());
                        completed.erase(it);
                    }
                    space_cv.notify_all();
                    result_space_cv.notify_all();

                    if (!result.packed_block.empty()) {
                        if (!pages_out.append(result.packed_block.data(),
                                              result.packed_block.size())) {
                            throw std::runtime_error("failed to write packed task");
                        }
                    }
                    ++next_task_to_write;
                    if (read_first_io && !producer_done) {
                        std::unique_lock<std::mutex> lock(mutex);
                        if (completed_bytes <= low_water_bytes) {
                            continue;
                        }
                    }
                }
            } catch (...) {
                set_error(std::current_exception());
                task_cv.notify_all();
                result_cv.notify_all();
                space_cv.notify_all();
                raw_buffer_cv.notify_all();
                result_space_cv.notify_all();
            }
        });

        std::unique_ptr<char[]> current_task_raw = acquire_raw_buffer();
        if (!current_task_raw) return false;
        size_t current_task_size = 0;

        uint64_t sequence_count = 0;
        uint64_t total_length = 0;
        uint64_t current_sequence_length = 0;
        uint64_t next_task_id = 0;
        bool sequence_open = false;

        auto flush_task = [&](bool acquire_next) -> bool {
            if (current_task_size == 0) return true;
            BuildTask task;
            task.task_id = next_task_id++;
            task.page_residues = page_residues;
            task.encoding_mode = encoding_mode;
            task.raw = std::move(current_task_raw);
            task.raw_size = current_task_size;
            current_task_size = 0;
            if (stats) ++stats->tasks;
            if (!enqueue_task(std::move(task))) return false;
            if (acquire_next) {
                current_task_raw = acquire_raw_buffer();
                if (!current_task_raw) return false;
            }
            return true;
        };

        auto begin_sequence = [&]() -> bool {
            if (sequence_open) return false;
            sequence_open = true;
            current_sequence_length = 0;
            return true;
        };

        auto append_residues = [&](const char* data, size_t len) -> bool {
            if (!sequence_open || (data == nullptr && len != 0)) return false;
            if (len == 0) return true;

            if (len <= task_raw_capacity - current_task_size) {
                std::memcpy(current_task_raw.get() + current_task_size, data, len);
                current_task_size += len;
            } else {
                size_t copied = 0;
                while (copied < len) {
                    if (current_task_size == task_raw_capacity) {
                        if (!flush_task(true)) return false;
                    }
                    size_t space = task_raw_capacity - current_task_size;
                    size_t take = std::min(space, len - copied);
                    std::memcpy(current_task_raw.get() + current_task_size,
                                data + copied, take);
                    current_task_size += take;
                    copied += take;
                }
            }
            current_sequence_length += static_cast<uint64_t>(len);
            if (stats) stats->raw_bytes += static_cast<uint64_t>(len);
            return true;
        };

        auto end_sequence = [&]() -> bool {
            if (!sequence_open) return false;
            total_length += current_sequence_length;
            ++sequence_count;
            sequence_open = false;
            return meta_writer.append(total_length);
        };

        bool ok = true;
        auto producer_start = std::chrono::steady_clock::now();
        try {
            ok = produce_stream(begin_sequence, append_residues, end_sequence);
            if (sequence_open) ok = false;
            if (ok) ok = flush_task(false);
        } catch (...) {
            set_error(std::current_exception());
            ok = false;
            raw_buffer_cv.notify_all();
        }
        if (stats) {
            stats->producer_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - producer_start).count());
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            producer_done = true;
        }
        task_cv.notify_all();
        result_cv.notify_all();
        space_cv.notify_all();
        raw_buffer_cv.notify_all();
        result_space_cv.notify_all();

        auto drain_start = std::chrono::steady_clock::now();
        for (std::thread& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        if (writer.joinable()) writer.join();
        if (stats) {
            stats->drain_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - drain_start).count());
        }

        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (...) {
                return false;
            }
        }
        if (!ok) return false;

        const uint64_t page_count =
            total_length == 0 ? 0 : (total_length + page_residues - 1) / page_residues;
        if (stats) {
            stats->pages = page_count;
            if (page_count != 0) {
                stats->packed_bytes =
                    (page_count - 1) * packed_bytes_for_residues(page_residues) +
                    packed_bytes_for_residues(
                        total_length - (page_count - 1) * page_residues);
            }
        }

        auto finalize_start = std::chrono::steady_clock::now();
        if (!pages_out.close()) return false;
        if (stats) {
            stats->pack_ns = pack_ns.load(std::memory_order_relaxed);
            stats->write_ns = pages_out.write_ns();
            stats->page_write_buffer_bytes = pages_out.buffer_bytes();
            stats->page_writeback_step_bytes = pages_out.writeback_step_bytes();
            stats->page_output_direct = pages_out.direct_output() ? 1 : 0;
            stats->page_output_file_bytes = pages_out.file_bytes_written();
            stats->page_writeback_ns = pages_out.writeback_ns();
            stats->page_writeback_bytes = pages_out.writeback_bytes();
            stats->page_writeback_calls = pages_out.writeback_calls();
            stats->build_result_buffer_limit_bytes =
                read_first_io ? write_buffer_limit_bytes : 0;
            stats->build_result_buffer_peak_bytes =
                read_first_io ? completed_peak_bytes : 0;
            stats->build_result_buffer_wait_ns =
                read_first_io ? result_buffer_wait_ns.load(std::memory_order_relaxed) : 0;
            stats->page_close_ns = pages_out.close_ns();
        }

        bool meta_ok = meta_writer.finalize(sequence_count, total_length,
                                            page_residues, page_count);
        if (stats) {
            stats->index_write_ns = meta_writer.write_ns();
            stats->index_bytes = meta_writer.bytes_written();
            stats->index_flushes = meta_writer.flushes();
            stats->index_buffer_bytes = meta_writer.buffer_bytes();
            stats->finalize_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - finalize_start).count());
        }
        return meta_ok;
    }

    bool open(const std::string& store_dir) {
        if (!open(store_dir, 0)) return false;
        preload_cache_parallel(0, 0);
        return true;
    }

    bool open(const std::string& store_dir, const Options& options) {
        if (!open(store_dir, options.memory_bytes)) return false;
        configure(options);
        if (options.load_names) {
            std::string names_path = options.names_path.empty()
                ? (std::filesystem::path(store_dir) / "seq_names.txt").string()
                : options.names_path;
            if (!load_names(names_path)) return false;
        }
        if (options.auto_preload) {
            uint64_t preload_limit = options.memory_bytes == 0
                ? uint64_t{0}
                : options.memory_bytes;
            preload_cache_parallel(preload_limit, options.preload_threads);
            reset_stats();
        }
        return true;
    }

    bool open(const std::string& store_dir, uint64_t cache_capacity_bytes) {
        namespace fs = std::filesystem;
        const fs::path dir(store_dir);
        meta_path_ = (dir / "meta.bin").string();
        pages_path_ = (dir / "pages.bin").string();

        close_pages_fd();
        clear_names();
        if (!read_meta(meta_path_)) return false;
        pages_fd_ = ::open(pages_path_.c_str(), O_RDONLY);
        if (pages_fd_ < 0) return false;
        if (!validate_pages_file_size(pages_path_)) {
            close_pages_fd();
            return false;
        }

        cache_capacity_bytes_ = cache_capacity_bytes == 0
            ? full_cache_storage_bytes()
            : cache_capacity_bytes;
        clear_cache();
        stats_ = CacheStats{};
        return true;
    }

    void configure(const Options& options) {
        set_resident_cache_policy(options.cache_policy);
        set_prefetch_depth(options.prefetch_depth);
        set_prefetch_io_backend(options.io_backend);
        set_uring_sqpoll(options.uring_sqpoll);
        set_io_threads(options.io_threads);
        set_full_cache_decode_threads(options.decode_threads);
        set_prefetch_memory_bytes(options.prefetch_bytes);
        set_single_thread_aio(options.caller_runs_async_io);
        set_single_thread_aio_completion_batch(options.aio_completion_batch);
        set_aio_completion_min(options.aio_completion_min);
    }

    bool load_names() {
        if (pages_path_.empty()) return false;
        return load_names((std::filesystem::path(pages_path_).parent_path() /
                           "seq_names.txt").string());
    }

    bool load_names(const std::string& names_path) {
        names_blob_.clear();
        name_offsets32_.clear();
        name_offsets64_.clear();
        name_fingerprint_slots_.clear();
        name_id_slots_.clear();
        name_hash_mask_ = 0;

        std::ifstream in(names_path, std::ios::binary);
        if (!in) return false;
        uint64_t bytes = file_size_or_zero(names_path);
        if (bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("sequence names file is too large");
        }

        names_blob_.resize(static_cast<size_t>(bytes));
        if (bytes != 0) {
            in.read(names_blob_.data(), static_cast<std::streamsize>(names_blob_.size()));
            if (!in) return false;
        }

        const std::string index_path = default_name_index_path(names_path);
        if (load_name_index(index_path, bytes)) {
            names_path_ = names_path;
            return true;
        }

        std::vector<uint64_t> offsets;
        offsets.reserve(static_cast<size_t>(n_ + 1));
        offsets.push_back(0);
        for (size_t i = 0; i < names_blob_.size(); ++i) {
            if (names_blob_[i] == '\n') {
                offsets.push_back(static_cast<uint64_t>(i + 1));
            }
        }
        if (!names_blob_.empty() && names_blob_.back() != '\n') {
            offsets.push_back(static_cast<uint64_t>(names_blob_.size()));
        }
        if (offsets.size() != static_cast<size_t>(n_ + 1)) {
            clear_names();
            return false;
        }
        assign_name_offsets(std::move(offsets), bytes);

        if (n_ > std::numeric_limits<uint32_t>::max()) {
            clear_names();
            throw std::runtime_error("name index supports at most UINT32_MAX sequences");
        }

        const size_t slot_count = next_power_of_two(
            std::max<uint64_t>(2, (n_ * 4 + 2) / 3));
        name_fingerprint_slots_.assign(slot_count, 0);
        name_id_slots_.assign(slot_count, invalid_name_id());
        name_hash_mask_ = slot_count - 1;

        for (size_t i = 0; i < static_cast<size_t>(n_); ++i) {
            std::string_view name = name_view(i);
            uint64_t hash = hash_name(name);
            uint16_t fingerprint = name_fingerprint(hash);
            size_t slot = static_cast<size_t>(mix_hash(hash)) & name_hash_mask_;
            while (name_id_slots_[slot] != invalid_name_id()) {
                if (name_fingerprint_slots_[slot] == fingerprint &&
                    name_view(name_id_slots_[slot]) == name) {
                    clear_names();
                    throw std::runtime_error("duplicate sequence name: " +
                                             std::string(name));
                }
                slot = (slot + 1) & name_hash_mask_;
            }
            name_fingerprint_slots_[slot] = fingerprint;
            name_id_slots_[slot] = static_cast<uint32_t>(i);
        }
        names_path_ = names_path;
        return true;
    }

    static std::string default_name_index_path(const std::string& names_path) {
        namespace fs = std::filesystem;
        fs::path path(names_path);
        if (path.filename() == "seq_names.txt") {
            return (path.parent_path() / "seq_names.index.bin").string();
        }
        return names_path + ".index.bin";
    }

    static bool build_name_index(const std::string& names_path,
                                 const std::string& index_path,
                                 uint64_t expected_names = 0,
                                 NameIndexStats* stats = nullptr) {
        if (stats) *stats = NameIndexStats{};
        auto total_start = std::chrono::steady_clock::now();

        std::ifstream in(names_path, std::ios::binary);
        if (!in) return false;
        uint64_t bytes = file_size_or_zero(names_path);
        if (bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("sequence names file is too large");
        }

        auto read_start = std::chrono::steady_clock::now();
        std::string blob;
        blob.resize(static_cast<size_t>(bytes));
        if (bytes != 0) {
            in.read(blob.data(), static_cast<std::streamsize>(blob.size()));
            if (!in) return false;
        }
        if (stats) {
            stats->read_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - read_start).count());
            stats->names_bytes = bytes;
        }

        auto scan_start = std::chrono::steady_clock::now();
        std::vector<uint64_t> offsets;
        if (expected_names != 0) {
            offsets.reserve(static_cast<size_t>(expected_names + 1));
        }
        offsets.push_back(0);
        for (size_t i = 0; i < blob.size(); ++i) {
            if (blob[i] == '\n') offsets.push_back(static_cast<uint64_t>(i + 1));
        }
        if (!blob.empty() && blob.back() != '\n') {
            offsets.push_back(static_cast<uint64_t>(blob.size()));
        }
        const uint64_t name_count = offsets.empty()
            ? 0
            : static_cast<uint64_t>(offsets.size() - 1);
        if (expected_names != 0 && name_count != expected_names) return false;
        if (name_count > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("name index supports at most UINT32_MAX sequences");
        }
        if (stats) {
            stats->scan_offsets_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - scan_start).count());
            stats->names = name_count;
        }

        auto build_start = std::chrono::steady_clock::now();
        const size_t slot_count = next_power_of_two(
            std::max<uint64_t>(2, (name_count * 4 + 2) / 3));
        std::vector<uint16_t> fingerprint_slots(slot_count, 0);
        std::vector<uint32_t> id_slots(slot_count, invalid_name_id());
        const size_t mask = slot_count - 1;

        auto view_name = [&](size_t seq_id) {
            uint64_t begin = offsets[seq_id];
            uint64_t end = offsets[seq_id + 1];
            if (end > begin && blob[static_cast<size_t>(end - 1)] == '\n') --end;
            if (end > begin && blob[static_cast<size_t>(end - 1)] == '\r') --end;
            return std::string_view(blob.data() + static_cast<size_t>(begin),
                                    static_cast<size_t>(end - begin));
        };

        for (size_t i = 0; i < static_cast<size_t>(name_count); ++i) {
            std::string_view name = view_name(i);
            uint64_t hash = hash_name(name);
            uint16_t fingerprint = name_fingerprint(hash);
            size_t slot = static_cast<size_t>(mix_hash(hash)) & mask;
            while (id_slots[slot] != invalid_name_id()) {
                if (fingerprint_slots[slot] == fingerprint &&
                    view_name(id_slots[slot]) == name) {
                    throw std::runtime_error("duplicate sequence name: " +
                                             std::string(name));
                }
                slot = (slot + 1) & mask;
            }
            fingerprint_slots[slot] = fingerprint;
            id_slots[slot] = static_cast<uint32_t>(i);
        }
        if (stats) {
            stats->build_hash_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - build_start).count());
            stats->hash_slots = static_cast<uint64_t>(slot_count);
        }

        auto write_start = std::chrono::steady_clock::now();
        std::ofstream out(index_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const bool use_offsets32 =
            bytes <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
        const uint64_t offset_width = use_offsets32 ? 4 : 8;
        const uint64_t header[] = {
            kNameIndexMagic,
            kNameIndexVersion,
            name_count,
            bytes,
            static_cast<uint64_t>(slot_count),
            offset_width
        };
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        uint64_t offsets_bytes = 0;
        if (use_offsets32) {
            std::vector<uint32_t> offsets32;
            offsets32.reserve(offsets.size());
            for (uint64_t offset : offsets) {
                offsets32.push_back(static_cast<uint32_t>(offset));
            }
            offsets_bytes = static_cast<uint64_t>(offsets32.size() * sizeof(uint32_t));
            out.write(reinterpret_cast<const char*>(offsets32.data()),
                      static_cast<std::streamsize>(offsets_bytes));
        } else {
            offsets_bytes = static_cast<uint64_t>(offsets.size() * sizeof(uint64_t));
            out.write(reinterpret_cast<const char*>(offsets.data()),
                      static_cast<std::streamsize>(offsets_bytes));
        }
        out.write(reinterpret_cast<const char*>(fingerprint_slots.data()),
                  static_cast<std::streamsize>(
                      fingerprint_slots.size() * sizeof(uint16_t)));
        out.write(reinterpret_cast<const char*>(id_slots.data()),
                  static_cast<std::streamsize>(id_slots.size() * sizeof(uint32_t)));
        out.close();
        if (!out) return false;

        if (stats) {
            stats->write_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - write_start).count());
            stats->index_bytes = static_cast<uint64_t>(sizeof(header)) +
                offsets_bytes +
                static_cast<uint64_t>(fingerprint_slots.size() * sizeof(uint16_t)) +
                static_cast<uint64_t>(id_slots.size() * sizeof(uint32_t));
            (void)total_start;
        }
        return true;
    }

    void clear_names() {
        names_blob_.clear();
        name_offsets32_.clear();
        name_offsets64_.clear();
        name_fingerprint_slots_.clear();
        name_id_slots_.clear();
        name_hash_mask_ = 0;
        names_path_.clear();
    }

    bool names_loaded() const {
        return !name_offsets32_.empty() || !name_offsets64_.empty();
    }

    std::string_view name(size_t seq_id) const {
        if (!names_loaded()) {
            throw std::runtime_error("sequence names are not loaded");
        }
        if (seq_id >= n_) {
            throw std::out_of_range("sequence index out of range");
        }
        return name_view(seq_id);
    }

    size_t id_of(std::string_view name) const {
        if (!names_loaded()) {
            throw std::runtime_error("sequence names are not loaded");
        }
        size_t seq_id = 0;
        if (!find_name_id(name, seq_id)) throw std::out_of_range("sequence name not found");
        return seq_id;
    }

    bool try_id_of(std::string_view name, size_t& seq_id) const {
        if (!names_loaded()) return false;
        return find_name_id(name, seq_id);
    }

    std::string get_by_name(std::string_view name) {
        return get(id_of(name));
    }

    void get_by_name_into(std::string_view name, char* dst) {
        get_into(id_of(name), dst);
    }

    std::vector<size_t> ids_of(const std::vector<std::string_view>& names) const {
        return resolve_names_to_ids(names);
    }

    std::vector<std::string> batch_get_by_name(
            const std::vector<std::string_view>& names) {
        return batch_get(resolve_names_to_ids(names));
    }

    void batch_get_by_name_into(const std::vector<std::string_view>& names,
                                const std::vector<char*>& outputs) {
        batch_get_into(resolve_names_to_ids(names), outputs);
    }

    size_t size() const { return static_cast<size_t>(n_); }
    uint64_t total_length() const { return total_length_; }
    uint64_t sequence_length(size_t seq_id) const {
        uint64_t start = 0;
        uint64_t length = 0;
        sequence_bounds(seq_id, start, length);
        return length;
    }
    uint64_t page_residues() const { return page_residues_; }
    size_t page_count() const { return static_cast<size_t>(page_count_); }
    static const char* decode_kernel_name() {
        return aastore_detail::packed_5bit_decode_kernel_name(
            aastore_detail::packed_5bit_decode_kernel());
    }

    static const char* encode_kernel_name() {
        return aastore_detail::packed_5bit_encode_kernel_name(
            aastore_detail::packed_5bit_encode_kernel());
    }
    bool stop_aware_encoding() const { return stop_aware_encoding_; }
    EncodingMode encoding_mode() const {
        return stop_aware_encoding_ ? EncodingMode::StopAware : EncodingMode::Fast;
    }
    static const char* encoding_mode_name(EncodingMode mode) {
        return mode == EncodingMode::StopAware ? "stop_aware" : "fast";
    }
    uint64_t cache_bytes() const { return current_cache_bytes_; }
    CacheStats stats() const { return stats_; }

    static size_t automatic_preload_thread_count() {
        return std::min<size_t>(
            16, std::max<size_t>(1, std::thread::hardware_concurrency()));
    }

    static constexpr uint64_t default_prefetch_memory_cap_bytes() {
        return 16ULL * 1024ULL * 1024ULL;
    }

    static const char* prefetch_io_backend_name(uint64_t backend) {
        switch (static_cast<PrefetchIoBackend>(backend)) {
            case PrefetchIoBackend::Auto: return "auto";
            case PrefetchIoBackend::Pread: return "pread";
            case PrefetchIoBackend::LinuxAio: return "aio";
            case PrefetchIoBackend::IoUring: return "uring";
            case PrefetchIoBackend::IoUringFixed: return "uring_fixed";
            case PrefetchIoBackend::IoUringRegisteredFile: return "uring_file";
        }
        return "unknown";
    }

    static const char* resident_cache_policy_name(ResidentCachePolicy policy) {
        switch (policy) {
            case ResidentCachePolicy::Lru: return "lru_2q_lite";
            case ResidentCachePolicy::Prefix: return "prefix";
        }
        return "unknown";
    }

    static PrefetchIoBackend parse_prefetch_io_backend(const std::string& backend) {
        if (backend == "auto") return PrefetchIoBackend::Auto;
        if (backend == "pread") return PrefetchIoBackend::Pread;
        if (backend == "aio") return PrefetchIoBackend::LinuxAio;
        if (backend == "uring") return PrefetchIoBackend::IoUring;
        if (backend == "uring_fixed") return PrefetchIoBackend::IoUringFixed;
        if (backend == "uring_file") return PrefetchIoBackend::IoUringRegisteredFile;
        throw std::invalid_argument("unknown prefetch I/O backend: " + backend);
    }

    static ResidentCachePolicy parse_resident_cache_policy(const std::string& policy) {
        if (policy == "lru" || policy == "lru_2q_lite" || policy == "2q_lite") {
            return ResidentCachePolicy::Lru;
        }
        if (policy == "prefix" || policy == "non-lru" || policy == "non_lru") {
            return ResidentCachePolicy::Prefix;
        }
        throw std::invalid_argument("unknown resident cache policy: " + policy);
    }

    const char* resident_cache_policy_name() const {
        return resident_cache_policy_name(resident_cache_policy_);
    }

    void set_resident_cache_policy(ResidentCachePolicy policy) {
        resident_cache_policy_ = policy;
    }

    void set_resident_cache_policy(const std::string& policy) {
        resident_cache_policy_ = parse_resident_cache_policy(policy);
    }

    void reset_stats() { stats_ = CacheStats{}; }
    void set_prefetch_depth(size_t depth) {
        // 0 means auto: choose an I/O depth from the temporary buffer budget.
        prefetch_depth_ = depth;
    }
    void set_prefetch_io_backend(PrefetchIoBackend backend) {
        prefetch_io_backend_ = backend;
    }
    void set_prefetch_io_backend(const std::string& backend) {
        prefetch_io_backend_ = parse_prefetch_io_backend(backend);
    }
    void set_uring_sqpoll(bool enabled) {
        uring_sqpoll_ = enabled;
    }
    void set_io_threads(size_t threads) {
        default_batch_prefetch_threads_ = normalize_prefetch_threads(threads);
    }
    size_t io_threads() const {
        return default_batch_prefetch_threads_;
    }
    void set_full_cache_decode_threads(size_t threads) {
        full_cache_decode_threads_ = std::max<size_t>(1, threads);
    }
    size_t full_cache_decode_threads() const {
        return full_cache_decode_threads_;
    }
    void set_prefetch_memory_bytes(uint64_t prefetch_memory_bytes) {
        default_batch_prefetch_memory_bytes_ =
            cap_prefetch_memory_bytes(prefetch_memory_bytes);
    }
    uint64_t prefetch_memory_bytes() const {
        return default_batch_prefetch_memory_bytes_;
    }
    void set_batch_prefetch(size_t prefetch_threads,
                            uint64_t prefetch_memory_bytes) {
        set_io_threads(prefetch_threads);
        set_prefetch_memory_bytes(prefetch_memory_bytes);
    }
    void set_single_thread_aio(bool enabled) {
        single_thread_aio_ = enabled;
        if (enabled) {
            default_batch_prefetch_threads_ = 1;
        }
    }
    void set_single_thread_aio_completion_batch(size_t completion_batch) {
        single_thread_aio_completion_batch_ = std::max<size_t>(1, completion_batch);
    }
    void set_aio_completion_min(size_t completion_min) {
        aio_completion_min_ = std::max<size_t>(1, completion_min);
    }
    void drop_pages_file_cache() const {
#ifdef POSIX_FADV_DONTNEED
        if (pages_fd_ >= 0) {
            int rc = ::posix_fadvise(pages_fd_, 0, 0, POSIX_FADV_DONTNEED);
            if (rc != 0) {
                throw std::runtime_error(std::string("failed to drop pages.bin file cache: ") +
                                         std::strerror(rc));
            }
        }
#endif
    }

    MemoryBreakdown memory_breakdown() const {
        MemoryBreakdown mem;
        mem.object_bytes = sizeof(*this);
        mem.start_positions_bytes = static_cast<uint64_t>(start_positions_.size() * sizeof(uint64_t));
        mem.start_positions_capacity_bytes = static_cast<uint64_t>(start_positions_.capacity() * sizeof(uint64_t));
        mem.seq_meta_bytes = 0;
        mem.seq_meta_capacity_bytes = 0;
        mem.page_meta_bytes = 0;
        mem.page_cache_payload_bytes = current_cache_bytes_;
        mem.page_cache_capacity_bytes = cache_capacity_bytes_;
        mem.page_cache_direct_entries =
            full_cache_pages_.empty() ? prefix_cache_pages_ : page_count_;
        mem.page_cache_direct_vector_bytes = full_cache_pages_.empty()
            ? (prefix_cache_pages_ == 0 ? 0 : static_cast<uint64_t>(sizeof(prefix_cache_packed_)))
            : static_cast<uint64_t>(sizeof(full_cache_pages_));
        mem.page_cache_lru_entries = lru_live_entries_;
        mem.page_cache_entries = mem.page_cache_direct_entries + mem.page_cache_lru_entries;
        mem.page_cache_hash_buckets = static_cast<uint64_t>(page_to_lru_slot_.size());
        mem.page_cache_hash_bucket_bytes =
            static_cast<uint64_t>(page_to_lru_slot_.capacity() * sizeof(uint64_t));
        mem.page_cache_hash_table_est_bytes =
            mem.page_cache_hash_bucket_bytes +
            static_cast<uint64_t>(lru_slot_to_page_.capacity() * sizeof(uint64_t));
        mem.page_cache_lru_est_bytes =
            static_cast<uint64_t>(
                lru_ref_bits_.capacity() * sizeof(uint8_t) +
                free_lru_slots_.capacity() * sizeof(uint64_t));
        mem.page_cache_admission_bytes = static_cast<uint64_t>(
            lru_recent_miss_epoch_.capacity() * sizeof(uint16_t));
        mem.path_string_bytes =
            static_cast<uint64_t>(
                meta_path_.capacity() + pages_path_.capacity() +
                names_path_.capacity() + 3);
        mem.names_blob_bytes =
            static_cast<uint64_t>(names_blob_.capacity());
        mem.names_offsets_bytes =
            static_cast<uint64_t>(
                name_offsets32_.capacity() * sizeof(uint32_t) +
                name_offsets64_.capacity() * sizeof(uint64_t));
        mem.names_hash_slots =
            static_cast<uint64_t>(name_fingerprint_slots_.capacity());
        mem.names_hash_est_bytes =
            static_cast<uint64_t>(
                                  name_fingerprint_slots_.capacity() * sizeof(uint16_t) +
                                  name_id_slots_.capacity() * sizeof(uint32_t));
        mem.disk_meta_bytes = file_size_or_zero(meta_path_);
        mem.disk_pages_bytes = file_size_or_zero(pages_path_);
        mem.total_resident_est_bytes =
            mem.object_bytes +
            mem.start_positions_capacity_bytes +
            mem.names_blob_bytes +
            mem.names_offsets_bytes +
            mem.names_hash_est_bytes +
            mem.page_cache_direct_vector_bytes +
            mem.page_cache_payload_bytes +
            mem.page_cache_hash_table_est_bytes +
            mem.page_cache_lru_est_bytes +
            mem.page_cache_admission_bytes +
            mem.path_string_bytes;
        return mem;
    }

    uint64_t preload_cache(uint64_t max_bytes = 0) {
        return preload_cache_parallel(max_bytes, 1);
    }

    uint64_t preload_cache_parallel(uint64_t max_bytes = 0, size_t thread_count = 0) {
        if (thread_count == 0) {
            thread_count = automatic_preload_thread_count();
        }
        uint64_t limit = max_bytes == 0 ? cache_capacity_bytes_ : std::min(max_bytes, cache_capacity_bytes_);

        if (packed_page_bytes_ != 0 &&
            page_count_ > std::numeric_limits<uint64_t>::max() / packed_page_bytes_) {
            throw std::runtime_error("full page cache size overflow");
        }
        uint64_t full_cache_bytes = page_count_ * packed_page_bytes_;
        uint64_t full_file_bytes = expected_pages_file_bytes();
        if (page_count_ == 0 || (limit > 0 && full_cache_bytes <= limit)) {
            if (full_cache_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::runtime_error("full page cache is too large");
            }
            clear_cache();
            full_cache_pages_.assign(static_cast<size_t>(full_cache_bytes), 0);
            if (full_file_bytes > 0) {
                read_exact_at_fd(pages_fd_, full_cache_pages_.data(), full_file_bytes, 0,
                                 "failed to read pages.bin during full preload");
            }
            current_cache_bytes_ = full_cache_bytes;
            return page_count_;
        }

        clear_cache();
        if (resident_cache_policy_ == ResidentCachePolicy::Prefix) {
            uint64_t load_pages = std::min<uint64_t>(
                page_count_, limit / std::max<uint64_t>(packed_page_bytes_, 1));
            if (load_pages == 0) return 0;
            uint64_t bytes_to_read = load_pages * packed_page_bytes_;
            if (bytes_to_read > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                throw std::runtime_error("prefix page cache is too large");
            }
            prefix_cache_packed_ = allocate_aligned_buffer(
                static_cast<size_t>(bytes_to_read), 4096);
            read_exact_at_fd(pages_fd_, prefix_cache_packed_.get(), bytes_to_read, 0,
                             "failed to read pages.bin during prefix preload");
            prefix_cache_pages_ = load_pages;
            prefix_cache_stride_residues_ = page_residues_;
            prefix_cache_stride_bytes_ = packed_page_bytes_;
            current_cache_bytes_ = bytes_to_read;
            return load_pages;
        }

        return preload_lru_cache_sequential(limit);
    }

    std::string get(size_t seq_id) {
        uint64_t start = 0;
        uint64_t length = 0;
        sequence_bounds(seq_id, start, length);
        std::string out;
        out.resize(static_cast<size_t>(length));
        decode_sequence_into(out.data(), start, length);
        return out;
    }

    void get_into(size_t seq_id, char* dst) {
        uint64_t start = 0;
        uint64_t length = 0;
        sequence_bounds(seq_id, start, length);
        decode_sequence_into(dst, start, length);
    }

    std::vector<std::string> batch_get(const std::vector<size_t>& seq_ids) {
        return batch_get_impl(seq_ids,
                              default_batch_prefetch_threads_,
                              default_batch_prefetch_memory_bytes_);
    }

    std::vector<std::string> batch_get_prefetch(const std::vector<size_t>& seq_ids,
                                                size_t prefetch_threads,
                                                uint64_t prefetch_memory_bytes) {
        return batch_get_impl(seq_ids, normalize_prefetch_threads(prefetch_threads),
                              prefetch_memory_bytes);
    }

    void batch_get_into(const std::vector<size_t>& seq_ids,
                        const std::vector<char*>& outputs) {
        batch_get_into_impl(seq_ids, outputs,
                            default_batch_prefetch_threads_,
                            default_batch_prefetch_memory_bytes_);
    }

    void batch_get_into_prefetch(const std::vector<size_t>& seq_ids,
                                 const std::vector<char*>& outputs,
                                 size_t prefetch_threads,
                                 uint64_t prefetch_memory_bytes) {
        batch_get_into_impl(seq_ids, outputs, normalize_prefetch_threads(prefetch_threads),
                            prefetch_memory_bytes);
    }

    template <typename Visitor>
    void batch_visit(const std::vector<size_t>& seq_ids, Visitor&& visitor) {
        batch_visit_impl(seq_ids,
                         default_batch_prefetch_threads_,
                         default_batch_prefetch_memory_bytes_,
                         std::forward<Visitor>(visitor));
    }

    template <typename Visitor>
    void batch_visit_prefetch(const std::vector<size_t>& seq_ids,
                              size_t prefetch_threads,
                              uint64_t prefetch_memory_bytes,
                              Visitor&& visitor) {
        batch_visit_impl(seq_ids, normalize_prefetch_threads(prefetch_threads),
                         prefetch_memory_bytes, std::forward<Visitor>(visitor));
    }

private:
    struct LegacySeqMeta {
        uint64_t page_id = 0;
        uint64_t page_offset = 0;
        uint64_t length = 0;
    };

    struct BatchChunk {
        uint64_t page_id = 0;
        uint64_t page_offset = 0;
        uint64_t length = 0;
        uint64_t output_offset = 0;
        size_t original_pos = 0;
    };

    struct PageGroup {
        uint64_t page_id = 0;
        size_t begin = 0;
        size_t end = 0;
    };

    void decode_sequence_into(char* dst, uint64_t start, uint64_t length) {
        if (has_full_raw_cache()) {
            decode_full_cache_global_range(start, length, dst);
            return;
        }

        uint64_t page_id = start / page_residues_;
        uint64_t page_offset = start % page_residues_;
        uint64_t remaining = length;
        uint64_t output_offset = 0;

        while (remaining > 0) {
            PageView page = get_page(page_id);
            if (page_offset >= page.size) {
                throw std::runtime_error("corrupt sequence metadata");
            }
            uint64_t available = page.size - page_offset;
            uint64_t take = std::min(available, remaining);
            decode_page_range(page, page_offset, take,
                              dst + static_cast<size_t>(output_offset));
            remaining -= take;
            output_offset += take;
            ++page_id;
            page_offset = 0;
        }
    }

    struct PrefetchRange {
        size_t group_index = 0;
        uint64_t page_id = 0;
        uint64_t file_offset = 0;
        uint64_t bytes_to_read = 0;
        bool admit_to_lru = false;
    };

    struct PrefetchRangeSlot {
        PrefetchRange range;
        size_t buffer_offset = 0;
    };

    struct AlignedBufferDeleter {
        void operator()(uint8_t* ptr) const {
            std::free(ptr);
        }
    };
    using AlignedBuffer = std::unique_ptr<uint8_t, AlignedBufferDeleter>;

    static AlignedBuffer allocate_aligned_buffer(size_t bytes, size_t alignment) {
        void* ptr = nullptr;
        int rc = ::posix_memalign(&ptr, alignment, bytes);
        if (rc != 0) {
            throw std::runtime_error(std::string("failed to allocate aligned prefetch buffer: ") +
                                     std::strerror(rc));
        }
        return AlignedBuffer(static_cast<uint8_t*>(ptr));
    }

    struct ScopedFd {
        int fd = -1;
        ScopedFd() = default;
        explicit ScopedFd(int value) : fd(value) {}
        ScopedFd(const ScopedFd&) = delete;
        ScopedFd& operator=(const ScopedFd&) = delete;
        ~ScopedFd() {
            if (fd >= 0) ::close(fd);
        }
    };

#ifdef __linux__
    struct LinuxAioContext {
        aio_context_t ctx = 0;

        LinuxAioContext() = default;
        LinuxAioContext(const LinuxAioContext&) = delete;
        LinuxAioContext& operator=(const LinuxAioContext&) = delete;
        ~LinuxAioContext() {
            if (ctx != 0) {
                ::syscall(__NR_io_destroy, ctx);
            }
        }

        void init(size_t depth) {
            ctx = 0;
            long rc = ::syscall(__NR_io_setup, static_cast<unsigned>(depth), &ctx);
            if (rc < 0) {
                throw std::runtime_error(std::string("io_setup failed: ") +
                                         std::strerror(errno));
            }
        }
    };

    static long linux_io_submit(aio_context_t ctx, long nr, struct iocb** iocbs) {
        return ::syscall(__NR_io_submit, ctx, nr, iocbs);
    }

    static long linux_io_getevents(aio_context_t ctx,
                                   long min_nr,
                                   long max_nr,
                                   struct io_event* events,
                                   struct timespec* timeout = nullptr) {
        return ::syscall(__NR_io_getevents, ctx, min_nr, max_nr, events, timeout);
    }
#endif

    inline void decode_packed_range(const uint8_t* packed,
                                    uint64_t offset,
                                    uint64_t length,
                                    char* dst) const {
        aastore_detail::decode_packed_5bit_range(packed, offset, length, dst);
    }

    inline void decode_page_range(const PageView& page,
                                  uint64_t offset,
                                  uint64_t length,
                                  char* dst) const {
        if (length == 0) return;
        if (page.packed != nullptr) {
            decode_packed_range(page.packed, offset, length, dst);
            return;
        }
        if (page.codes == nullptr) {
            throw std::runtime_error("empty page view");
        }
        const uint8_t* packed =
            reinterpret_cast<const uint8_t*>(page.codes->data());
        decode_packed_range(packed, page.base + offset, length, dst);
    }

    enum class PrefetchSlotState {
        Empty,
        Reading,
        Ready
    };

    std::vector<std::string> batch_get_impl(const std::vector<size_t>& seq_ids,
                                            size_t prefetch_threads,
                                            uint64_t prefetch_memory_bytes) {
        std::vector<std::string> out(seq_ids.size());
        if (all_pages_preloaded()) {
            auto decode_start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < seq_ids.size(); ++i) {
                uint64_t start = 0;
                uint64_t length = 0;
                sequence_bounds(seq_ids[i], start, length);
                out[i].resize(static_cast<size_t>(length));
                decode_sequence_into(out[i].data(), start, length);
            }
            stats_.no_prefetch_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count());
            return out;
        }

        std::vector<char*> outputs(seq_ids.size(), nullptr);
        for (size_t i = 0; i < seq_ids.size(); ++i) {
            uint64_t start = 0;
            uint64_t length = 0;
            sequence_bounds(seq_ids[i], start, length);
            out[i].resize(static_cast<size_t>(length));
            outputs[i] = out[i].data();
        }
        batch_get_into_impl(seq_ids, outputs, prefetch_threads, prefetch_memory_bytes);
        return out;
    }

    void batch_get_into_impl(const std::vector<size_t>& seq_ids,
                             const std::vector<char*>& outputs,
                             size_t prefetch_threads,
                             uint64_t prefetch_memory_bytes) {
        if (outputs.size() != seq_ids.size()) {
            throw std::invalid_argument("outputs size must match seq_ids size");
        }

        if (all_pages_preloaded()) {
            auto decode_start = std::chrono::steady_clock::now();
            const size_t requested_threads =
                has_full_raw_cache()
                    ? std::min(full_cache_decode_threads_, seq_ids.size())
                    : size_t{1};
            if (requested_threads <= 1 || seq_ids.size() < 4096) {
                for (size_t i = 0; i < seq_ids.size(); ++i) {
                    uint64_t start = 0;
                    uint64_t length = 0;
                    sequence_bounds(seq_ids[i], start, length);
                    if (length != 0 && outputs[i] == nullptr) {
                        throw std::invalid_argument("output buffer is null");
                    }
                    decode_sequence_into(outputs[i], start, length);
                }
                stats_.full_cache_decode_threads = 1;
            } else {
                std::vector<std::thread> workers;
                std::vector<uint64_t> local_hits(requested_threads, 0);
                std::exception_ptr error = nullptr;
                std::mutex error_mutex;
                workers.reserve(requested_threads);

                const size_t chunk =
                    (seq_ids.size() + requested_threads - 1) / requested_threads;
                for (size_t t = 0; t < requested_threads; ++t) {
                    const size_t begin = t * chunk;
                    const size_t end = std::min(seq_ids.size(), begin + chunk);
                    if (begin >= end) break;
                    workers.emplace_back([&, t, begin, end]() {
                        try {
                            uint64_t hits = 0;
                            for (size_t i = begin; i < end; ++i) {
                                size_t seq_id = seq_ids[i];
                                if (seq_id >= n_ ||
                                    seq_id + 1 >= start_positions_.size()) {
                                    throw std::out_of_range("sequence index out of range");
                                }
                                uint64_t start = start_positions_[seq_id];
                                uint64_t length = start_positions_[seq_id + 1] - start;
                                if (length != 0 && outputs[i] == nullptr) {
                                    throw std::invalid_argument("output buffer is null");
                                }
                                hits += decode_full_cache_global_range_no_stats(
                                    start, length, outputs[i]);
                            }
                            local_hits[t] = hits;
                        } catch (...) {
                            std::lock_guard<std::mutex> lock(error_mutex);
                            if (!error) error = std::current_exception();
                        }
                    });
                }
                for (std::thread& worker : workers) worker.join();
                if (error) std::rethrow_exception(error);

                uint64_t hits = 0;
                for (uint64_t value : local_hits) hits += value;
                stats_.hits += hits;
                stats_.full_cache_decode_threads =
                    static_cast<uint64_t>(workers.size());
            }
            stats_.no_prefetch_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count());
            return;
        }

        struct SequenceRequest {
            size_t seq_id = 0;
            size_t original_pos = 0;
        };

        std::vector<SequenceRequest> ordered_requests;
        ordered_requests.reserve(seq_ids.size());
        for (size_t i = 0; i < seq_ids.size(); ++i) {
            if (seq_ids[i] >= n_) {
                throw std::out_of_range("sequence index out of range");
            }
            if (start_positions_[seq_ids[i] + 1] != start_positions_[seq_ids[i]] &&
                outputs[i] == nullptr) {
                throw std::invalid_argument("output buffer is null");
            }
            ordered_requests.push_back(SequenceRequest{seq_ids[i], i});
        }

        // Sequence IDs and packed page IDs are both monotonic in FASTA order.
        // Sorting requests by sequence ID therefore gives page-ordered chunks while
        // making the start_positions_ gathers substantially more cache-friendly.
        auto sort_start = std::chrono::steady_clock::now();
        if (ordered_requests.size() > 1) {
            if (n_ <= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                constexpr unsigned radix_bits = 11;
                constexpr size_t radix = 1ULL << radix_bits;
                constexpr uint32_t mask = static_cast<uint32_t>(radix - 1);
                std::vector<SequenceRequest> tmp(ordered_requests.size());
                std::array<size_t, radix> counts{};

                for (unsigned shift = 0; shift < 32; shift += radix_bits) {
                    counts.fill(0);
                    for (const SequenceRequest& request : ordered_requests) {
                        ++counts[(static_cast<uint32_t>(request.seq_id) >> shift) & mask];
                    }

                    size_t sum = 0;
                    for (size_t& count : counts) {
                        size_t current = count;
                        count = sum;
                        sum += current;
                    }

                    for (const SequenceRequest& request : ordered_requests) {
                        size_t bucket =
                            (static_cast<uint32_t>(request.seq_id) >> shift) & mask;
                        tmp[counts[bucket]++] = request;
                    }
                    ordered_requests.swap(tmp);
                }
            } else {
                std::sort(ordered_requests.begin(), ordered_requests.end(),
                          [](const SequenceRequest& a, const SequenceRequest& b) {
                              return a.seq_id < b.seq_id;
                          });
            }
        }
        stats_.batch_sort_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - sort_start).count());

        std::vector<BatchChunk> chunks;
        const size_t chunk_headroom = ordered_requests.size() / 8;
        chunks.reserve(ordered_requests.size() <=
                       std::numeric_limits<size_t>::max() - chunk_headroom
            ? ordered_requests.size() + chunk_headroom
            : ordered_requests.size());

        auto chunk_build_start = std::chrono::steady_clock::now();
        for (const SequenceRequest& request : ordered_requests) {
            uint64_t start = start_positions_[request.seq_id];
            uint64_t length = start_positions_[request.seq_id + 1] - start;

            uint64_t page_id = start / page_residues_;
            uint64_t page_offset = start % page_residues_;
            uint64_t remaining = length;
            uint64_t output_offset = 0;

            if (remaining == 0) continue;
            if (page_id >= page_count_) {
                throw std::runtime_error("corrupt sequence metadata");
            }
            uint64_t current_page_residues = residues_in_page(page_id);
            if (page_offset >= current_page_residues) {
                throw std::runtime_error("corrupt sequence metadata");
            }
            uint64_t available = current_page_residues - page_offset;

            // Most proteins fit in one 4KB page. Keep their request construction
            // to one bounds lookup and one chunk append.
            if (remaining <= available) {
                chunks.push_back(BatchChunk{
                    page_id,
                    page_offset,
                    remaining,
                    0,
                    request.original_pos
                });
                continue;
            }

            while (remaining > 0) {
                uint64_t take = std::min(available, remaining);
                chunks.push_back(BatchChunk{
                    page_id,
                    page_offset,
                    take,
                    output_offset,
                    request.original_pos
                });

                remaining -= take;
                output_offset += take;
                ++page_id;
                page_offset = 0;
                if (remaining == 0) break;
                if (page_id >= page_count_) {
                    throw std::runtime_error("corrupt sequence metadata");
                }
                current_page_residues = residues_in_page(page_id);
                available = current_page_residues;
            }
        }
        stats_.batch_chunk_build_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - chunk_build_start).count());

        auto group_start = std::chrono::steady_clock::now();
        std::vector<PageGroup> groups;
        groups.reserve(chunks.size());
        for (size_t i = 0; i < chunks.size();) {
            size_t j = i + 1;
            while (j < chunks.size() && chunks[j].page_id == chunks[i].page_id) ++j;
            groups.push_back(PageGroup{chunks[i].page_id, i, j});
            i = j;
        }
        stats_.batch_group_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - group_start).count());

        auto decode_group = [&](const PageGroup& group, const PageView& page) {
            for (size_t j = group.begin; j < group.end; ++j) {
                const BatchChunk& chunk = chunks[j];
                if (chunk.page_offset + chunk.length > page.size) {
                    throw std::runtime_error("corrupt sequence metadata");
                }

                decode_page_range(page, chunk.page_offset, chunk.length,
                                  outputs[chunk.original_pos] +
                                      static_cast<size_t>(chunk.output_offset));
            }
        };

        if (prefetch_memory_bytes == 0 || groups.size() <= 1) {
            auto decode_start = std::chrono::steady_clock::now();
            for (const PageGroup& group : groups) {
                decode_group(group, get_page(group.page_id));
            }
            stats_.no_prefetch_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count());
        } else {
            batch_decode_with_prefetch(groups, decode_group, prefetch_threads,
                                       prefetch_memory_bytes);
        }
    }

    template <typename Visitor>
    void batch_visit_impl(const std::vector<size_t>& seq_ids,
                          size_t prefetch_threads,
                          uint64_t prefetch_memory_bytes,
                          Visitor&& visitor) {
        std::vector<uint64_t> offsets(seq_ids.size() + 1, 0);
        for (size_t i = 0; i < seq_ids.size(); ++i) {
            uint64_t start = 0;
            uint64_t length = 0;
            sequence_bounds(seq_ids[i], start, length);
            offsets[i + 1] = offsets[i] + length;
        }

        std::vector<char> buffer(static_cast<size_t>(offsets.back()));
        std::vector<char*> outputs(seq_ids.size(), nullptr);
        for (size_t i = 0; i < seq_ids.size(); ++i) {
            outputs[i] = buffer.empty()
                ? nullptr
                : buffer.data() + static_cast<size_t>(offsets[i]);
        }

        batch_get_into_impl(seq_ids, outputs, prefetch_threads, prefetch_memory_bytes);

        for (size_t i = 0; i < seq_ids.size(); ++i) {
            const char* data = buffer.empty()
                ? nullptr
                : buffer.data() + static_cast<size_t>(offsets[i]);
            size_t length = static_cast<size_t>(offsets[i + 1] - offsets[i]);
            visitor(i, std::string_view(data, length));
        }
    }

    bool all_pages_preloaded() const {
        return page_count_ == 0 || has_full_raw_cache() ||
               prefix_cache_pages_ >= page_count_ ||
               lru_live_entries_ >= page_count_;
    }

    void sequence_bounds(size_t seq_id, uint64_t& start, uint64_t& length) const {
        if (seq_id >= n_ || seq_id + 1 >= start_positions_.size()) {
            throw std::out_of_range("sequence index out of range");
        }
        start = start_positions_[seq_id];
        uint64_t end = start_positions_[seq_id + 1];
        length = end - start;
    }

    bool has_full_raw_cache() const {
        return !full_cache_pages_.empty();
    }

    uint64_t full_cache_storage_bytes() const {
        if (page_count_ != 0 &&
            page_count_ > std::numeric_limits<uint64_t>::max() / packed_page_bytes_) {
            throw std::runtime_error("full page cache size overflow");
        }
        return page_count_ * packed_page_bytes_;
    }

    const uint8_t* full_cache_page_data(uint64_t page_id) const {
        return full_cache_pages_.data() + static_cast<size_t>(page_file_offset(page_id));
    }

    uint64_t decode_full_cache_global_range_no_stats(uint64_t offset,
                                                     uint64_t length,
                                                     char* dst) const {
        uint64_t done = 0;
        uint64_t touched_pages = 0;
        while (done < length) {
            uint64_t global = offset + done;
            uint64_t page_id = global / page_residues_;
            uint64_t page_offset = global % page_residues_;
            if (page_id >= page_count_) {
                throw std::runtime_error("corrupt sequence metadata");
            }
            uint64_t page_residues = residues_in_page(page_id);
            if (page_offset >= page_residues) {
                throw std::runtime_error("corrupt sequence metadata");
            }
            uint64_t take = std::min<uint64_t>(page_residues - page_offset, length - done);
            decode_packed_range(full_cache_page_data(page_id), page_offset, take,
                                dst + static_cast<size_t>(done));
            ++touched_pages;
            done += take;
        }
        return touched_pages;
    }

    void decode_full_cache_global_range(uint64_t offset, uint64_t length, char* dst) {
        stats_.hits += decode_full_cache_global_range_no_stats(offset, length, dst);
    }

    template <typename DecodeGroup>
    void batch_decode_with_prefetch(const std::vector<PageGroup>& groups,
                                    DecodeGroup&& decode_group,
                                    size_t prefetch_threads,
                                    uint64_t prefetch_memory_bytes) {
        size_t worker_count = prefetch_threads == 0
            ? (single_thread_aio_
                ? size_t{1}
                : static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency())))
            : prefetch_threads;
        worker_count = std::max<size_t>(1, worker_count);

        std::vector<size_t> miss_group_indices;
        miss_group_indices.reserve(groups.size());
        auto miss_scan_start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < groups.size(); ++i) {
            if (!find_cached_page(groups[i].page_id, false).valid()) {
                miss_group_indices.push_back(i);
            }
        }
        stats_.prefetch_miss_scan_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - miss_scan_start).count());
        stats_.prefetch_miss_pages += static_cast<uint64_t>(miss_group_indices.size());

        if (miss_group_indices.empty()) {
            auto decode_start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < groups.size(); ++i) {
                PageView cached = find_cached_page(groups[i].page_id, true);
                if (!cached.valid()) {
                    throw std::runtime_error("expected cached page");
                }
                ++stats_.hits;
                decode_group(groups[i], cached);
            }
            stats_.cache_hit_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count());
            return;
        }

        std::vector<PrefetchRange> ranges;
        ranges.reserve(miss_group_indices.size());
        const uint16_t lru_admission_epoch = next_lru_admission_epoch();
        bool batch_has_lru_admissions = false;
        bool batch_all_lru_admissions = !miss_group_indices.empty();

        uint64_t read_bytes = 0;
        auto range_build_start = std::chrono::steady_clock::now();
        for (size_t miss_index = 0; miss_index < miss_group_indices.size(); ++miss_index) {
            size_t group_index = miss_group_indices[miss_index];
            uint64_t page_id = groups[group_index].page_id;
            uint64_t page_bytes = packed_bytes_for_residues(residues_in_page(page_id));
            PrefetchRange range;
            range.group_index = group_index;
            range.page_id = page_id;
            range.file_offset = page_file_offset(page_id);
            range.bytes_to_read = page_bytes;
            range.admit_to_lru = lru_should_admit_page(
                page_id, lru_admission_epoch);
            batch_has_lru_admissions =
                batch_has_lru_admissions || range.admit_to_lru;
            batch_all_lru_admissions =
                batch_all_lru_admissions && range.admit_to_lru;
            read_bytes += page_bytes;
            ranges.push_back(range);
        }
        stats_.prefetch_range_build_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - range_build_start).count());

        stats_.prefetch_ranges += static_cast<uint64_t>(ranges.size());
        stats_.prefetch_read_bytes += read_bytes;

        auto setup_start = std::chrono::steady_clock::now();
        std::vector<PrefetchRangeSlot> slots(ranges.size());
        std::vector<std::atomic<uint8_t>> slot_states(ranges.size());
        for (std::atomic<uint8_t>& state : slot_states) {
            state.store(static_cast<uint8_t>(PrefetchSlotState::Empty),
                        std::memory_order_relaxed);
        }

        uint64_t memory_limit = std::max<uint64_t>(prefetch_memory_bytes, packed_page_bytes_);
        if (memory_limit > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("prefetch memory limit is too large");
        }
        size_t requested_depth = prefetch_depth_ == 0
            ? automatic_prefetch_depth(worker_count, prefetch_memory_bytes,
                                       packed_page_bytes_)
            : std::max<size_t>(1, prefetch_depth_);
        size_t active_range_limit = std::numeric_limits<size_t>::max();
        if (requested_depth != 0 &&
            worker_count <= std::numeric_limits<size_t>::max() / requested_depth) {
            active_range_limit = worker_count * requested_depth;
            if (active_range_limit <= std::numeric_limits<size_t>::max() / 2) {
                active_range_limit *= 2;
            }
        }
        active_range_limit = std::max<size_t>(1, active_range_limit);

        size_t max_blocks_by_memory = static_cast<size_t>(
            memory_limit / std::max<uint64_t>(packed_page_bytes_, 1));
        max_blocks_by_memory = std::max<size_t>(1, max_blocks_by_memory);
        const size_t fixed_block_bytes = static_cast<size_t>(packed_page_bytes_);
        size_t fixed_pool_blocks = std::min(max_blocks_by_memory, ranges.size());
        fixed_pool_blocks = std::max<size_t>(1, fixed_pool_blocks);
        if (fixed_block_bytes != 0 &&
            fixed_pool_blocks > std::numeric_limits<size_t>::max() / fixed_block_bytes) {
            throw std::runtime_error("prefetch fixed block pool is too large");
        }
        const size_t pool_size = fixed_pool_blocks * fixed_block_bytes;
        const bool direct_fixed_pool =
            fixed_pool_blocks >= ranges.size() && ranges.size() <= active_range_limit;
        std::vector<size_t> free_fixed_blocks;
        if (!direct_fixed_pool) {
            free_fixed_blocks.reserve(fixed_pool_blocks);
            for (size_t block = fixed_pool_blocks; block > 0; --block) {
                free_fixed_blocks.push_back(block - 1);
            }
        }

        AlignedBuffer prefetch_pool;

        bool aio_compatible = requested_depth > 1 &&
                              fixed_block_bytes % 4096 == 0 &&
                              pool_size % 4096 == 0;
        if (aio_compatible) {
            for (const PrefetchRange& range : ranges) {
                if (range.bytes_to_read > fixed_block_bytes ||
                    range.file_offset % 4096 != 0) {
                    aio_compatible = false;
                    break;
                }
            }
        }
        bool use_linux_aio = false;
        bool use_io_uring = false;
        bool use_io_uring_fixed = false;
        bool use_io_uring_registered_file = false;
        switch (prefetch_io_backend_) {
            case PrefetchIoBackend::Auto:
#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
                if (aio_compatible) {
                    use_io_uring = true;
                    use_io_uring_registered_file = true;
                }
#elif defined(__linux__)
                use_linux_aio = aio_compatible;
#endif
                break;
            case PrefetchIoBackend::Pread:
                break;
            case PrefetchIoBackend::LinuxAio:
#ifdef __linux__
                if (!aio_compatible) {
                    throw std::runtime_error(
                        "--io-backend aio requires --prefetch-depth > 1 and 4KB-aligned fixed-page O_DIRECT reads");
                }
                use_linux_aio = true;
#else
                throw std::runtime_error("--io-backend aio requires Linux");
#endif
                break;
            case PrefetchIoBackend::IoUring:
#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
                if (!aio_compatible) {
                    throw std::runtime_error(
                        "--io-backend uring requires --prefetch-depth > 1 and 4KB-aligned fixed-page O_DIRECT reads");
                }
                use_io_uring = true;
                use_io_uring_registered_file = true;
#elif defined(__linux__)
                throw std::runtime_error(
                    "--io-backend uring was requested, but this binary was built without liburing support");
#else
                throw std::runtime_error("--io-backend uring requires Linux");
#endif
                break;
            case PrefetchIoBackend::IoUringFixed:
#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
                if (!aio_compatible) {
                    throw std::runtime_error(
                        "--io-backend uring_fixed requires --prefetch-depth > 1 and 4KB-aligned fixed-page O_DIRECT reads");
                }
                use_io_uring = true;
                use_io_uring_fixed = true;
#elif defined(__linux__)
                throw std::runtime_error(
                    "--io-backend uring_fixed was requested, but this binary was built without liburing support");
#else
                throw std::runtime_error("--io-backend uring_fixed requires Linux");
#endif
                break;
            case PrefetchIoBackend::IoUringRegisteredFile:
#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
                if (!aio_compatible) {
                    throw std::runtime_error(
                        "--io-backend uring_file requires --prefetch-depth > 1 and 4KB-aligned fixed-page O_DIRECT reads");
                }
                use_io_uring = true;
                use_io_uring_registered_file = true;
#elif defined(__linux__)
                throw std::runtime_error(
                    "--io-backend uring_file was requested, but this binary was built without liburing support");
#else
                throw std::runtime_error("--io-backend uring_file requires Linux");
#endif
                break;
        }

#if defined(__linux__) && defined(PAGED_AASTORE_ENABLE_IO_URING)
        if (prefetch_io_backend_ == PrefetchIoBackend::Auto && use_io_uring) {
            struct io_uring probe_ring;
            struct io_uring_params probe_params;
            std::memset(&probe_ring, 0, sizeof(probe_ring));
            std::memset(&probe_params, 0, sizeof(probe_params));
            if (uring_sqpoll_) {
                probe_params.flags |= IORING_SETUP_SQPOLL;
                probe_params.sq_thread_idle = 2000;
            }
            int probe_rc = io_uring_queue_init_params(
                static_cast<unsigned>(requested_depth), &probe_ring, &probe_params);
            if (probe_rc == 0) {
                io_uring_queue_exit(&probe_ring);
            } else {
                use_io_uring = false;
                use_io_uring_fixed = false;
                use_io_uring_registered_file = false;
                use_linux_aio = aio_compatible;
            }
        }
#endif

        const bool use_async_io = use_linux_aio || use_io_uring;
        const size_t effective_depth = use_async_io ? requested_depth : 1;
        stats_.prefetch_depth = static_cast<uint64_t>(effective_depth);
        stats_.prefetch_io_backend = static_cast<uint64_t>(
            prefetch_io_backend_ == PrefetchIoBackend::IoUringRegisteredFile
                         ? PrefetchIoBackend::IoUringRegisteredFile
                         : (use_io_uring_fixed ? PrefetchIoBackend::IoUringFixed
                         : (use_io_uring ? PrefetchIoBackend::IoUring
                         : (use_linux_aio ? PrefetchIoBackend::LinuxAio
                                          : PrefetchIoBackend::Pread))));
        const size_t aio_completion_batch = use_async_io
            ? std::min<size_t>(effective_depth, single_thread_aio_completion_batch_)
            : 0;
        const size_t aio_completion_min = use_async_io
            ? std::min<size_t>(aio_completion_batch, aio_completion_min_)
            : 0;
        if (use_async_io) {
            stats_.prefetch_completion_batch = static_cast<uint64_t>(aio_completion_batch);
            stats_.prefetch_completion_min = static_cast<uint64_t>(aio_completion_min);
        }
        const size_t range_fetch_chunk = 64;
        stats_.prefetch_setup_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - setup_start).count());

        size_t next_range_to_read = 0;
        std::atomic<size_t> next_direct_range_to_read{0};
        uint64_t active_prefetch_bytes = 0;
        uint64_t active_prefetch_peak_bytes =
            direct_fixed_pool ? static_cast<uint64_t>(pool_size) : 0;
        uint64_t active_prefetch_ranges = 0;
        uint64_t active_prefetch_peak_ranges =
            direct_fixed_pool ? static_cast<uint64_t>(ranges.size()) : 0;
        std::atomic<bool> stop_workers{false};
        std::atomic<bool> has_worker_error{false};
        std::atomic<uint64_t> pool_wait_ns{0};
        std::atomic<uint64_t> submit_ns_total{0};
        std::atomic<uint64_t> submit_calls{0};
        std::atomic<uint64_t> submitted_requests{0};
        std::atomic<uint64_t> getevents_calls{0};
        std::atomic<uint64_t> completed_events{0};
        std::atomic<uint64_t> pread_ns_total{0};
        std::atomic<uint64_t> ready_push_ns{0};
        std::atomic<uint64_t> fixed_slot_decoded_ranges{0};
        std::atomic<uint64_t> fixed_slot_decode_ns{0};
        std::atomic<uint64_t> fixed_slot_active_buffers{0};
        std::atomic<uint64_t> fixed_slot_active_peak_buffers{0};
        std::exception_ptr worker_error = nullptr;
        std::mutex pool_mutex;
        std::condition_variable pool_cv;
        std::mutex ready_wait_mutex;
        std::condition_variable ready_cv;
        const size_t no_ready_range = ranges.size();
        std::vector<std::atomic<size_t>> ready_next(ranges.size());
        for (std::atomic<size_t>& next : ready_next) {
            next.store(no_ready_range, std::memory_order_relaxed);
        }
        std::atomic<size_t> ready_stack_head{no_ready_range};
        std::mutex error_mutex;

        auto wait_until_no_error = [&]() {
            if (has_worker_error.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (worker_error) std::rethrow_exception(worker_error);
                throw std::runtime_error("prefetch worker failed");
            }
        };

        auto note_fixed_slot_acquire = [&]() {
            uint64_t current =
                fixed_slot_active_buffers.fetch_add(1, std::memory_order_relaxed) + 1;
            uint64_t peak = fixed_slot_active_peak_buffers.load(std::memory_order_relaxed);
            while (current > peak &&
                   !fixed_slot_active_peak_buffers.compare_exchange_weak(
                       peak, current, std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        };

        auto note_fixed_slot_release = [&]() {
            fixed_slot_active_buffers.fetch_sub(1, std::memory_order_relaxed);
        };

        auto stop_and_join = [&](std::vector<std::thread>& workers) {
            stop_workers.store(true, std::memory_order_release);
            pool_cv.notify_all();
            ready_cv.notify_all();
            for (std::thread& worker : workers) {
                if (worker.joinable()) worker.join();
            }
        };

        auto record_worker_error = [&]() {
            {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (!worker_error) worker_error = std::current_exception();
            }
            has_worker_error.store(true, std::memory_order_release);
            pool_cv.notify_all();
            ready_cv.notify_all();
        };

        enum class ReserveResult {
            Reserved,
            NoBuffer,
            Done,
            Stopped
        };

        auto try_reserve_next_range = [&](size_t& range_index, PrefetchRange& range) -> ReserveResult {
            if (direct_fixed_pool) {
                if (stop_workers.load(std::memory_order_acquire) ||
                    has_worker_error.load(std::memory_order_acquire)) {
                    return ReserveResult::Stopped;
                }
                range_index = next_direct_range_to_read.fetch_add(
                    1, std::memory_order_relaxed);
                if (range_index >= ranges.size()) return ReserveResult::Done;
                range = ranges[range_index];
                slots[range_index].range = range;
                slots[range_index].buffer_offset =
                    range_index * fixed_block_bytes;
                slot_states[range_index].store(
                    static_cast<uint8_t>(PrefetchSlotState::Reading),
                    std::memory_order_release);
                return ReserveResult::Reserved;
            }

            std::unique_lock<std::mutex> lock(pool_mutex);
            if (stop_workers.load(std::memory_order_acquire) ||
                has_worker_error.load(std::memory_order_acquire)) {
                return ReserveResult::Stopped;
            }
            if (next_range_to_read >= ranges.size()) {
                return ReserveResult::Done;
            }
            if (free_fixed_blocks.empty()) {
                return ReserveResult::NoBuffer;
            }

            range_index = next_range_to_read++;
            range = ranges[range_index];
            size_t block_id = free_fixed_blocks.back();
            free_fixed_blocks.pop_back();
            size_t buffer_offset = block_id * fixed_block_bytes;
            active_prefetch_bytes += fixed_block_bytes;
            ++active_prefetch_ranges;
            active_prefetch_peak_bytes =
                std::max(active_prefetch_peak_bytes, active_prefetch_bytes);
            active_prefetch_peak_ranges =
                std::max(active_prefetch_peak_ranges, active_prefetch_ranges);
            slots[range_index].range = range;
            slots[range_index].buffer_offset = buffer_offset;
            slot_states[range_index].store(
                static_cast<uint8_t>(PrefetchSlotState::Reading),
                std::memory_order_release);
            return ReserveResult::Reserved;
        };

        auto reserve_direct_range_index = [&](size_t range_index, PrefetchRange& range) -> bool {
            if (range_index >= ranges.size()) return false;
            range = ranges[range_index];
            slots[range_index].range = range;
            slots[range_index].buffer_offset = range_index * fixed_block_bytes;
            slot_states[range_index].store(
                static_cast<uint8_t>(PrefetchSlotState::Reading),
                std::memory_order_release);
            return true;
        };

        auto wait_for_pool_space = [&]() {
            auto wait_start = std::chrono::steady_clock::now();
            std::unique_lock<std::mutex> lock(pool_mutex);
            pool_cv.wait(lock, [&]() {
                if (stop_workers.load(std::memory_order_acquire) ||
                    has_worker_error.load(std::memory_order_acquire) ||
                    next_range_to_read >= ranges.size()) {
                    return true;
                }
                return !free_fixed_blocks.empty();
            });
            pool_wait_ns.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count()),
                std::memory_order_relaxed);
        };

        auto reserve_next_range = [&](size_t& range_index, PrefetchRange& range) -> bool {
            for (;;) {
                ReserveResult result = try_reserve_next_range(range_index, range);
                if (result == ReserveResult::Reserved) return true;
                if (result == ReserveResult::Done ||
                    result == ReserveResult::Stopped) {
                    return false;
                }
                wait_for_pool_space();
            }
        };

        auto mark_range_ready = [&](size_t range_index) {
            auto push_start = std::chrono::steady_clock::now();
            slot_states[range_index].store(
                static_cast<uint8_t>(PrefetchSlotState::Ready),
                std::memory_order_release);
            size_t old_head = ready_stack_head.load(std::memory_order_acquire);
            do {
                ready_next[range_index].store(old_head, std::memory_order_relaxed);
            } while (!ready_stack_head.compare_exchange_weak(
                old_head, range_index,
                std::memory_order_release,
                std::memory_order_acquire));
            ready_cv.notify_one();
            ready_push_ns.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - push_start).count()),
                std::memory_order_relaxed);
        };

        auto mark_ranges_ready = [&](const std::vector<size_t>& range_indices) {
            if (range_indices.empty()) return;
            auto push_start = std::chrono::steady_clock::now();
            for (size_t range_index : range_indices) {
                slot_states[range_index].store(
                    static_cast<uint8_t>(PrefetchSlotState::Ready),
                    std::memory_order_release);
            }
            for (size_t i = 1; i < range_indices.size(); ++i) {
                ready_next[range_indices[i - 1]].store(
                    range_indices[i], std::memory_order_relaxed);
            }
            size_t first = range_indices.front();
            size_t last = range_indices.back();
            size_t old_head = ready_stack_head.load(std::memory_order_acquire);
            do {
                ready_next[last].store(old_head, std::memory_order_relaxed);
            } while (!ready_stack_head.compare_exchange_weak(
                old_head, first,
                std::memory_order_release,
                std::memory_order_acquire));
            ready_cv.notify_one();
            ready_push_ns.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - push_start).count()),
                std::memory_order_relaxed);
        };

        auto release_range = [&](size_t range_index) {
            auto release_start = std::chrono::steady_clock::now();
            if (direct_fixed_pool) {
                slot_states[range_index].store(
                    static_cast<uint8_t>(PrefetchSlotState::Empty),
                    std::memory_order_release);
                stats_.prefetch_release_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - release_start).count());
                return;
            }

            std::unique_lock<std::mutex> lock(pool_mutex);
            if (active_prefetch_bytes < fixed_block_bytes || active_prefetch_ranges == 0) {
                throw std::runtime_error("prefetch memory accounting underflow");
            }
            active_prefetch_bytes -= fixed_block_bytes;
            --active_prefetch_ranges;
            free_fixed_blocks.push_back(
                slots[range_index].buffer_offset / fixed_block_bytes);
            slot_states[range_index].store(
                static_cast<uint8_t>(PrefetchSlotState::Empty),
                std::memory_order_release);
            lock.unlock();
            pool_cv.notify_one();
            stats_.prefetch_release_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - release_start).count());
        };

        auto decode_cached_groups = [&]() {
            auto decode_start = std::chrono::steady_clock::now();
            for (size_t group_index = 0; group_index < groups.size(); ++group_index) {
                PageView cached = find_cached_page(groups[group_index].page_id, true);
                if (cached.valid()) {
                    ++stats_.hits;
                    decode_group(groups[group_index], cached);
                }
            }
            stats_.cache_hit_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count());
        };

        auto decode_prefetched_range = [&](size_t range_index) {
            if (slot_states[range_index].load(std::memory_order_acquire) !=
                static_cast<uint8_t>(PrefetchSlotState::Ready)) {
                throw std::runtime_error("prefetch ready queue mismatch");
            }

            const PrefetchRange& range = slots[range_index].range;
            auto decode_start = std::chrono::steady_clock::now();
            if (groups[range.group_index].page_id != range.page_id) {
                throw std::runtime_error("prefetch page mismatch");
            }
            uint64_t page_bytes = packed_bytes_for_residues(residues_in_page(range.page_id));
            if (page_bytes > range.bytes_to_read) {
                throw std::runtime_error("prefetch page outside range");
            }

            decode_group(groups[range.group_index], PageView{
                nullptr,
                0,
                residues_in_page(range.page_id),
                prefetch_pool.get() + slots[range_index].buffer_offset
            });
            if (range.admit_to_lru) {
                insert_packed_page_into_cache(
                    range.page_id,
                    prefetch_pool.get() + slots[range_index].buffer_offset);
            }
            ++stats_.misses;
            stats_.prefetch_miss_decode_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - decode_start).count());
        };

        auto decode_prefetched_buffer = [&](const PrefetchRange& range,
                                            const uint8_t* packed,
                                            bool promote_to_lru = true) {
            auto decode_start = std::chrono::steady_clock::now();
            if (groups[range.group_index].page_id != range.page_id) {
                throw std::runtime_error("prefetch page mismatch");
            }
            uint64_t page_bytes = packed_bytes_for_residues(residues_in_page(range.page_id));
            if (page_bytes > range.bytes_to_read) {
                throw std::runtime_error("prefetch page outside range");
            }

            decode_group(groups[range.group_index], PageView{
                nullptr,
                0,
                residues_in_page(range.page_id),
                packed
            });
            fixed_slot_decoded_ranges.fetch_add(1, std::memory_order_relaxed);
            fixed_slot_decode_ns.fetch_add(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - decode_start).count()),
                std::memory_order_relaxed);
            if (promote_to_lru && range.admit_to_lru) {
                insert_packed_page_into_cache(range.page_id, packed);
            }
        };

        auto decode_prefetched_cache_buffer = [&](const PrefetchRange& range,
                                                  CacheWriteBuffer&& buffer) {
            const uint8_t* packed_ptr = buffer.data;
            decode_prefetched_buffer(range, packed_ptr, false);
            if (range.admit_to_lru) {
                insert_reserved_cache_write_buffer(
                    range.page_id, std::move(buffer), residues_in_page(range.page_id));
            }
        };

        auto drain_ready_ranges = [&](bool wait_for_ready) {
            auto wait_start = std::chrono::steady_clock::now();
            size_t ready_head = ready_stack_head.exchange(
                no_ready_range, std::memory_order_acquire);
            if (wait_for_ready &&
                ready_head == no_ready_range &&
                !has_worker_error.load(std::memory_order_acquire)) {
                std::unique_lock<std::mutex> lock(ready_wait_mutex);
                ready_cv.wait(lock, [&]() {
                    return ready_stack_head.load(std::memory_order_acquire) != no_ready_range ||
                           has_worker_error.load(std::memory_order_acquire);
                });
                ready_head = ready_stack_head.exchange(
                    no_ready_range, std::memory_order_acquire);
            }
            wait_until_no_error();
            if (wait_for_ready) {
                stats_.prefetch_wait_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - wait_start).count());
            }

            size_t decoded = 0;
            while (ready_head != no_ready_range) {
                size_t range_index = ready_head;
                ready_head = ready_next[range_index].load(std::memory_order_relaxed);
                ready_next[range_index].store(no_ready_range, std::memory_order_relaxed);

                decode_prefetched_range(range_index);
                release_range(range_index);
                ++decoded;
            }
            return decoded;
        };

#ifdef __linux__
        ScopedFd async_pages_fd;
        if (use_async_io) {
            async_pages_fd.fd = ::open(pages_path_.c_str(), O_RDONLY | O_DIRECT);
            if (async_pages_fd.fd < 0) {
                throw std::runtime_error(std::string("failed to open pages.bin with O_DIRECT: ") +
                                         std::strerror(errno));
            }
        }

        auto getevents_wait_then_drain = [&](aio_context_t ctx,
                                             std::vector<struct io_event>& events,
                                             size_t outstanding) {
            if (outstanding == 0 || events.empty()) {
                return std::pair<long, uint64_t>{0, 0};
            }

            uint64_t total_wait_ns = 0;
            long total_completed = 0;
            long max_events = static_cast<long>(
                std::min<size_t>(outstanding, events.size()));
            long min_events = static_cast<long>(
                std::min<size_t>(static_cast<size_t>(max_events), aio_completion_min));

            auto wait_start = std::chrono::steady_clock::now();
            long completed = linux_io_getevents(
                ctx, min_events, max_events, events.data());
            total_wait_ns += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - wait_start).count());
            getevents_calls.fetch_add(1, std::memory_order_relaxed);
            if (completed < 0) {
                throw std::runtime_error(std::string("io_getevents failed: ") +
                                         std::strerror(errno));
            }
            total_completed += completed;

            while (total_completed < max_events) {
                struct timespec zero_timeout;
                zero_timeout.tv_sec = 0;
                zero_timeout.tv_nsec = 0;
                auto drain_start = std::chrono::steady_clock::now();
                long drained = linux_io_getevents(
                    ctx,
                    0,
                    max_events - total_completed,
                    events.data() + total_completed,
                    &zero_timeout);
                total_wait_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - drain_start).count());
                getevents_calls.fetch_add(1, std::memory_order_relaxed);
                if (drained < 0) {
                    throw std::runtime_error(std::string("io_getevents drain failed: ") +
                                             std::strerror(errno));
                }
                if (drained == 0) break;
                total_completed += drained;
            }

            completed_events.fetch_add(static_cast<uint64_t>(total_completed),
                                       std::memory_order_relaxed);
            return std::pair<long, uint64_t>{total_completed, total_wait_ns};
        };

        auto run_linux_aio_worker = [&](bool caller_worker,
                                        size_t* caller_decoded_ranges,
                                        size_t worker_ordinal,
                                        size_t fixed_slots_per_worker) {
            struct AioRequest {
                struct iocb cb;
                size_t range_index = 0;
                PrefetchRange range;
                size_t buffer_id = 0;
                size_t buffer_offset = 0;
            };

            struct ReadyDecode {
                PrefetchRange range;
                size_t buffer_id = 0;
                size_t buffer_offset = 0;
            };

            const bool fixed_slot_pipeline = fixed_slots_per_worker != 0;
            const size_t local_depth = fixed_slot_pipeline
                ? std::min(effective_depth, fixed_slots_per_worker)
                : effective_depth;
            const size_t worker_buffer_base =
                fixed_slot_pipeline ? worker_ordinal * fixed_slots_per_worker : 0;

            LinuxAioContext aio;
            aio.init(local_depth);

            std::vector<AioRequest> requests(local_depth);
            std::vector<size_t> free_request_ids;
            free_request_ids.reserve(local_depth);
            for (size_t i = local_depth; i > 0; --i) {
                free_request_ids.push_back(i - 1);
            }
            std::vector<size_t> free_buffer_ids;
            if (fixed_slot_pipeline) {
                free_buffer_ids.reserve(fixed_slots_per_worker);
                for (size_t i = fixed_slots_per_worker; i > 0; --i) {
                    free_buffer_ids.push_back(i - 1);
                }
            }
            std::vector<struct io_event> events(aio_completion_batch);
            std::vector<struct iocb*> submit_cbs;
            submit_cbs.reserve(local_depth);
            std::vector<size_t> ready_batch;
            ready_batch.reserve(local_depth);
            std::vector<ReadyDecode> decode_batch;
            decode_batch.reserve(local_depth);

            size_t outstanding = 0;
            bool no_more_ranges = false;
            size_t local_range_next = 0;
            size_t local_range_end = 0;

            auto reserve_worker_next_range = [&](size_t& range_index,
                                                 PrefetchRange& range) -> ReserveResult {
                if (!fixed_slot_pipeline && !direct_fixed_pool) {
                    return try_reserve_next_range(range_index, range);
                }
                if (stop_workers.load(std::memory_order_acquire) ||
                    has_worker_error.load(std::memory_order_acquire)) {
                    return ReserveResult::Stopped;
                }
                while (local_range_next >= local_range_end) {
                    size_t begin = next_direct_range_to_read.fetch_add(
                        range_fetch_chunk, std::memory_order_relaxed);
                    if (begin >= ranges.size()) return ReserveResult::Done;
                    local_range_next = begin;
                    local_range_end = std::min(begin + range_fetch_chunk, ranges.size());
                }
                range_index = local_range_next++;
                if (fixed_slot_pipeline) {
                    if (range_index >= ranges.size()) return ReserveResult::Done;
                    range = ranges[range_index];
                    return ReserveResult::Reserved;
                }
                if (!reserve_direct_range_index(range_index, range)) {
                    return ReserveResult::Done;
                }
                return ReserveResult::Reserved;
            };

            auto prepare_one = [&](size_t request_id,
                                   size_t buffer_id,
                                   size_t range_index,
                                   const PrefetchRange& range) {
                AioRequest& request = requests[request_id];
                std::memset(&request.cb, 0, sizeof(request.cb));
                request.range_index = range_index;
                request.range = range;
                request.buffer_id = buffer_id;
                const size_t buffer_block = fixed_slot_pipeline
                    ? worker_buffer_base + buffer_id
                    : slots[range_index].buffer_offset / fixed_block_bytes;
                const size_t buffer_offset = buffer_block * fixed_block_bytes;
                request.buffer_offset = buffer_offset;
                if (fixed_slot_pipeline) {
                    slots[range_index].range = range;
                    slots[range_index].buffer_offset = buffer_offset;
                }
                request.cb.aio_data = static_cast<uint64_t>(request_id);
                request.cb.aio_lio_opcode = IOCB_CMD_PREAD;
                request.cb.aio_fildes = static_cast<uint32_t>(async_pages_fd.fd);
                request.cb.aio_buf = reinterpret_cast<uint64_t>(
                    prefetch_pool.get() + buffer_offset);
                request.cb.aio_nbytes = fixed_block_bytes;
                request.cb.aio_offset = static_cast<int64_t>(range.file_offset);
                submit_cbs.push_back(&request.cb);
            };

            auto submit_prepared = [&]() {
                size_t submitted_total = 0;
                while (submitted_total < submit_cbs.size()) {
                    auto submit_start = std::chrono::steady_clock::now();
                    long submitted = linux_io_submit(
                        aio.ctx,
                        static_cast<long>(submit_cbs.size() - submitted_total),
                        submit_cbs.data() + submitted_total);
                    submit_ns_total.fetch_add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - submit_start).count()),
                        std::memory_order_relaxed);
                    if (submitted < 0) {
                        throw std::runtime_error(std::string("io_submit failed: ") +
                                                 std::strerror(errno));
                    }
                    if (submitted == 0) {
                        throw std::runtime_error("io_submit submitted no requests");
                    }
                    submit_calls.fetch_add(1, std::memory_order_relaxed);
                    submitted_requests.fetch_add(static_cast<uint64_t>(submitted),
                                                 std::memory_order_relaxed);
                    submitted_total += static_cast<size_t>(submitted);
                    outstanding += static_cast<size_t>(submitted);
                }
                submit_cbs.clear();
            };

            auto submit_until_full = [&]() {
                submit_cbs.clear();
                while (outstanding + submit_cbs.size() < local_depth && !no_more_ranges) {
                    if (fixed_slot_pipeline && free_buffer_ids.empty()) {
                        break;
                    }
                    size_t range_index = 0;
                    PrefetchRange range;
                    ReserveResult reserve_result = reserve_worker_next_range(range_index, range);
                    if (reserve_result == ReserveResult::Reserved) {
                        size_t request_id = free_request_ids.back();
                        free_request_ids.pop_back();
                        size_t buffer_id = 0;
                        if (fixed_slot_pipeline) {
                            buffer_id = free_buffer_ids.back();
                            free_buffer_ids.pop_back();
                            note_fixed_slot_acquire();
                        }
                        prepare_one(request_id, buffer_id, range_index, range);
                    } else if (reserve_result == ReserveResult::Done ||
                               reserve_result == ReserveResult::Stopped) {
                        no_more_ranges = true;
                    } else {
                        break;
                    }
                }
                if (!submit_cbs.empty()) {
                    submit_prepared();
                }
            };

            if (caller_worker) {
                submit_until_full();
                decode_cached_groups();
            }

            while (!stop_workers.load(std::memory_order_acquire)) {
                submit_until_full();
                if (caller_worker && !fixed_slot_pipeline) {
                    *caller_decoded_ranges += drain_ready_ranges(false);
                }

                if (outstanding == 0) {
                    if (no_more_ranges) break;
                    if (caller_worker && !fixed_slot_pipeline) {
                        *caller_decoded_ranges += drain_ready_ranges(true);
                    } else if (!fixed_slot_pipeline) {
                        wait_for_pool_space();
                    } else {
                        std::this_thread::yield();
                    }
                    continue;
                }

                auto pread_start = std::chrono::steady_clock::now();
                auto completed_result = getevents_wait_then_drain(
                    aio.ctx, events, outstanding);
                long completed = completed_result.first;
                pread_ns_total.fetch_add(static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - pread_start).count()),
                    std::memory_order_relaxed);

                ready_batch.clear();
                decode_batch.clear();
                for (long i = 0; i < completed; ++i) {
                    size_t request_id = static_cast<size_t>(events[static_cast<size_t>(i)].data);
                    if (request_id >= requests.size()) {
                        throw std::runtime_error("io_getevents returned invalid request id");
                    }
                    size_t range_index = requests[request_id].range_index;
                    const PrefetchRange& range = fixed_slot_pipeline
                        ? requests[request_id].range
                        : slots[range_index].range;
                    if (events[static_cast<size_t>(i)].res < 0 ||
                        static_cast<uint64_t>(events[static_cast<size_t>(i)].res) <
                            range.bytes_to_read) {
                        throw std::runtime_error("failed to read prefetch range with Linux AIO");
                    }
                    if (fixed_slot_pipeline) {
                        decode_batch.push_back(ReadyDecode{
                            range,
                            requests[request_id].buffer_id,
                            requests[request_id].buffer_offset
                        });
                    } else {
                        ready_batch.push_back(range_index);
                    }
                    free_request_ids.push_back(request_id);
                    --outstanding;
                }
                submit_until_full();
                if (fixed_slot_pipeline) {
                    if (caller_worker) {
                        stats_.prefetch_wait_ns += completed_result.second;
                    }
                    for (const ReadyDecode& ready : decode_batch) {
                        decode_prefetched_buffer(
                            ready.range,
                            prefetch_pool.get() + ready.buffer_offset);
                        free_buffer_ids.push_back(ready.buffer_id);
                        note_fixed_slot_release();
                    }
                    submit_until_full();
                } else if (caller_worker) {
                    stats_.prefetch_wait_ns += completed_result.second;
                    for (size_t range_index : ready_batch) {
                        slot_states[range_index].store(
                            static_cast<uint8_t>(PrefetchSlotState::Ready),
                            std::memory_order_release);
                        decode_prefetched_range(range_index);
                        release_range(range_index);
                        ++*caller_decoded_ranges;
                    }
                    *caller_decoded_ranges += drain_ready_ranges(false);
                } else {
                    mark_ranges_ready(ready_batch);
                }
            }
        };

#if defined(PAGED_AASTORE_ENABLE_IO_URING)
        auto run_io_uring_worker = [&](bool caller_worker,
                                       size_t* caller_decoded_ranges,
                                       size_t worker_ordinal,
                                       size_t fixed_slots_per_worker) {
            struct UringRequest {
                size_t range_index = 0;
                PrefetchRange range;
                size_t buffer_id = 0;
                size_t buffer_offset = 0;
            };

            struct ReadyDecode {
                PrefetchRange range;
                size_t buffer_id = 0;
                size_t buffer_offset = 0;
            };

            const bool fixed_slot_pipeline = fixed_slots_per_worker != 0;
            // A fully admitted batch needs no temporary pool: io_uring reads
            // straight into the final aligned resident cache slots.
            const bool cache_slot_pipeline =
                fixed_slot_pipeline && batch_all_lru_admissions &&
                !use_io_uring_fixed;
            const bool direct_cache_fill_enabled =
                fixed_slot_pipeline && !use_io_uring_fixed;
            const bool defer_cache_decode =
                fixed_slot_pipeline && batch_has_lru_admissions;
            const size_t local_depth = fixed_slot_pipeline
                ? std::min(effective_depth, fixed_slots_per_worker)
                : effective_depth;
            const size_t local_completion_batch =
                std::max<size_t>(1, std::min(aio_completion_batch, local_depth));
            const size_t local_completion_min =
                std::min(aio_completion_min, local_completion_batch);
            const size_t worker_buffer_base =
                fixed_slot_pipeline ? worker_ordinal * fixed_slots_per_worker : 0;

            struct RingGuard {
                struct io_uring ring;
                bool initialized = false;
                ~RingGuard() {
                    if (initialized) io_uring_queue_exit(&ring);
                }
            } guard;

            struct io_uring_params params;
            std::memset(&params, 0, sizeof(params));
            if (uring_sqpoll_) {
                params.flags |= IORING_SETUP_SQPOLL;
                params.sq_thread_idle = 2000;
            }
            int rc = io_uring_queue_init_params(static_cast<unsigned>(local_depth),
                                                &guard.ring, &params);
            if (rc < 0) {
                throw std::runtime_error(std::string("io_uring_queue_init failed: ") +
                                         std::strerror(-rc));
            }
            guard.initialized = true;
            if (uring_sqpoll_) {
                stats_.uring_sqpoll = 1;
            }

            int registered_files[1] = {async_pages_fd.fd};
            if (use_io_uring_registered_file) {
                rc = io_uring_register_files(&guard.ring, registered_files, 1);
                if (rc < 0) {
                    throw std::runtime_error(std::string("io_uring_register_files failed: ") +
                                             std::strerror(-rc));
                }
            }

            std::vector<struct iovec> fixed_iovecs;
            if (use_io_uring_fixed) {
                fixed_iovecs.resize(1);
                fixed_iovecs[0].iov_base = prefetch_pool.get();
                fixed_iovecs[0].iov_len = pool_size;
                rc = io_uring_register_buffers(
                    &guard.ring,
                    fixed_iovecs.data(),
                    static_cast<unsigned>(fixed_iovecs.size()));
                if (rc < 0) {
                    throw std::runtime_error(
                        std::string("io_uring_register_buffers failed: ") +
                        std::strerror(-rc) +
                        " (try raising RLIMIT_MEMLOCK/ulimit -l or use --io-backend uring)");
                }
            }

            std::vector<UringRequest> requests(local_depth);
            std::vector<CacheWriteBuffer> request_cache_buffers(local_depth);
            std::vector<size_t> free_request_ids;
            free_request_ids.reserve(local_depth);
            for (size_t i = local_depth; i > 0; --i) {
                free_request_ids.push_back(i - 1);
            }
            std::vector<size_t> free_buffer_ids;
            if (fixed_slot_pipeline) {
                free_buffer_ids.reserve(fixed_slots_per_worker);
                for (size_t i = fixed_slots_per_worker; i > 0; --i) {
                    free_buffer_ids.push_back(i - 1);
                }
            }
            std::vector<size_t> ready_batch;
            ready_batch.reserve(local_completion_batch);
            std::vector<ReadyDecode> decode_batch;
            decode_batch.reserve(local_completion_batch);
            std::vector<PrefetchRange> cache_decode_ranges;
            cache_decode_ranges.reserve(local_completion_batch);
            std::vector<CacheWriteBuffer> cache_decode_buffers;
            cache_decode_buffers.reserve(local_completion_batch);

            size_t outstanding = 0;
            bool no_more_ranges = false;
            size_t local_range_next = 0;
            size_t local_range_end = 0;

            auto reserve_worker_next_range = [&](size_t& range_index,
                                                 PrefetchRange& range) -> ReserveResult {
                if (!fixed_slot_pipeline && !direct_fixed_pool) {
                    return try_reserve_next_range(range_index, range);
                }
                if (stop_workers.load(std::memory_order_acquire) ||
                    has_worker_error.load(std::memory_order_acquire)) {
                    return ReserveResult::Stopped;
                }
                while (local_range_next >= local_range_end) {
                    size_t begin = next_direct_range_to_read.fetch_add(
                        range_fetch_chunk, std::memory_order_relaxed);
                    if (begin >= ranges.size()) return ReserveResult::Done;
                    local_range_next = begin;
                    local_range_end = std::min(begin + range_fetch_chunk, ranges.size());
                }
                range_index = local_range_next++;
                if (fixed_slot_pipeline) {
                    if (range_index >= ranges.size()) return ReserveResult::Done;
                    range = ranges[range_index];
                    return ReserveResult::Reserved;
                }
                if (!reserve_direct_range_index(range_index, range)) {
                    return ReserveResult::Done;
                }
                return ReserveResult::Reserved;
            };

            auto prepare_one = [&](size_t request_id,
                                   size_t buffer_id,
                                   size_t range_index,
                                   const PrefetchRange& range) {
                struct io_uring_sqe* sqe = io_uring_get_sqe(&guard.ring);
                if (sqe == nullptr) {
                    throw std::runtime_error("io_uring_get_sqe returned no SQE");
                }
                requests[request_id].range_index = range_index;
                requests[request_id].range = range;
                requests[request_id].buffer_id = buffer_id;
                const size_t buffer_block = fixed_slot_pipeline
                    ? worker_buffer_base + buffer_id
                    : slots[range_index].buffer_offset / fixed_block_bytes;
                const size_t buffer_offset = buffer_block * fixed_block_bytes;
                requests[request_id].buffer_offset = buffer_offset;
                if (fixed_slot_pipeline) {
                    slots[range_index].range = range;
                    slots[range_index].buffer_offset = buffer_offset;
                }
                uint8_t* dst = nullptr;
                if (cache_slot_pipeline ||
                    (direct_cache_fill_enabled && range.admit_to_lru)) {
                    request_cache_buffers[request_id] =
                        reserve_cache_write_buffer(range.page_id);
                    dst = request_cache_buffers[request_id].data;
                } else {
                    dst = prefetch_pool.get() + buffer_offset;
                }
                if (use_io_uring_fixed) {
                    io_uring_prep_read_fixed(
                        sqe,
                        use_io_uring_registered_file ? 0 : async_pages_fd.fd,
                        dst,
                        static_cast<unsigned>(fixed_block_bytes),
                        static_cast<off_t>(range.file_offset),
                        0);
                } else {
                    io_uring_prep_read(
                        sqe,
                        use_io_uring_registered_file ? 0 : async_pages_fd.fd,
                        dst,
                        static_cast<unsigned>(fixed_block_bytes),
                        static_cast<off_t>(range.file_offset));
                }
                if (use_io_uring_registered_file) {
                    sqe->flags |= IOSQE_FIXED_FILE;
                }
                sqe->user_data = static_cast<unsigned long long>(request_id);
            };

            auto submit_prepared = [&](size_t prepared) {
                if (prepared == 0) return;
                if (uring_sqpoll_) {
                    auto submit_start = std::chrono::steady_clock::now();
                    int submitted = io_uring_submit(&guard.ring);
                    submit_ns_total.fetch_add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - submit_start).count()),
                        std::memory_order_relaxed);
                    submit_calls.fetch_add(1, std::memory_order_relaxed);
                    if (submitted < 0) {
                        throw std::runtime_error(std::string("io_uring_submit failed: ") +
                                                 std::strerror(-submitted));
                    }
                    submitted_requests.fetch_add(static_cast<uint64_t>(prepared),
                                                 std::memory_order_relaxed);
                    outstanding += prepared;
                    return;
                }

                size_t submitted_total = 0;
                size_t zero_submit_spins = 0;
                while (submitted_total < prepared) {
                    auto submit_start = std::chrono::steady_clock::now();
                    int submitted = io_uring_submit(&guard.ring);
                    submit_ns_total.fetch_add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - submit_start).count()),
                        std::memory_order_relaxed);
                    submit_calls.fetch_add(1, std::memory_order_relaxed);
                    if (submitted < 0) {
                        if (submitted == -EAGAIN || submitted == -EBUSY) {
                            std::this_thread::yield();
                            continue;
                        }
                        throw std::runtime_error(std::string("io_uring_submit failed: ") +
                                                 std::strerror(-submitted));
                    }
                    if (submitted == 0) {
                        if (++zero_submit_spins > 1024) {
                            throw std::runtime_error("io_uring_submit made no progress");
                        }
                        std::this_thread::yield();
                        continue;
                    }
                    zero_submit_spins = 0;
                    submitted_total += static_cast<size_t>(submitted);
                    submitted_requests.fetch_add(static_cast<uint64_t>(submitted),
                                                 std::memory_order_relaxed);
                    outstanding += static_cast<size_t>(submitted);
                }
            };

            auto submit_until_full = [&]() {
                size_t prepared = 0;
                while (outstanding + prepared < local_depth && !no_more_ranges) {
                    if (fixed_slot_pipeline && free_buffer_ids.empty()) {
                        break;
                    }
                    size_t range_index = 0;
                    PrefetchRange range;
                    ReserveResult reserve_result = reserve_worker_next_range(range_index, range);
                    if (reserve_result == ReserveResult::Reserved) {
                        if (free_request_ids.empty()) {
                            throw std::runtime_error("io_uring request id pool exhausted");
                        }
                        size_t request_id = free_request_ids.back();
                        free_request_ids.pop_back();
                        size_t buffer_id = 0;
                        if (fixed_slot_pipeline) {
                            buffer_id = free_buffer_ids.back();
                            free_buffer_ids.pop_back();
                            note_fixed_slot_acquire();
                        }
                        prepare_one(request_id, buffer_id, range_index, range);
                        ++prepared;
                    } else if (reserve_result == ReserveResult::Done ||
                               reserve_result == ReserveResult::Stopped) {
                        no_more_ranges = true;
                    } else {
                        break;
                    }
                }
                submit_prepared(prepared);
            };

            auto collect_completions = [&](size_t max_events) {
                ready_batch.clear();
                decode_batch.clear();
                cache_decode_ranges.clear();
                cache_decode_buffers.clear();
                uint64_t wait_ns = 0;
                size_t completed = 0;
                size_t min_events = std::min<size_t>(max_events, local_completion_min);

                auto process_cqe = [&](struct io_uring_cqe* cqe) {
                    size_t request_id = static_cast<size_t>(cqe->user_data);
                    if (request_id >= requests.size()) {
                        int res = cqe->res;
                        io_uring_cqe_seen(&guard.ring, cqe);
                        throw std::runtime_error("io_uring returned invalid request id: " +
                                                 std::to_string(request_id) +
                                                 " res=" + std::to_string(res));
                    }
                    size_t range_index = requests[request_id].range_index;
                    const PrefetchRange& range = fixed_slot_pipeline
                        ? requests[request_id].range
                        : slots[range_index].range;
                    int res = cqe->res;
                    io_uring_cqe_seen(&guard.ring, cqe);
                    if (res < 0 || static_cast<uint64_t>(res) < range.bytes_to_read) {
                        throw std::runtime_error("failed to read prefetch range with io_uring: res=" +
                                                 std::to_string(res) +
                                                 " expected-at-least=" +
                                                 std::to_string(range.bytes_to_read));
                    }
                    if (fixed_slot_pipeline) {
                        if (request_cache_buffers[request_id].valid()) {
                            cache_decode_ranges.push_back(range);
                            cache_decode_buffers.push_back(
                                std::move(request_cache_buffers[request_id]));
                            free_buffer_ids.push_back(requests[request_id].buffer_id);
                            note_fixed_slot_release();
                        } else {
                            ReadyDecode ready;
                            ready.range = range;
                            ready.buffer_id = requests[request_id].buffer_id;
                            ready.buffer_offset = requests[request_id].buffer_offset;
                            decode_batch.push_back(std::move(ready));
                        }
                    } else {
                        ready_batch.push_back(range_index);
                    }
                    free_request_ids.push_back(request_id);
                    --outstanding;
                    ++completed;
                };

                auto wait_start = std::chrono::steady_clock::now();
                while (completed < min_events) {
                    struct io_uring_cqe* cqe = nullptr;
                    int wait_rc = io_uring_wait_cqe(&guard.ring, &cqe);
                    if (wait_rc < 0) {
                        throw std::runtime_error(std::string("io_uring_wait_cqe failed: ") +
                                                 std::strerror(-wait_rc));
                    }
                    process_cqe(cqe);
                }
                wait_ns += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - wait_start).count());

                while (completed < max_events) {
                    struct io_uring_cqe* cqe = nullptr;
                    int peek_rc = io_uring_peek_cqe(&guard.ring, &cqe);
                    if (peek_rc == -EAGAIN) break;
                    if (peek_rc < 0) {
                        throw std::runtime_error(std::string("io_uring_peek_cqe failed: ") +
                                                 std::strerror(-peek_rc));
                    }
                    process_cqe(cqe);
                }

                getevents_calls.fetch_add(1, std::memory_order_relaxed);
                completed_events.fetch_add(static_cast<uint64_t>(completed),
                                           std::memory_order_relaxed);
                pread_ns_total.fetch_add(wait_ns, std::memory_order_relaxed);
                return wait_ns;
            };

            if (caller_worker) {
                submit_until_full();
                if (!cache_slot_pipeline && !defer_cache_decode) {
                    decode_cached_groups();
                }
            }

            while (!stop_workers.load(std::memory_order_acquire)) {
                submit_until_full();
                if (caller_worker && !fixed_slot_pipeline) {
                    *caller_decoded_ranges += drain_ready_ranges(false);
                }

                if (outstanding == 0) {
                    if (no_more_ranges) break;
                    if (caller_worker && !fixed_slot_pipeline) {
                        *caller_decoded_ranges += drain_ready_ranges(true);
                    } else if (!fixed_slot_pipeline) {
                        wait_for_pool_space();
                    } else {
                        std::this_thread::yield();
                    }
                    continue;
                }

                uint64_t completion_wait_ns = collect_completions(
                    std::min<size_t>(outstanding, local_completion_batch));
                submit_until_full();
                if (fixed_slot_pipeline) {
                    if (caller_worker) {
                        stats_.prefetch_wait_ns += completion_wait_ns;
                    }
                    for (size_t i = 0; i < cache_decode_ranges.size(); ++i) {
                        decode_prefetched_cache_buffer(
                            cache_decode_ranges[i],
                            std::move(cache_decode_buffers[i]));
                    }
                    for (ReadyDecode& ready : decode_batch) {
                        decode_prefetched_buffer(
                            ready.range,
                            prefetch_pool.get() + ready.buffer_offset);
                    }
                    for (ReadyDecode& ready : decode_batch) {
                        free_buffer_ids.push_back(ready.buffer_id);
                        note_fixed_slot_release();
                    }
                    submit_until_full();
                } else if (caller_worker) {
                    stats_.prefetch_wait_ns += completion_wait_ns;
                    for (size_t range_index : ready_batch) {
                        slot_states[range_index].store(
                            static_cast<uint8_t>(PrefetchSlotState::Ready),
                            std::memory_order_release);
                        decode_prefetched_range(range_index);
                        release_range(range_index);
                        ++*caller_decoded_ranges;
                    }
                    *caller_decoded_ranges += drain_ready_ranges(false);
                } else {
                    mark_ranges_ready(ready_batch);
                }
            }
        };
#endif

#endif

        const size_t io_stream_count = std::min(worker_count, ranges.size());
        const bool caller_runs_async_io =
            (use_io_uring || use_linux_aio) && io_stream_count != 0;
        const size_t background_workers_to_start =
            io_stream_count - (caller_runs_async_io ? 1 : 0);
        size_t fixed_pipeline_slots_per_worker = 0;
        if (use_io_uring && io_stream_count != 0) {
            fixed_pipeline_slots_per_worker = fixed_pool_blocks / io_stream_count;
            if (fixed_pipeline_slots_per_worker == 0) {
                fixed_pipeline_slots_per_worker = 1;
            }
            if (fixed_pipeline_slots_per_worker * io_stream_count > fixed_pool_blocks) {
                fixed_pipeline_slots_per_worker = 0;
            }
        }
        const bool using_fixed_pipeline =
            fixed_pipeline_slots_per_worker != 0;
        const bool using_cache_slot_pipeline =
            using_fixed_pipeline && use_io_uring &&
            !use_io_uring_fixed && batch_all_lru_admissions;
        const bool promote_fixed_misses_to_lru =
            using_fixed_pipeline && use_io_uring &&
            cache_capacity_bytes_ != 0 &&
            batch_has_lru_admissions;
        const bool decode_cached_groups_before_io =
            using_cache_slot_pipeline || promote_fixed_misses_to_lru;
        stats_.prefetch_slots += static_cast<uint64_t>(ranges.size());
        std::vector<std::thread> workers;
        auto io_start = std::chrono::steady_clock::now();

        stats_.prefetch_workers += static_cast<uint64_t>(io_stream_count);
        stats_.prefetch_background_workers +=
            static_cast<uint64_t>(background_workers_to_start);
        stats_.prefetch_caller_worker += caller_runs_async_io ? 1 : 0;
        stats_.prefetch_queue_depth += static_cast<uint64_t>(
            io_stream_count * effective_depth);
        workers.reserve(background_workers_to_start);

        if (!using_cache_slot_pipeline) {
            prefetch_pool = allocate_aligned_buffer(pool_size, 4096);
            stats_.prefetch_pool_bytes += static_cast<uint64_t>(pool_size);
        }

        if (decode_cached_groups_before_io) {
            decode_cached_groups();
        }

        for (size_t worker = 0; worker < background_workers_to_start; ++worker) {
            workers.emplace_back([&, worker]() {
                try {
#ifdef __linux__
                    if (use_io_uring) {
#if defined(PAGED_AASTORE_ENABLE_IO_URING)
                        run_io_uring_worker(false, nullptr,
                                            worker, fixed_pipeline_slots_per_worker);
                        return;
#else
                        throw std::runtime_error(
                            "--io-backend uring was requested, but this binary was built without liburing support");
#endif
                    }
                    if (use_linux_aio) {
                        run_linux_aio_worker(false, nullptr,
                                             worker, 0);
                        return;
                    }
#endif
                    size_t local_range_next = 0;
                    size_t local_range_end = 0;
                    std::vector<size_t> ready_batch;
                    ready_batch.reserve(range_fetch_chunk);
                    auto reserve_worker_next_range = [&](size_t& range_index,
                                                         PrefetchRange& range) -> bool {
                        if (!direct_fixed_pool) {
                            return reserve_next_range(range_index, range);
                        }
                        if (stop_workers.load(std::memory_order_acquire) ||
                            has_worker_error.load(std::memory_order_acquire)) {
                            return false;
                        }
                        while (local_range_next >= local_range_end) {
                            size_t begin = next_direct_range_to_read.fetch_add(
                                range_fetch_chunk, std::memory_order_relaxed);
                            if (begin >= ranges.size()) return false;
                            local_range_next = begin;
                            local_range_end = std::min(begin + range_fetch_chunk,
                                                       ranges.size());
                        }
                        range_index = local_range_next++;
                        return reserve_direct_range_index(range_index, range);
                    };

                    while (!stop_workers.load(std::memory_order_acquire)) {
                        ready_batch.clear();
                        bool done = false;
                        size_t batch_limit = direct_fixed_pool ? range_fetch_chunk : 1;
                        for (size_t item = 0; item < batch_limit; ++item) {
                            size_t range_index = 0;
                            PrefetchRange range;
                            if (!reserve_worker_next_range(range_index, range)) {
                                done = true;
                                break;
                            }

                            auto pread_start = std::chrono::steady_clock::now();
                            read_exact_at_fd(pages_fd_,
                                             prefetch_pool.get() + slots[range_index].buffer_offset,
                                             range.bytes_to_read, range.file_offset,
                                             "failed to read prefetch range");
                            pread_ns_total.fetch_add(static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - pread_start).count()),
                                std::memory_order_relaxed);
                            ready_batch.push_back(range_index);
                        }
                        mark_ranges_ready(ready_batch);
                        if (done) break;
                    }
                } catch (...) {
                    record_worker_error();
                }
            });
        }

        bool workers_joined = false;

        try {
            size_t decoded_range_count = 0;
            if (use_io_uring && caller_runs_async_io) {
#if defined(PAGED_AASTORE_ENABLE_IO_URING)
                run_io_uring_worker(true, &decoded_range_count,
                                    background_workers_to_start,
                                    fixed_pipeline_slots_per_worker);
#else
                throw std::runtime_error(
                    "--io-backend uring was requested, but this binary was built without liburing support");
#endif
            } else if (use_linux_aio && caller_runs_async_io) {
#ifdef __linux__
                run_linux_aio_worker(true, &decoded_range_count,
                                     background_workers_to_start, 0);
#else
                throw std::runtime_error("--io-backend aio requires Linux");
#endif
            } else {
                decode_cached_groups();
            }

            if (using_fixed_pipeline) {
                for (std::thread& worker : workers) {
                    if (worker.joinable()) worker.join();
                }
                workers_joined = true;
                wait_until_no_error();
                decoded_range_count =
                    static_cast<size_t>(fixed_slot_decoded_ranges.load(
                        std::memory_order_relaxed));
                if (decoded_range_count != ranges.size()) {
                    throw std::runtime_error(
                        "fixed-slot io_uring decoded range count mismatch");
                }
            } else {
                while (decoded_range_count < ranges.size()) {
                    decoded_range_count += drain_ready_ranges(true);
                }
            }
        } catch (...) {
            stop_and_join(workers);
            throw;
        }

        if (!workers_joined) {
            stop_and_join(workers);
        }
        stats_.prefetch_io_wall_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - io_start).count());
        if (using_fixed_pipeline) {
            uint64_t peak_buffers =
                fixed_slot_active_peak_buffers.load(std::memory_order_relaxed);
            active_prefetch_peak_ranges = peak_buffers;
            active_prefetch_peak_bytes = peak_buffers * fixed_block_bytes;
        }
        stats_.prefetch_active_peak_bytes += active_prefetch_peak_bytes;
        stats_.prefetch_active_peak_ranges += active_prefetch_peak_ranges;
        stats_.prefetch_pool_wait_ns += pool_wait_ns.load(std::memory_order_relaxed);
        stats_.prefetch_submit_ns_total += submit_ns_total.load(std::memory_order_relaxed);
        stats_.prefetch_submit_calls += submit_calls.load(std::memory_order_relaxed);
        stats_.prefetch_submitted_requests += submitted_requests.load(std::memory_order_relaxed);
        stats_.prefetch_getevents_calls += getevents_calls.load(std::memory_order_relaxed);
        stats_.prefetch_completed_events += completed_events.load(std::memory_order_relaxed);
        stats_.prefetch_pread_ns_total += pread_ns_total.load(std::memory_order_relaxed);
        stats_.prefetch_ready_push_ns += ready_push_ns.load(std::memory_order_relaxed);
        if (using_fixed_pipeline) {
            stats_.misses += fixed_slot_decoded_ranges.load(std::memory_order_relaxed);
            stats_.prefetch_miss_decode_ns +=
                fixed_slot_decode_ns.load(std::memory_order_relaxed);
        }
        wait_until_no_error();
    }

    static constexpr uint64_t invalid_cache_slot() {
        return std::numeric_limits<uint64_t>::max();
    }

    struct CacheWriteBuffer {
        uint8_t* data = nullptr;
        AlignedBuffer owned;
        uint64_t slot_id = invalid_cache_slot();
        uint64_t page_id = 0;

        bool valid() const { return data != nullptr; }
    };

    uint64_t n_ = 0;
    uint64_t total_length_ = 0;
    uint64_t page_residues_ = 0;
    uint64_t page_count_ = 0;
    uint64_t packed_page_bytes_ = 0;
    bool stop_aware_encoding_ = false;
    std::string meta_path_;
    std::string pages_path_;
    std::string names_path_;
    std::vector<uint64_t> start_positions_;
    std::string names_blob_;
    std::vector<uint32_t> name_offsets32_;
    std::vector<uint64_t> name_offsets64_;
    std::vector<uint16_t> name_fingerprint_slots_;
    std::vector<uint32_t> name_id_slots_;
    size_t name_hash_mask_ = 0;
    std::vector<uint8_t> full_cache_pages_;
    sdsl::int_vector<5> prefix_cache_codes_;
    AlignedBuffer prefix_cache_packed_;
    AlignedBuffer lru_cache_packed_;
    uint64_t lru_cache_slot_count_ = 0;
    std::vector<uint64_t> page_to_lru_slot_;
    std::vector<uint64_t> lru_slot_to_page_;
    std::vector<uint8_t> lru_ref_bits_;
    std::vector<uint64_t> free_lru_slots_;
    std::vector<uint16_t> lru_recent_miss_epoch_;
    AlignedBuffer single_page_buffer_;
    uint64_t prefix_cache_pages_ = 0;
    uint64_t prefix_cache_stride_residues_ = 0;
    uint64_t prefix_cache_stride_bytes_ = 0;

    int pages_fd_ = -1;
    uint64_t cache_capacity_bytes_ = 0;
    uint64_t current_cache_bytes_ = 0;
    static size_t automatic_prefetch_depth(size_t worker_count,
                                           uint64_t prefetch_memory_bytes,
                                           uint64_t page_bytes) {
        const uint64_t safe_page_bytes = std::max<uint64_t>(1, page_bytes);
        size_t blocks_by_memory = static_cast<size_t>(
            std::max<uint64_t>(1, prefetch_memory_bytes / safe_page_bytes));
        const size_t streams = std::max<size_t>(1, worker_count);
        const size_t divisor = streams > std::numeric_limits<size_t>::max() / 2
            ? std::numeric_limits<size_t>::max()
            : streams * 2;
        size_t depth = blocks_by_memory / std::max<size_t>(1, divisor);
        depth = std::max<size_t>(32, depth);
        depth = std::min<size_t>(512, depth);
        return depth;
    }

    static size_t normalize_prefetch_threads(size_t threads) {
        return threads <= 1 ? 1 : threads;
    }

    static uint64_t cap_prefetch_memory_bytes(uint64_t bytes) {
        if (bytes == 0) return 0;
        return std::min<uint64_t>(bytes, default_prefetch_memory_cap_bytes());
    }

    // 0 means auto. Users give memory; the library chooses queue depth/window.
    size_t prefetch_depth_ = 0;
    PrefetchIoBackend prefetch_io_backend_ = PrefetchIoBackend::Auto;
    bool uring_sqpoll_ = false;
    size_t default_batch_prefetch_threads_ = 2;
    size_t full_cache_decode_threads_ = 1;
    uint64_t default_batch_prefetch_memory_bytes_ =
        default_prefetch_memory_cap_bytes();
    ResidentCachePolicy resident_cache_policy_ = ResidentCachePolicy::Lru;
    bool single_thread_aio_ = false;
    size_t single_thread_aio_completion_batch_ = 64;
    size_t aio_completion_min_ = 4;
    uint64_t lru_clock_hand_slot_ = 0;
    uint64_t lru_live_entries_ = 0;
    uint16_t lru_admission_epoch_ = 0;
    std::mutex cache_mutex_;
    CacheStats stats_;

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

    static inline void set_packed_5bit(std::vector<uint8_t>& packed,
                                       uint64_t offset,
                                       uint8_t code) {
        set_packed_5bit(packed.data(), offset, code);
    }

    static inline void append_ascii_5bit(uint8_t* packed,
                                         uint64_t& offset,
                                         const char* src,
                                         size_t len,
                                         EncodingMode encoding_mode = EncodingMode::StopAware) {
        if (encoding_mode == EncodingMode::StopAware) {
            append_ascii_5bit_stop_aware(packed, offset, src, len);
            return;
        }

        size_t pos = 0;
        while (pos < len && (offset & 7ULL) != 0) {
            set_packed_5bit(packed, offset, ProteinAAStore::enc(src[pos]));
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
            set_packed_5bit(packed, offset, ProteinAAStore::enc(src[pos]));
            ++offset;
            ++pos;
        }
    }

    static inline void append_ascii_5bit(std::vector<uint8_t>& packed,
                                         uint64_t& offset,
                                         const char* src,
                                         size_t len,
                                         EncodingMode encoding_mode = EncodingMode::StopAware) {
        append_ascii_5bit(packed.data(), offset, src, len, encoding_mode);
    }

    static inline void append_ascii_5bit_stop_aware(uint8_t* packed,
                                                    uint64_t& offset,
                                                    const char* src,
                                                    size_t len) {
        size_t pos = 0;
        while (pos < len && (offset & 7ULL) != 0) {
            set_packed_5bit(
                packed, offset,
                aastore_detail::encode_aa_5bit_stop_branchless(src[pos]));
            ++offset;
            ++pos;
        }

        while (pos + 64 <= len) {
            size_t byte_pos = static_cast<size_t>((offset >> 3) * 5);
            aastore_detail::pack_64_ascii_5bit_hybrid_stop(src + pos,
                                                           packed + byte_pos);
            offset += 64;
            pos += 64;
        }

        while (pos + 8 <= len) {
            size_t byte_pos = static_cast<size_t>((offset >> 3) * 5);
            aastore_detail::pack_8_ascii_5bit_stop_scalar(src + pos,
                                                          packed + byte_pos);
            offset += 8;
            pos += 8;
        }

        while (pos < len) {
            set_packed_5bit(
                packed, offset,
                aastore_detail::encode_aa_5bit_stop_branchless(src[pos]));
            ++offset;
            ++pos;
        }
    }

    static BuildTaskResult pack_build_task(const BuildTask& task) {
        BuildTaskResult result;
        result.task_id = task.task_id;
        if (task.page_residues == 0) {
            throw std::runtime_error("build task has zero page size");
        }

        const uint64_t raw_bytes = static_cast<uint64_t>(task.raw_size);
        const uint64_t page_count =
            raw_bytes == 0 ? 0 : (raw_bytes + task.page_residues - 1) / task.page_residues;
        size_t total_packed_bytes = 0;
        if (page_count != 0) {
            total_packed_bytes = static_cast<size_t>(
                (page_count - 1) * packed_bytes_for_residues(task.page_residues) +
                packed_bytes_for_residues(
                    raw_bytes - (page_count - 1) * task.page_residues));
        }
        result.packed_block.assign(total_packed_bytes, 0);
        size_t block_offset = 0;
        size_t raw_offset = 0;
        const char* raw_data = task.raw.get();
        while (raw_offset < task.raw_size) {
            size_t residues = std::min<size_t>(
                static_cast<size_t>(task.page_residues),
                task.raw_size - raw_offset);
            size_t page_bytes = static_cast<size_t>(
                packed_bytes_for_residues(static_cast<uint64_t>(residues)));
            uint64_t offset = 0;
            append_ascii_5bit(result.packed_block.data() + block_offset, offset,
                              raw_data + raw_offset, residues,
                              task.encoding_mode);
            block_offset += page_bytes;
            raw_offset += residues;
        }
        return result;
    }

    static void pack_5bit(const std::vector<uint8_t>& codes, std::vector<uint8_t>& packed) {
        packed.assign(static_cast<size_t>(packed_bytes_for_residues(static_cast<uint64_t>(codes.size()))), 0);
        for (size_t i = 0; i < codes.size(); ++i) {
            set_packed_5bit(packed, static_cast<uint64_t>(i), codes[i]);
        }
    }

    static uint64_t int_vector_storage_bytes(uint64_t residues) {
        return ((residues * 5 + 63) / 64) * sizeof(uint64_t);
    }

    static uint64_t packed_bytes_for_residues(uint64_t residues) {
        return (residues * 5 + 7) / 8;
    }

    static uint64_t direct_cache_stride_residues(uint64_t residues) {
        return ((residues + 63) / 64) * 64;
    }

    static uint64_t direct_cache_stride_bytes(uint64_t stride_residues) {
        return (stride_residues * 5) / 8;
    }

    static uint64_t computed_page_count(uint64_t total_length, uint64_t page_residues) {
        if (page_residues == 0) return 0;
        return (total_length + page_residues - 1) / page_residues;
    }

    uint64_t residues_in_page(uint64_t page_id) const {
        if (page_id >= page_count_) {
            throw std::out_of_range("page id out of range");
        }
        if (page_id + 1 < page_count_) {
            return page_residues_;
        }

        uint64_t tail = total_length_ % page_residues_;
        return tail == 0 ? page_residues_ : tail;
    }

    uint64_t page_file_offset(uint64_t page_id) const {
        return page_id * packed_page_bytes_;
    }

    uint64_t expected_pages_file_bytes() const {
        if (page_count_ == 0) return 0;
        return (page_count_ - 1) * packed_page_bytes_ +
               packed_bytes_for_residues(residues_in_page(page_count_ - 1));
    }

    bool validate_pages_file_size(const std::string& pages_path) const {
        try {
            uint64_t actual = static_cast<uint64_t>(std::filesystem::file_size(pages_path));
            return actual >= expected_pages_file_bytes();
        } catch (...) {
            return false;
        }
    }

    static uint64_t file_size_or_zero(const std::string& path) {
        try {
            return static_cast<uint64_t>(std::filesystem::file_size(path));
        } catch (...) {
            return 0;
        }
    }

    bool validate_start_positions() const {
        if (start_positions_.size() != static_cast<size_t>(n_ + 1)) return false;
        if (start_positions_.empty()) return n_ == 0 && total_length_ == 0;
        if (start_positions_.front() != 0) return false;
        if (start_positions_.back() != total_length_) return false;
        for (size_t i = 1; i < start_positions_.size(); ++i) {
            if (start_positions_[i] < start_positions_[i - 1]) return false;
        }
        return true;
    }

    void assign_name_offsets(std::vector<uint64_t>&& offsets,
                             uint64_t names_bytes) {
        name_offsets32_.clear();
        name_offsets64_.clear();
        if (names_bytes <=
            static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
            name_offsets32_.reserve(offsets.size());
            for (uint64_t offset : offsets) {
                name_offsets32_.push_back(static_cast<uint32_t>(offset));
            }
        } else {
            name_offsets64_ = std::move(offsets);
        }
    }

    uint64_t name_offset(size_t index) const {
        if (!name_offsets32_.empty()) return name_offsets32_[index];
        return name_offsets64_[index];
    }

    std::string_view name_view(size_t seq_id) const {
        uint64_t begin = name_offset(seq_id);
        uint64_t end = name_offset(seq_id + 1);
        if (end > begin && names_blob_[static_cast<size_t>(end - 1)] == '\n') {
            --end;
        }
        if (end > begin && names_blob_[static_cast<size_t>(end - 1)] == '\r') {
            --end;
        }
        return std::string_view(names_blob_.data() + static_cast<size_t>(begin),
                                static_cast<size_t>(end - begin));
    }

    std::vector<size_t> resolve_names_to_ids(
            const std::vector<std::string_view>& names) const {
        if (!names_loaded()) {
            throw std::runtime_error("sequence names are not loaded");
        }

        std::vector<size_t> ids(names.size());
        const size_t requested_threads =
            std::min(full_cache_decode_threads_, names.size());
        if (requested_threads <= 1 || names.size() < 4096) {
            for (size_t i = 0; i < names.size(); ++i) {
                size_t seq_id = 0;
                if (!find_name_id(names[i], seq_id)) {
                    throw std::out_of_range("sequence name not found");
                }
                ids[i] = seq_id;
            }
            return ids;
        }

        std::vector<std::thread> workers;
        std::exception_ptr error = nullptr;
        std::mutex error_mutex;
        workers.reserve(requested_threads);
        const size_t chunk = (names.size() + requested_threads - 1) / requested_threads;

        for (size_t t = 0; t < requested_threads; ++t) {
            const size_t begin = t * chunk;
            const size_t end = std::min(names.size(), begin + chunk);
            if (begin >= end) break;
            workers.emplace_back([&, begin, end]() {
                try {
                    for (size_t i = begin; i < end; ++i) {
                        size_t seq_id = 0;
                        if (!find_name_id(names[i], seq_id)) {
                            throw std::out_of_range("sequence name not found");
                        }
                        ids[i] = seq_id;
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (!error) error = std::current_exception();
                }
            });
        }

        for (std::thread& worker : workers) worker.join();
        if (error) std::rethrow_exception(error);
        return ids;
    }

    static constexpr uint32_t invalid_name_id() {
        return std::numeric_limits<uint32_t>::max();
    }

    static constexpr uint64_t kNameIndexMagic = 0x31415844494E4141ULL;
    static constexpr uint64_t kNameIndexVersion = 3;

    static size_t next_power_of_two(uint64_t value) {
        size_t result = 1;
        while (result < value) {
            if (result > std::numeric_limits<size_t>::max() / 2) {
                throw std::runtime_error("name hash table is too large");
            }
            result <<= 1;
        }
        return result;
    }

    static uint64_t hash_name(std::string_view name) {
        uint64_t hash = 1469598103934665603ULL;
        for (unsigned char c : name) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return hash == 0 ? 1 : hash;
    }

    static uint64_t mix_hash(uint64_t value) {
        value += 0x9E3779B97F4A7C15ULL;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31);
    }

    static uint16_t name_fingerprint(uint64_t hash) {
        uint64_t mixed = hash ^ (hash >> 32) ^ (hash >> 16);
        return static_cast<uint16_t>(mixed);
    }

    bool find_name_id(std::string_view name, size_t& seq_id) const {
        if (name_id_slots_.empty()) return false;
        uint64_t hash = hash_name(name);
        uint16_t fingerprint = name_fingerprint(hash);
        size_t slot = static_cast<size_t>(mix_hash(hash)) & name_hash_mask_;
        while (true) {
            uint32_t candidate = name_id_slots_[slot];
            if (candidate == invalid_name_id()) return false;
            if (name_fingerprint_slots_[slot] == fingerprint) {
                if (candidate != invalid_name_id() && name_view(candidate) == name) {
                    seq_id = candidate;
                    return true;
                }
            }
            slot = (slot + 1) & name_hash_mask_;
        }
    }

    bool load_name_index(const std::string& index_path, uint64_t names_bytes) {
        std::ifstream in(index_path, std::ios::binary);
        if (!in) return false;

        uint64_t header[6] = {};
        in.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!in) return false;
        if (header[0] != kNameIndexMagic || header[1] != kNameIndexVersion) {
            return false;
        }
        if (header[2] != n_ || header[3] != names_bytes) {
            return false;
        }
        const uint64_t slot_count_u64 = header[4];
        if (slot_count_u64 == 0 ||
            (slot_count_u64 & (slot_count_u64 - 1)) != 0 ||
            slot_count_u64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            return false;
        }
        const size_t slot_count = static_cast<size_t>(slot_count_u64);
        if (n_ > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - 1)) {
            return false;
        }

        std::vector<uint16_t> fingerprint_slots(slot_count);
        std::vector<uint32_t> id_slots(slot_count);

        const uint64_t offset_width = header[5];
        std::vector<uint32_t> offsets32;
        std::vector<uint64_t> offsets64;
        if (offset_width == 4) {
            offsets32.resize(static_cast<size_t>(n_ + 1));
            in.read(reinterpret_cast<char*>(offsets32.data()),
                    static_cast<std::streamsize>(offsets32.size() * sizeof(uint32_t)));
            if (!in) return false;
        } else if (offset_width == 8) {
            offsets64.resize(static_cast<size_t>(n_ + 1));
            in.read(reinterpret_cast<char*>(offsets64.data()),
                    static_cast<std::streamsize>(offsets64.size() * sizeof(uint64_t)));
            if (!in) return false;
        } else {
            return false;
        }
        in.read(reinterpret_cast<char*>(fingerprint_slots.data()),
                static_cast<std::streamsize>(
                    fingerprint_slots.size() * sizeof(uint16_t)));
        if (!in) return false;
        in.read(reinterpret_cast<char*>(id_slots.data()),
                static_cast<std::streamsize>(id_slots.size() * sizeof(uint32_t)));
        if (!in) return false;

        auto loaded_offset = [&](size_t index) -> uint64_t {
            if (!offsets32.empty()) return offsets32[index];
            return offsets64[index];
        };
        const size_t offset_count = static_cast<size_t>(n_ + 1);
        if (offset_count == 0 || loaded_offset(0) != 0 ||
            loaded_offset(offset_count - 1) != names_bytes) {
            return false;
        }
        for (size_t i = 1; i < offset_count; ++i) {
            if (loaded_offset(i) < loaded_offset(i - 1)) return false;
        }

        name_offsets32_.swap(offsets32);
        name_offsets64_.swap(offsets64);
        name_fingerprint_slots_.swap(fingerprint_slots);
        name_id_slots_.swap(id_slots);
        name_hash_mask_ = slot_count - 1;
        return true;
    }

    bool convert_legacy_seq_meta(const std::vector<LegacySeqMeta>& legacy_meta) {
        start_positions_.assign(legacy_meta.size() + 1, 0);
        uint64_t expected_start = 0;
        for (size_t i = 0; i < legacy_meta.size(); ++i) {
            const LegacySeqMeta& meta = legacy_meta[i];
            uint64_t start = meta.page_id * page_residues_ + meta.page_offset;
            if (start != expected_start) return false;
            if (start > total_length_ || meta.length > total_length_ - start) return false;

            start_positions_[i] = start;
            expected_start = start + meta.length;
            start_positions_[i + 1] = expected_start;
        }
        return validate_start_positions();
    }

    sdsl::int_vector<5> read_page_from_fd(uint64_t page_id) const {
        if (page_id >= page_count_) {
            throw std::out_of_range("page id out of range");
        }

        uint64_t residues = residues_in_page(page_id);
        uint64_t packed_bytes = packed_bytes_for_residues(residues);
        sdsl::int_vector<5> codes(static_cast<size_t>(residues), 0);
        read_exact_at_fd(pages_fd_, codes.data(), packed_bytes,
                         page_file_offset(page_id), "failed to read page");
        return codes;
    }

    uint64_t packed_page_storage_bytes(uint64_t page_id) const {
        (void)page_id;
        return packed_page_bytes_;
    }

    AlignedBuffer allocate_packed_page_buffer() const {
        return allocate_aligned_buffer(static_cast<size_t>(packed_page_bytes_), 4096);
    }

    AlignedBuffer read_packed_page_from_fd(uint64_t page_id) const {
        if (page_id >= page_count_) {
            throw std::out_of_range("page id out of range");
        }
        AlignedBuffer packed = allocate_packed_page_buffer();
        uint64_t packed_bytes = packed_bytes_for_residues(residues_in_page(page_id));
        read_exact_at_fd(pages_fd_, packed.get(), packed_bytes,
                         page_file_offset(page_id), "failed to read packed page");
        return packed;
    }

    uint64_t lru_slot_capacity_for_limit(uint64_t limit) const {
        if (page_count_ == 0 || packed_page_bytes_ == 0 || limit == 0) {
            return 0;
        }
        uint64_t slots = limit / packed_page_bytes_;
        if (slots == 0) slots = 1;
        return std::min<uint64_t>(page_count_, slots);
    }

    static constexpr uint16_t lru_admission_window_epochs() {
        return 32;
    }

    uint16_t next_lru_admission_epoch() {
        if (resident_cache_policy_ != ResidentCachePolicy::Lru || page_count_ == 0) {
            return 0;
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        if (lru_recent_miss_epoch_.size() != static_cast<size_t>(page_count_)) {
            lru_recent_miss_epoch_.assign(static_cast<size_t>(page_count_), 0);
            lru_admission_epoch_ = 0;
        }
        ++lru_admission_epoch_;
        if (lru_admission_epoch_ == 0) {
            std::fill(lru_recent_miss_epoch_.begin(), lru_recent_miss_epoch_.end(), 0);
            lru_admission_epoch_ = 1;
        }
        return lru_admission_epoch_;
    }

    bool lru_should_admit_page(uint64_t page_id, uint16_t epoch) {
        if (resident_cache_policy_ != ResidentCachePolicy::Lru || epoch == 0 ||
            page_id >= page_count_) {
            return false;
        }

        // Batch planning runs before I/O workers can promote pages, so this direct
        // table update stays off the per-miss cache mutex path.
        if (lru_recent_miss_epoch_.size() != static_cast<size_t>(page_count_)) {
            throw std::runtime_error("LRU admission index is not initialized");
        }
        uint16_t previous = lru_recent_miss_epoch_[static_cast<size_t>(page_id)];
        bool admit = previous != 0 &&
            static_cast<uint16_t>(epoch - previous) <= lru_admission_window_epochs();
        lru_recent_miss_epoch_[static_cast<size_t>(page_id)] = epoch;
        if (admit) {
            ++stats_.lru_admission_promotions;
        } else {
            ++stats_.lru_admission_first_misses;
        }
        return admit;
    }

    void reset_lru_cache_index(uint64_t slot_count, bool mark_free_slots) {
        if (slot_count > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("LRU cache slot count is too large");
        }
        if (slot_count != 0 &&
            slot_count > std::numeric_limits<uint64_t>::max() / packed_page_bytes_) {
            throw std::runtime_error("LRU cache arena size overflow");
        }

        uint64_t arena_bytes = slot_count * packed_page_bytes_;
        if (arena_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("LRU cache arena is too large");
        }
        lru_cache_packed_.reset();
        if (arena_bytes != 0) {
            lru_cache_packed_ =
                allocate_aligned_buffer(static_cast<size_t>(arena_bytes), 4096);
        }
        lru_cache_slot_count_ = slot_count;
        page_to_lru_slot_.assign(static_cast<size_t>(page_count_), invalid_cache_slot());
        lru_slot_to_page_.assign(static_cast<size_t>(slot_count), invalid_cache_slot());
        lru_ref_bits_.assign(static_cast<size_t>(slot_count), 0);
        free_lru_slots_.clear();
        if (mark_free_slots) {
            free_lru_slots_.reserve(static_cast<size_t>(slot_count));
            for (uint64_t slot = slot_count; slot > 0; --slot) {
                free_lru_slots_.push_back(slot - 1);
            }
        }
        lru_clock_hand_slot_ = 0;
        lru_live_entries_ = 0;
        current_cache_bytes_ = 0;
    }

    void ensure_lru_cache_index_unlocked(uint64_t desired_slots) {
        if (desired_slots == 0) return;
        if (lru_cache_slot_count_ == 0) {
            reset_lru_cache_index(desired_slots, true);
            return;
        }
        if (page_to_lru_slot_.size() != static_cast<size_t>(page_count_)) {
            throw std::runtime_error("LRU page index size mismatch");
        }
        if (desired_slots <= lru_cache_slot_count_) {
            return;
        }
        if (desired_slots > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            desired_slots > std::numeric_limits<uint64_t>::max() / packed_page_bytes_) {
            throw std::runtime_error("LRU cache arena size overflow");
        }
        uint64_t old_slots = lru_cache_slot_count_;
        uint64_t new_bytes = desired_slots * packed_page_bytes_;
        if (new_bytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("LRU cache arena is too large");
        }
        AlignedBuffer grown =
            allocate_aligned_buffer(static_cast<size_t>(new_bytes), 4096);
        if (lru_cache_packed_ && old_slots != 0) {
            std::memcpy(grown.get(), lru_cache_packed_.get(),
                        static_cast<size_t>(old_slots * packed_page_bytes_));
        }
        lru_cache_packed_ = std::move(grown);
        lru_cache_slot_count_ = desired_slots;
        lru_slot_to_page_.resize(static_cast<size_t>(desired_slots),
                                 invalid_cache_slot());
        lru_ref_bits_.resize(static_cast<size_t>(desired_slots), 0);
        for (uint64_t slot = desired_slots; slot > old_slots; --slot) {
            free_lru_slots_.push_back(slot - 1);
        }
    }

    uint64_t lru_slot_for_page_unlocked(uint64_t page_id) const {
        if (page_id >= page_to_lru_slot_.size()) return invalid_cache_slot();
        uint64_t slot = page_to_lru_slot_[static_cast<size_t>(page_id)];
        if (slot >= lru_slot_to_page_.size() ||
            lru_slot_to_page_[static_cast<size_t>(slot)] != page_id) {
            return invalid_cache_slot();
        }
        return slot;
    }

    void lru_mark_referenced_unlocked(uint64_t slot) {
        if (slot >= lru_cache_slot_count_) {
            throw std::runtime_error("invalid LRU slot");
        }
        lru_ref_bits_[static_cast<size_t>(slot)] = 1;
    }

    uint64_t evict_lru_clock_slot_unlocked(uint64_t protected_page_id) {
        if (lru_cache_slot_count_ == 0 || lru_live_entries_ == 0) {
            return invalid_cache_slot();
        }
        uint64_t scans_left = lru_cache_slot_count_ * 2 + 1;
        while (scans_left-- != 0) {
            uint64_t slot = lru_clock_hand_slot_ % lru_cache_slot_count_;
            lru_clock_hand_slot_ = (slot + 1) % lru_cache_slot_count_;
            uint64_t victim = lru_slot_to_page_[static_cast<size_t>(slot)];
            if (victim == invalid_cache_slot() || victim == protected_page_id) {
                continue;
            }
            if (lru_ref_bits_[static_cast<size_t>(slot)] != 0) {
                lru_ref_bits_[static_cast<size_t>(slot)] = 0;
                continue;
            }
            page_to_lru_slot_[static_cast<size_t>(victim)] = invalid_cache_slot();
            lru_slot_to_page_[static_cast<size_t>(slot)] = invalid_cache_slot();
            current_cache_bytes_ -= packed_page_storage_bytes(victim);
            if (lru_live_entries_ != 0) --lru_live_entries_;
            ++stats_.evictions;
            return slot;
        }
        return invalid_cache_slot();
    }

    uint64_t preload_lru_cache_sequential(uint64_t limit) {
        uint64_t load_pages = lru_slot_capacity_for_limit(limit);
        if (load_pages == 0) {
            return 0;
        }

        reset_lru_cache_index(load_pages, false);

        uint64_t bytes_to_read =
            (load_pages - 1) * packed_page_bytes_ +
            packed_bytes_for_residues(residues_in_page(load_pages - 1));
        read_exact_at_fd(pages_fd_, lru_cache_packed_.get(), bytes_to_read, 0,
                         "failed to read packed pages during LRU preload");

        for (uint64_t page_id = 0; page_id < load_pages; ++page_id) {
            uint64_t slot = page_id;
            page_to_lru_slot_[static_cast<size_t>(page_id)] = slot;
            lru_slot_to_page_[static_cast<size_t>(slot)] = page_id;
            ++lru_live_entries_;
            current_cache_bytes_ += packed_page_storage_bytes(page_id);
        }
        return load_pages;
    }

    uint8_t* lru_cache_slot_data(uint64_t slot_id) const {
        if (!lru_cache_packed_ || slot_id >= lru_cache_slot_count_) {
            return nullptr;
        }
        return lru_cache_packed_.get() +
               static_cast<size_t>(slot_id * packed_page_bytes_);
    }

    PageView find_cached_page(uint64_t page_id, bool touch_lru) {
        if (!full_cache_pages_.empty() && page_id < page_count_) {
            return PageView{
                nullptr,
                0,
                residues_in_page(page_id),
                full_cache_pages_.data() + static_cast<size_t>(page_file_offset(page_id))
            };
        }

        if (page_id < prefix_cache_pages_) {
            return PageView{
                nullptr,
                0,
                residues_in_page(page_id),
                prefix_cache_packed_.get() +
                    static_cast<size_t>(page_id * prefix_cache_stride_bytes_)
            };
        }

        uint64_t slot = lru_slot_for_page_unlocked(page_id);
        if (slot == invalid_cache_slot()) return PageView{};
        if (touch_lru) {
            lru_mark_referenced_unlocked(slot);
        }
        return PageView{nullptr, 0, residues_in_page(page_id),
                        lru_cache_slot_data(slot)};
    }

    PageView insert_page_into_cache(uint64_t page_id, sdsl::int_vector<5>&& codes) {
        uint64_t residues = static_cast<uint64_t>(codes.size());
        CacheWriteBuffer buffer = reserve_cache_write_buffer(page_id);
        uint64_t page_bytes = packed_bytes_for_residues(residues);
        std::memcpy(buffer.data, codes.data(), static_cast<size_t>(page_bytes));
        return insert_reserved_cache_write_buffer(page_id, std::move(buffer), residues);
    }

    void insert_packed_page_into_cache(uint64_t page_id, const uint8_t* packed) {
        if (packed == nullptr || page_id >= page_count_ || !full_cache_pages_.empty()) {
            return;
        }
        if (resident_cache_policy_ == ResidentCachePolicy::Prefix) {
            return;
        }
        uint64_t residues = residues_in_page(page_id);
        uint64_t page_bytes = packed_bytes_for_residues(residues);
        CacheWriteBuffer buffer = reserve_cache_write_buffer(page_id);
        std::memcpy(buffer.data, packed, static_cast<size_t>(page_bytes));
        insert_reserved_cache_write_buffer(page_id, std::move(buffer), residues);
    }

    CacheWriteBuffer reserve_cache_write_buffer(uint64_t page_id) {
        if (page_id >= page_count_) {
            throw std::out_of_range("page id out of range");
        }
        CacheWriteBuffer reusable;
        reusable.page_id = page_id;
        uint64_t storage_bytes = packed_page_storage_bytes(page_id);
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            ensure_lru_cache_index_unlocked(
                lru_slot_capacity_for_limit(cache_capacity_bytes_));

            uint64_t existing_slot = lru_slot_for_page_unlocked(page_id);
            if (existing_slot != invalid_cache_slot()) {
                reusable.slot_id = existing_slot;
                current_cache_bytes_ -= packed_page_storage_bytes(page_id);
                page_to_lru_slot_[static_cast<size_t>(page_id)] =
                    invalid_cache_slot();
                lru_slot_to_page_[static_cast<size_t>(existing_slot)] =
                    invalid_cache_slot();
                lru_ref_bits_[static_cast<size_t>(existing_slot)] = 0;
                if (lru_live_entries_ != 0) {
                    --lru_live_entries_;
                }
            } else {
                while (lru_live_entries_ != 0 &&
                       current_cache_bytes_ + storage_bytes > cache_capacity_bytes_) {
                    uint64_t victim_slot = evict_lru_clock_slot_unlocked(page_id);
                    if (victim_slot == invalid_cache_slot()) break;
                    if (reusable.slot_id == invalid_cache_slot()) {
                        reusable.slot_id = victim_slot;
                    } else {
                        free_lru_slots_.push_back(victim_slot);
                    }
                }
            }
            if (reusable.slot_id == invalid_cache_slot()) {
                if (!free_lru_slots_.empty()) {
                    reusable.slot_id = free_lru_slots_.back();
                    free_lru_slots_.pop_back();
                } else if (lru_live_entries_ != 0) {
                    reusable.slot_id = evict_lru_clock_slot_unlocked(page_id);
                }
            }
            current_cache_bytes_ += storage_bytes;
        }
        if (reusable.slot_id != invalid_cache_slot()) {
            reusable.data = lru_cache_slot_data(reusable.slot_id);
        }
        if (reusable.data == nullptr) {
            throw std::runtime_error("failed to reserve LRU cache slot");
        }
        return reusable;
    }

    PageView insert_reserved_cache_write_buffer(uint64_t page_id,
                                                CacheWriteBuffer&& buffer,
                                                uint64_t residues) {
        if (resident_cache_policy_ == ResidentCachePolicy::Prefix) {
            return find_cached_page(page_id, false);
        }

        if (!buffer.valid() || page_id >= page_count_ || !full_cache_pages_.empty() ||
            page_id < prefix_cache_pages_) {
            return find_cached_page(page_id, false);
        }

        uint64_t storage_bytes = packed_page_storage_bytes(page_id);
        if (buffer.slot_id == invalid_cache_slot()) {
            CacheWriteBuffer reserved = reserve_cache_write_buffer(page_id);
            std::memcpy(reserved.data, buffer.data,
                        static_cast<size_t>(packed_bytes_for_residues(residues)));
            buffer = std::move(reserved);
        }

        std::lock_guard<std::mutex> lock(cache_mutex_);
        uint64_t existing_slot = lru_slot_for_page_unlocked(page_id);
        if (existing_slot != invalid_cache_slot()) {
            if (current_cache_bytes_ >= storage_bytes) {
                current_cache_bytes_ -= storage_bytes;
            }
            if (buffer.slot_id != existing_slot &&
                buffer.slot_id != invalid_cache_slot()) {
                free_lru_slots_.push_back(buffer.slot_id);
            }
            lru_mark_referenced_unlocked(existing_slot);
            return PageView{nullptr, 0, residues_in_page(page_id),
                            lru_cache_slot_data(existing_slot)};
        }

        if (buffer.slot_id == invalid_cache_slot() ||
            buffer.slot_id >= lru_cache_slot_count_ ||
            lru_slot_to_page_[static_cast<size_t>(buffer.slot_id)] !=
                invalid_cache_slot()) {
            throw std::runtime_error("invalid reserved LRU cache slot");
        }
        page_to_lru_slot_[static_cast<size_t>(page_id)] = buffer.slot_id;
        lru_slot_to_page_[static_cast<size_t>(buffer.slot_id)] = page_id;
        lru_mark_referenced_unlocked(buffer.slot_id);
        ++lru_live_entries_;

        evict_lru_locked(page_id);
        return PageView{nullptr, 0, residues,
                        lru_cache_slot_data(buffer.slot_id)};
    }

    PageView insert_reserved_packed_page_into_cache(uint64_t page_id,
                                                    AlignedBuffer&& packed,
                                                    bool already_reserved) {
        (void)already_reserved;
        CacheWriteBuffer buffer = reserve_cache_write_buffer(page_id);
        uint64_t residues = residues_in_page(page_id);
        uint64_t page_bytes = packed_bytes_for_residues(residues);
        std::memcpy(buffer.data, packed.get(), static_cast<size_t>(page_bytes));
        return insert_reserved_cache_write_buffer(
            page_id, std::move(buffer), residues);
    }

    PageView get_page(uint64_t page_id) {
        PageView cached = find_cached_page(page_id, true);
        if (cached.valid()) {
            ++stats_.hits;
            return cached;
        }

        if (page_id >= page_count_) {
            throw std::out_of_range("page id out of range");
        }
        if (resident_cache_policy_ == ResidentCachePolicy::Prefix) {
            if (!single_page_buffer_) {
                single_page_buffer_ = allocate_packed_page_buffer();
            }
            read_exact_at_fd(pages_fd_, single_page_buffer_.get(),
                             packed_bytes_for_residues(residues_in_page(page_id)),
                             page_file_offset(page_id), "failed to read page");
            ++stats_.misses;
            return PageView{nullptr, 0, residues_in_page(page_id),
                            single_page_buffer_.get()};
        }

        const bool admit_to_lru = lru_should_admit_page(
            page_id, next_lru_admission_epoch());
        if (!admit_to_lru) {
            if (!single_page_buffer_) {
                single_page_buffer_ = allocate_packed_page_buffer();
            }
            read_exact_at_fd(pages_fd_, single_page_buffer_.get(),
                             packed_bytes_for_residues(residues_in_page(page_id)),
                             page_file_offset(page_id), "failed to read page");
            ++stats_.misses;
            return PageView{nullptr, 0, residues_in_page(page_id),
                            single_page_buffer_.get()};
        }

        AlignedBuffer packed = read_packed_page_from_fd(page_id);

        ++stats_.misses;
        return insert_reserved_packed_page_into_cache(page_id, std::move(packed), false);
    }

    void evict_lru(uint64_t protected_page_id) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        evict_lru_locked(protected_page_id);
    }

    void evict_lru_locked(uint64_t protected_page_id) {
        while (lru_live_entries_ > 1 && current_cache_bytes_ > cache_capacity_bytes_) {
            uint64_t victim_slot = evict_lru_clock_slot_unlocked(protected_page_id);
            if (victim_slot == invalid_cache_slot()) break;
            free_lru_slots_.push_back(victim_slot);
        }
    }

    void clear_cache() {
        full_cache_pages_.clear();
        full_cache_pages_.shrink_to_fit();
        prefix_cache_codes_ = sdsl::int_vector<5>();
        prefix_cache_packed_.reset();
        lru_cache_packed_.reset();
        lru_cache_slot_count_ = 0;
        page_to_lru_slot_.clear();
        page_to_lru_slot_.shrink_to_fit();
        lru_slot_to_page_.clear();
        lru_slot_to_page_.shrink_to_fit();
        lru_ref_bits_.clear();
        lru_ref_bits_.shrink_to_fit();
        free_lru_slots_.clear();
        free_lru_slots_.shrink_to_fit();
        lru_recent_miss_epoch_.clear();
        lru_recent_miss_epoch_.shrink_to_fit();
        single_page_buffer_.reset();
        prefix_cache_pages_ = 0;
        prefix_cache_stride_residues_ = 0;
        prefix_cache_stride_bytes_ = 0;
        lru_clock_hand_slot_ = 0;
        lru_live_entries_ = 0;
        lru_admission_epoch_ = 0;
        current_cache_bytes_ = 0;
    }

    void close_pages_fd() {
        if (pages_fd_ >= 0) {
            ::close(pages_fd_);
            pages_fd_ = -1;
        }
    }

    static void read_exact_at_fd(int fd,
                                 void* dst,
                                 uint64_t bytes,
                                 uint64_t offset,
                                 const char* context) {
        if (fd < 0) {
            throw std::runtime_error(std::string(context) + ": pages.bin is not open");
        }
        char* out = static_cast<char*>(dst);
        uint64_t done = 0;
        while (done < bytes) {
            size_t to_read = static_cast<size_t>(std::min<uint64_t>(
                bytes - done,
                static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())));
            ssize_t n = ::pread(fd,
                                out + done,
                                to_read,
                                static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string(context) + ": " +
                                         std::strerror(errno));
            }
            if (n == 0) {
                throw std::runtime_error(std::string(context) + ": short read");
            }
            done += static_cast<uint64_t>(n);
        }
    }

    template <typename T>
    static bool read_pod(std::ifstream& in, T& value) {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return static_cast<bool>(in);
    }

    bool read_meta(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        char magic[8] = {};
        uint64_t version = 0;
        uint64_t seq_count = 0;
        uint64_t page_count = 0;

        in.read(magic, sizeof(magic));
        bool is_v4 = std::strncmp(magic, "AAPAGE4", 7) == 0;
        bool is_v3 = std::strncmp(magic, "AAPAGE3", 7) == 0;
        bool is_v2 = std::strncmp(magic, "AAPAGE2", 7) == 0;
        bool is_v1 = std::strncmp(magic, "AAPAGE1", 7) == 0;
        if (!is_v4 && !is_v3 && !is_v2 && !is_v1) return false;
        if (!read_pod(in, version)) return false;
        if ((is_v4 && version != 4) || (is_v3 && version != 3) ||
            (is_v2 && version != 2) || (is_v1 && version != 1)) return false;
        if (!read_pod(in, n_) ||
            !read_pod(in, total_length_) ||
            !read_pod(in, page_residues_) ||
            !read_pod(in, seq_count) ||
            !read_pod(in, page_count)) {
            return false;
        }

        if (seq_count != n_) return false;
        if (page_residues_ == 0) return false;
        page_count_ = page_count;
        packed_page_bytes_ = packed_bytes_for_residues(page_residues_);
        if (page_count_ != computed_page_count(total_length_, page_residues_)) return false;
        stop_aware_encoding_ = false;

        if (is_v4) {
            uint64_t encoding_flags = 0;
            if (!read_pod(in, encoding_flags)) return false;
            if ((encoding_flags & ~1ULL) != 0) return false;
            stop_aware_encoding_ = (encoding_flags & 1ULL) != 0;
        }

        if (is_v4 || is_v3) {
            start_positions_.resize(static_cast<size_t>(seq_count + 1));
            if (!start_positions_.empty()) {
                in.read(reinterpret_cast<char*>(start_positions_.data()),
                        static_cast<std::streamsize>(start_positions_.size() * sizeof(uint64_t)));
            }
            if (!in) return false;
            return validate_start_positions();
        }

        std::vector<LegacySeqMeta> legacy_meta(static_cast<size_t>(seq_count));
        if (!legacy_meta.empty()) {
            in.read(reinterpret_cast<char*>(legacy_meta.data()),
                    static_cast<std::streamsize>(legacy_meta.size() * sizeof(LegacySeqMeta)));
        }
        if (!in) return false;
        return convert_legacy_seq_meta(legacy_meta);
    }
};

#endif
