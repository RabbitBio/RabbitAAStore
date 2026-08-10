#include "aastore/store.hpp"

#include "byte_range_planner.hpp"
#include "codec.hpp"
#include "metadata.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>

namespace aastore {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t elapsed_ns(Clock::time_point start) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start).count());
}

void pread_exact(int descriptor,
                 void* output,
                 uint64_t bytes,
                 uint64_t offset) {
    char* destination = static_cast<char*>(output);
    uint64_t completed = 0;
    while (completed < bytes) {
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(
            bytes - completed,
            static_cast<uint64_t>(std::numeric_limits<ssize_t>::max())));
        const ssize_t result = ::pread(
            descriptor, destination + completed, chunk,
            static_cast<off_t>(offset + completed));
        if (result < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "pread pages.bin failed: " +
                std::string(std::strerror(errno)));
        }
        if (result == 0) throw std::runtime_error("short read from pages.bin");
        completed += static_cast<uint64_t>(result);
    }
}

void pread_exact_parallel(int descriptor,
                          void* output,
                          uint64_t bytes,
                          uint64_t offset,
                          size_t thread_count) {
    constexpr uint64_t kMinBytesPerThread = 64ULL * 1024ULL * 1024ULL;
    if (thread_count <= 1 || bytes < 2 * kMinBytesPerThread) {
        pread_exact(descriptor, output, bytes, offset);
        return;
    }
    const size_t workers = std::min<size_t>(
        thread_count,
        static_cast<size_t>(bytes / kMinBytesPerThread));
    if (workers <= 1) {
        pread_exact(descriptor, output, bytes, offset);
        return;
    }

    std::vector<std::thread> threads;
    std::vector<std::exception_ptr> errors(workers);
    threads.reserve(workers - 1);
    auto run = [&](size_t worker) {
        try {
            const uint64_t begin = bytes * worker / workers;
            const uint64_t end = bytes * (worker + 1) / workers;
            pread_exact(descriptor, static_cast<char*>(output) + begin,
                        end - begin, offset + begin);
        } catch (...) {
            errors[worker] = std::current_exception();
        }
    };
    for (size_t worker = 1; worker < workers; ++worker) {
        threads.emplace_back(run, worker);
    }
    run(0);
    for (std::thread& thread : threads) thread.join();
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

uint64_t file_size(int descriptor) {
    const off_t end = ::lseek(descriptor, 0, SEEK_END);
    if (end < 0) throw std::runtime_error("cannot size pages.bin");
    return static_cast<uint64_t>(end);
}

}  // namespace

void ByteArena::resize_uninitialized(size_t size) {
    data_.reset(size == 0 ? nullptr : new char[size]);
    size_ = size;
}

std::string_view Batch::view(size_t index) const {
    if (index >= lengths.size() || index >= offsets.size()) {
        throw std::out_of_range("batch sequence index out of range");
    }
    const uint64_t offset = offsets[index];
    const uint64_t length = lengths[index];
    if (offset > storage.size() || length > storage.size() - offset) {
        throw std::runtime_error("corrupt batch arena");
    }
    return std::string_view(storage.data() + offset,
                            static_cast<size_t>(length));
}

class Store::Impl {
public:
    explicit Impl(const std::filesystem::path& path)
        : metadata_(path) {
        const std::filesystem::path pages_path = path / "pages.bin";
        pages_descriptor_ = ::open(pages_path.c_str(), O_RDONLY | O_CLOEXEC);
        if (pages_descriptor_ < 0) {
            throw std::runtime_error("cannot open " + pages_path.string() +
                                     ": " + std::strerror(errno));
        }
        try {
            if (file_size(pages_descriptor_) < metadata_.expected_pages_bytes()) {
                throw std::runtime_error("pages.bin is shorter than metadata");
            }
        } catch (...) {
            ::close(pages_descriptor_);
            pages_descriptor_ = -1;
            throw;
        }
    }

    ~Impl() {
        if (pages_descriptor_ >= 0) ::close(pages_descriptor_);
    }

    uint64_t size() const noexcept { return metadata_.sequence_count(); }
    uint64_t total_residues() const noexcept {
        return metadata_.total_residues();
    }
    uint64_t packed_bytes() const noexcept {
        return metadata_.expected_pages_bytes();
    }

    uint64_t sequence_length(SequenceId id) const {
        if (id >= size()) throw std::out_of_range("sequence id out of range");
        return metadata_.start_position(id + 1) -
               metadata_.start_position(id);
    }

    Batch batch_get(const std::vector<SequenceId>& ids,
                    const ReadOptions& options) const {
        const size_t io_threads = std::max<size_t>(1, options.io_threads);
        Batch batch;
        batch.offsets.resize(ids.size());
        batch.lengths.resize(ids.size());
        batch.stats.requested_sequences = ids.size();

        const auto layout_start = Clock::now();
        detail::SequenceRequests requests(ids.size());
        const size_t prepare_workers = ids.size() >= 65536
            ? std::min(io_threads, ids.size())
            : size_t{1};
        std::vector<uint64_t> chunk_totals(prepare_workers, 0);
        std::vector<std::exception_ptr> prepare_errors(prepare_workers);
        auto run_prepare = [&](auto&& function) {
            std::vector<std::thread> threads;
            threads.reserve(prepare_workers - 1);
            for (size_t worker = 1; worker < prepare_workers; ++worker) {
                threads.emplace_back(function, worker);
            }
            function(size_t{0});
            for (std::thread& thread : threads) thread.join();
        };
        run_prepare([&](size_t worker) {
            try {
                const size_t begin = ids.size() * worker / prepare_workers;
                const size_t end = ids.size() * (worker + 1) / prepare_workers;
                uint64_t local_total = 0;
                for (size_t i = begin; i < end; ++i) {
                    const SequenceId id = ids[i];
                    if (id >= size()) {
                        throw std::out_of_range("sequence id out of range");
                    }
                    const uint64_t start = metadata_.start_position(id);
                    const uint64_t length =
                        metadata_.start_position(id + 1) - start;
                    if (local_total >
                        std::numeric_limits<uint64_t>::max() - length - 1) {
                        throw std::length_error("batch output arena overflow");
                    }
                    batch.lengths[i] = length;
                    requests[i] = detail::SequenceRequest{id, i, start, length};
                    local_total += length + 1;
                }
                chunk_totals[worker] = local_total;
            } catch (...) {
                prepare_errors[worker] = std::current_exception();
            }
        });
        for (const std::exception_ptr& error : prepare_errors) {
            if (error) std::rethrow_exception(error);
        }
        uint64_t arena_bytes = 0;
        for (uint64_t chunk_total : chunk_totals) {
            if (arena_bytes > std::numeric_limits<uint64_t>::max() -
                                  chunk_total) {
                throw std::length_error("batch output arena overflow");
            }
            arena_bytes += chunk_total;
        }
        if (arena_bytes > static_cast<uint64_t>(
                std::numeric_limits<size_t>::max())) {
            throw std::length_error("batch output arena exceeds address space");
        }

        const auto sort_start = Clock::now();
        if (!options.input_sorted) {
            detail::sort_requests_by_sequence_id(requests, io_threads);
        }
        batch.stats.sort_ns = elapsed_ns(sort_start);

        // Keep decoded writes sequential in physical sequence order. offsets
        // maps each original request back to its physical arena position.
        std::vector<uint64_t> physical_totals(prepare_workers, 0);
        run_prepare([&](size_t worker) {
            const size_t begin = requests.size() * worker / prepare_workers;
            const size_t end = requests.size() * (worker + 1) / prepare_workers;
            uint64_t total = 0;
            for (size_t i = begin; i < end; ++i) {
                total += requests[i].length + 1;
            }
            physical_totals[worker] = total;
        });
        std::vector<uint64_t> physical_bases(prepare_workers, 0);
        uint64_t cursor = 0;
        for (size_t worker = 0; worker < prepare_workers; ++worker) {
            physical_bases[worker] = cursor;
            cursor += physical_totals[worker];
        }
        if (cursor != arena_bytes) {
            throw std::runtime_error("physical output layout size mismatch");
        }
        batch.storage.resize_uninitialized(static_cast<size_t>(arena_bytes));
        run_prepare([&](size_t worker) {
            const size_t begin = requests.size() * worker / prepare_workers;
            const size_t end = requests.size() * (worker + 1) / prepare_workers;
            uint64_t output_offset = physical_bases[worker];
            for (size_t i = begin; i < end; ++i) {
                const detail::SequenceRequest& request = requests[i];
                batch.offsets[request.output_index] = output_offset;
                if (request.length == 0) {
                    batch.storage[static_cast<size_t>(output_offset)] = '\0';
                }
                output_offset += request.length + 1;
            }
        });
        batch.stats.layout_ns = elapsed_ns(layout_start) - batch.stats.sort_ns;

        const auto plan_start = Clock::now();
        const detail::ReadPlan plan = detail::plan_byte_ranges(
            requests, metadata_.page_residues(),
            metadata_.packed_page_bytes());
        batch.stats.range_plan_ns = elapsed_ns(plan_start);
        batch.stats.requested_residues = plan.requested_residues;
        batch.stats.unique_packed_bytes = plan.unique_packed_bytes;
        batch.stats.ranges = plan.ranges.size();
        batch.stats.merged_gaps = plan.merged_gaps;
        batch.stats.read_bytes = plan.read_bytes;
        batch.stats.overread_bytes =
            plan.read_bytes - plan.unique_packed_bytes;

        const bool parallel_single_range = plan.ranges.size() == 1 &&
            io_threads > 1 && requests.size() >= 65536;
        const size_t worker_count = plan.ranges.empty()
            ? 0
            : std::min(io_threads, plan.ranges.size());
        batch.stats.workers = parallel_single_range
            ? io_threads
            : worker_count;
        if (worker_count == 0) return batch;

        struct WorkerStats {
            uint64_t read_ns = 0;
            uint64_t decode_ns = 0;
        };
        std::vector<WorkerStats> worker_stats(worker_count);
        std::vector<std::exception_ptr> errors(worker_count);
        std::atomic<size_t> next_range{0};

        auto run_worker = [&](size_t worker) {
            try {
                std::unique_ptr<uint8_t[]> buffer;
                size_t buffer_capacity = 0;
                while (true) {
                    const size_t range_index =
                        next_range.fetch_add(1, std::memory_order_relaxed);
                    if (range_index >= plan.ranges.size()) break;
                    const detail::ByteRange& range = plan.ranges[range_index];
                    const uint64_t range_bytes = range.end - range.begin;
                    if (range_bytes > static_cast<uint64_t>(
                            std::numeric_limits<size_t>::max())) {
                        throw std::length_error(
                            "packed byte range exceeds address space");
                    }
                    const size_t needed = static_cast<size_t>(range_bytes);
                    if (needed > buffer_capacity) {
                        buffer = std::make_unique<uint8_t[]>(needed);
                        buffer_capacity = needed;
                    }

                    const auto read_start = Clock::now();
                    if (parallel_single_range) {
                        pread_exact_parallel(
                            pages_descriptor_, buffer.get(), range_bytes,
                            range.begin, io_threads);
                    } else {
                        pread_exact(pages_descriptor_, buffer.get(),
                                    range_bytes, range.begin);
                    }
                    worker_stats[worker].read_ns += elapsed_ns(read_start);

                    auto decode_request = [&](size_t request_index) {
                        const detail::SequenceRequest& request =
                            requests[request_index];
                        if (request.length == 0) return;
                        char* output = batch.storage.data() +
                            batch.offsets[request.output_index];
                        uint64_t remaining = request.length;
                        uint64_t source = request.start;
                        uint64_t output_offset = 0;
                        while (remaining != 0) {
                            const uint64_t page_id =
                                source / metadata_.page_residues();
                            const uint64_t page_offset =
                                source % metadata_.page_residues();
                            const uint64_t page_start =
                                page_id * metadata_.page_residues();
                            const uint64_t page_size = std::min(
                                metadata_.page_residues(),
                                metadata_.total_residues() - page_start);
                            const uint64_t take = std::min(
                                remaining, page_size - page_offset);
                            const uint64_t aligned_residue =
                                page_offset & ~uint64_t{7};
                            const uint64_t decode_base =
                                page_id * metadata_.packed_page_bytes() +
                                (aligned_residue / 8) * 5;
                            if (decode_base < range.begin ||
                                decode_base >= range.end) {
                                throw std::runtime_error(
                                    "decode extent escaped merged range");
                            }
                            detail::decode_packed_5bit_range(
                                buffer.get() + static_cast<size_t>(
                                    decode_base - range.begin),
                                page_offset - aligned_residue, take,
                                output + output_offset);
                            source += take;
                            output_offset += take;
                            remaining -= take;
                        }
                        output[request.length] = '\0';
                    };

                    const auto decode_start = Clock::now();
                    if (parallel_single_range) {
                        constexpr size_t kRequestGrain = 4096;
                        std::atomic<size_t> next_request{range.request_begin};
                        std::vector<std::thread> decode_threads;
                        std::vector<std::exception_ptr> decode_errors(io_threads);
                        decode_threads.reserve(io_threads - 1);
                        auto run_decode = [&](size_t decode_worker) {
                            try {
                                while (true) {
                                    const size_t begin = next_request.fetch_add(
                                        kRequestGrain,
                                        std::memory_order_relaxed);
                                    if (begin >= range.request_end) break;
                                    const size_t end = std::min(
                                        range.request_end,
                                        begin + kRequestGrain);
                                    for (size_t i = begin; i < end; ++i) {
                                        decode_request(i);
                                    }
                                }
                            } catch (...) {
                                decode_errors[decode_worker] =
                                    std::current_exception();
                            }
                        };
                        for (size_t decode_worker = 1;
                             decode_worker < io_threads; ++decode_worker) {
                            decode_threads.emplace_back(run_decode,
                                                        decode_worker);
                        }
                        run_decode(0);
                        for (std::thread& thread : decode_threads) thread.join();
                        for (const std::exception_ptr& error : decode_errors) {
                            if (error) std::rethrow_exception(error);
                        }
                    } else {
                        for (size_t i = range.request_begin;
                             i < range.request_end; ++i) {
                            decode_request(i);
                        }
                    }
                    worker_stats[worker].decode_ns += elapsed_ns(decode_start);
                }
            } catch (...) {
                errors[worker] = std::current_exception();
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(worker_count - 1);
        for (size_t worker = 1; worker < worker_count; ++worker) {
            workers.emplace_back(run_worker, worker);
        }
        run_worker(0);
        for (std::thread& worker : workers) worker.join();
        for (const std::exception_ptr& error : errors) {
            if (error) std::rethrow_exception(error);
        }
        for (const WorkerStats& stats : worker_stats) {
            batch.stats.read_ns += stats.read_ns;
            batch.stats.decode_ns += stats.decode_ns;
        }
        return batch;
    }

private:
    detail::Metadata metadata_;
    int pages_descriptor_ = -1;
};

Store::Store(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>(path)) {}
Store::~Store() = default;
Store::Store(Store&&) noexcept = default;
Store& Store::operator=(Store&&) noexcept = default;

uint64_t Store::size() const noexcept { return impl_->size(); }
uint64_t Store::total_residues() const noexcept {
    return impl_->total_residues();
}
uint64_t Store::packed_bytes() const noexcept { return impl_->packed_bytes(); }
uint64_t Store::sequence_length(SequenceId id) const {
    return impl_->sequence_length(id);
}
const char* Store::decode_kernel_name() const noexcept {
    return detail::packed_5bit_decode_kernel_name();
}

std::string Store::get(SequenceId id) const {
    Batch batch = batch_get({id});
    return std::string(batch.view(0));
}

Batch Store::batch_get(const std::vector<SequenceId>& ids,
                       const ReadOptions& options) const {
    return impl_->batch_get(ids, options);
}

}  // namespace aastore
