#include "aastore/store.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Arguments {
    std::filesystem::path store;
    uint64_t requests = 1000000;
    uint64_t seed = 12345;
    size_t threads = 8;
    bool drop_cache = false;
    bool adaptive_gap = false;
    bool cache_aware_adaptive = true;
    uint64_t merge_gap_bytes = aastore::kByteRangeMergeGapBytes;
    uint64_t io_latency_ns = aastore::kAdaptiveIoLatencyNs;
    uint64_t cold_io_latency_ns = aastore::kAdaptiveColdIoLatencyNs;
    uint64_t io_bandwidth_mib_per_second = 8192;
    uint64_t cold_io_bandwidth_mib_per_second = 4096;
    uint64_t max_range_mib = 1;
};

uint64_t parse_u64(const char* text, const char* name) {
    size_t consumed = 0;
    const std::string value(text);
    const uint64_t result = std::stoull(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid ") + name);
    }
    return result;
}

Arguments parse_arguments(int argc, char** argv) {
    if (argc < 2) {
        throw std::invalid_argument(
            "usage: aastore_bench STORE [--requests N] [--seed N] "
            "[--threads N] [--drop-cache] [--adaptive-gap] "
            "[--merge-gap N] [--io-latency-ns N] "
            "[--cold-io-latency-ns N] "
            "[--io-bandwidth-mib-per-second N] "
            "[--cold-io-bandwidth-mib-per-second N] "
            "[--no-cache-aware] [--max-range-mib N]");
    }
    Arguments arguments;
    arguments.store = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string option(argv[i]);
        auto next = [&](const char* name) {
            if (++i >= argc) {
                throw std::invalid_argument(
                    std::string("missing value for ") + name);
            }
            return argv[i];
        };
        if (option == "--requests") {
            arguments.requests = parse_u64(next("--requests"), "requests");
        } else if (option == "--seed") {
            arguments.seed = parse_u64(next("--seed"), "seed");
        } else if (option == "--threads") {
            arguments.threads = static_cast<size_t>(
                parse_u64(next("--threads"), "threads"));
        } else if (option == "--drop-cache") {
            arguments.drop_cache = true;
        } else if (option == "--adaptive-gap") {
            arguments.adaptive_gap = true;
        } else if (option == "--no-cache-aware") {
            arguments.cache_aware_adaptive = false;
        } else if (option == "--merge-gap") {
            arguments.merge_gap_bytes =
                parse_u64(next("--merge-gap"), "merge gap");
        } else if (option == "--io-latency-ns") {
            arguments.io_latency_ns =
                parse_u64(next("--io-latency-ns"), "I/O latency");
        } else if (option == "--cold-io-latency-ns") {
            arguments.cold_io_latency_ns = parse_u64(
                next("--cold-io-latency-ns"), "cold I/O latency");
        } else if (option == "--io-bandwidth-mib-per-second") {
            arguments.io_bandwidth_mib_per_second = parse_u64(
                next("--io-bandwidth-mib-per-second"), "I/O bandwidth");
        } else if (option == "--cold-io-bandwidth-mib-per-second") {
            arguments.cold_io_bandwidth_mib_per_second = parse_u64(
                next("--cold-io-bandwidth-mib-per-second"),
                "cold I/O bandwidth");
        } else if (option == "--max-range-mib") {
            arguments.max_range_mib =
                parse_u64(next("--max-range-mib"), "maximum range");
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    arguments.threads = std::max<size_t>(1, arguments.threads);
    return arguments;
}

uint64_t splitmix64(uint64_t& state) {
    uint64_t value = (state += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

uint64_t mib_to_bytes(uint64_t value, const char* name) {
    constexpr uint64_t kMiB = 1024ULL * 1024ULL;
    if (value > std::numeric_limits<uint64_t>::max() / kMiB) {
        throw std::overflow_error(std::string(name) + " is too large");
    }
    return value * kMiB;
}

std::vector<aastore::SequenceId> make_ids(uint64_t size,
                                          uint64_t count,
                                          uint64_t seed) {
    if (size == 0 && count != 0) throw std::runtime_error("empty store");
    if (count > static_cast<uint64_t>(
            std::numeric_limits<size_t>::max())) {
        throw std::length_error("too many requests");
    }
    std::vector<aastore::SequenceId> ids(static_cast<size_t>(count));
    uint64_t state = seed;
    for (aastore::SequenceId& id : ids) id = splitmix64(state) % size;
    return ids;
}

void drop_file_cache(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::runtime_error("cannot open cache target " + path.string());
    }
#ifdef POSIX_FADV_DONTNEED
    const int result = ::posix_fadvise(descriptor, 0, 0,
                                       POSIX_FADV_DONTNEED);
    ::close(descriptor);
    if (result != 0) {
        throw std::runtime_error("cannot drop cache for " + path.string() +
                                 ": " + std::strerror(result));
    }
#else
    ::close(descriptor);
#endif
}

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        Clock::now() - start).count();
}

uint64_t checksum(const aastore::Batch& batch, size_t thread_count) {
    const size_t size = batch.storage.size();
    if (size == 0) return 0;
    thread_count = std::max<size_t>(1, std::min(thread_count, size));
    std::vector<uint64_t> partial(thread_count, 0);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    for (size_t worker = 0; worker < thread_count; ++worker) {
        const size_t begin = size * worker / thread_count;
        const size_t end = size * (worker + 1) / thread_count;
        workers.emplace_back([&, worker, begin, end]() {
            uint64_t sum = 0;
            const char* data = batch.storage.data();
            for (size_t i = begin; i < end; ++i) {
                sum += static_cast<unsigned char>(data[i]);
            }
            partial[worker] = sum;
        });
    }
    for (std::thread& worker : workers) worker.join();
    uint64_t result = 0;
    for (uint64_t value : partial) result += value;
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        if (arguments.drop_cache) {
            drop_file_cache(arguments.store / "pages.bin");
            drop_file_cache(arguments.store / "meta.bin");
            drop_file_cache(arguments.store / "meta.compact_u32.bin");
            // DONTNEED is advisory and eviction may complete asynchronously
            // on shared filesystems. Keep the wait outside measured time.
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        const auto total_start = Clock::now();
        const auto open_start = Clock::now();
        aastore::Store store(arguments.store);
        const double open_ms = elapsed_ms(open_start);
        const std::vector<aastore::SequenceId> ids = make_ids(
            store.size(), arguments.requests, arguments.seed);
        aastore::ReadOptions options;
        options.io_threads = arguments.threads;
        options.adaptive_merge_gap = arguments.adaptive_gap;
        options.adaptive.cache_aware = arguments.cache_aware_adaptive;
        options.merge_gap_bytes = arguments.merge_gap_bytes;
        options.adaptive.io_latency_ns = arguments.io_latency_ns;
        options.adaptive.cold_io_latency_ns = arguments.cold_io_latency_ns;
        options.adaptive.io_bandwidth_bytes_per_second = mib_to_bytes(
            arguments.io_bandwidth_mib_per_second, "I/O bandwidth");
        options.adaptive.cold_io_bandwidth_bytes_per_second = mib_to_bytes(
            arguments.cold_io_bandwidth_mib_per_second,
            "cold I/O bandwidth");
        options.adaptive.max_merged_range_bytes = mib_to_bytes(
            arguments.max_range_mib, "maximum range");
        const auto fetch_start = Clock::now();
        const aastore::Batch batch = store.batch_get(ids, options);
        const double fetch_ms = elapsed_ms(fetch_start);
        const auto consume_start = Clock::now();
        const uint64_t sum = checksum(batch, arguments.threads);
        const double consume_ms = elapsed_ms(consume_start);

        struct rusage usage {};
        ::getrusage(RUSAGE_SELF, &usage);
        std::cout << "store_size=" << store.size() << '\n'
                  << "requests=" << ids.size() << '\n'
                  << "residues=" << batch.stats.requested_residues << '\n'
                  << "checksum=" << sum << '\n'
                  << "threads=" << arguments.threads << '\n'
                  << "decode_kernel=" << store.decode_kernel_name() << '\n'
                  << "merge_policy="
                  << (arguments.adaptive_gap ? "adaptive" : "fixed") << '\n'
                  << "configured_merge_gap_bytes="
                  << arguments.merge_gap_bytes << '\n'
                  << "selected_merge_gap_bytes="
                  << batch.stats.selected_merge_gap_bytes << '\n'
                  << "io_latency_ns=" << arguments.io_latency_ns << '\n'
                  << "cold_io_latency_ns="
                  << arguments.cold_io_latency_ns << '\n'
                  << "io_bandwidth_mib_per_second="
                  << arguments.io_bandwidth_mib_per_second << '\n'
                  << "cold_io_bandwidth_mib_per_second="
                  << arguments.cold_io_bandwidth_mib_per_second << '\n'
                  << "cache_aware_adaptive="
                  << arguments.cache_aware_adaptive << '\n'
                  << "modeled_io_bandwidth_mib_per_second="
                  << batch.stats.modeled_io_bandwidth_bytes_per_second /
                         (1024.0 * 1024.0) << '\n'
                  << "modeled_io_latency_ns="
                  << batch.stats.modeled_io_latency_ns << '\n'
                  << "page_cache_resident_percent="
                  << (batch.stats.page_cache_residency_known &&
                              batch.stats.page_cache_total_bytes != 0
                          ? 100.0 * batch.stats.page_cache_resident_bytes /
                                batch.stats.page_cache_total_bytes
                          : -1.0) << '\n'
                  << "max_range_mib=" << arguments.max_range_mib << '\n'
                  << "merge_candidates="
                  << batch.stats.merge_candidates << '\n'
                  << "ranges=" << batch.stats.ranges << '\n'
                  << "max_range_bytes=" << batch.stats.max_range_bytes << '\n'
                  << "read_bytes=" << batch.stats.read_bytes << '\n'
                  << "unique_packed_bytes="
                  << batch.stats.unique_packed_bytes << '\n'
                  << "overread_bytes=" << batch.stats.overread_bytes << '\n'
                  << "estimated_io_ms="
                  << batch.stats.estimated_io_ns / 1.0e6 << '\n'
                  << "open_ms=" << open_ms << '\n'
                  << "fetch_ms=" << fetch_ms << '\n'
                  << "consume_ms=" << consume_ms << '\n'
                  << "end_to_end_ms=" << elapsed_ms(total_start) << '\n'
                  << "layout_ms=" << batch.stats.layout_ns / 1.0e6 << '\n'
                  << "sort_ms=" << batch.stats.sort_ns / 1.0e6 << '\n'
                  << "range_plan_ms="
                  << batch.stats.range_plan_ns / 1.0e6 << '\n'
                  << "worker_wall_ms="
                  << batch.stats.worker_wall_ns / 1.0e6 << '\n'
                  << "worker_max_busy_ms="
                  << batch.stats.worker_max_busy_ns / 1.0e6 << '\n'
                  << "worker_read_ms=" << batch.stats.read_ns / 1.0e6 << '\n'
                  << "worker_decode_ms="
                  << batch.stats.decode_ns / 1.0e6 << '\n'
                  << "peak_rss_kib=" << usage.ru_maxrss << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ERROR: " << error.what() << '\n';
        return 2;
    }
}
