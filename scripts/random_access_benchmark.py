#!/usr/bin/env python3
"""Benchmark random access on ProteinAAStore, paged AAStore, or pyfastx.

This script compiles a tiny C++ runner that directly calls ProteinAAStore::get()
or ProteinAAStore::checksum() for N randomly sampled sequence ids. Store loading
and random id generation are reported separately and are not included in the
access timing.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


CPP_SOURCE = r'''
#include "ProteinAAStore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0] << " <store.bin> <n> <seed> <mode>\n";
        return 2;
    }

    const std::string store_path = argv[1];
    const uint64_t sample_count = std::stoull(argv[2]);
    const uint64_t seed = std::stoull(argv[3]);
    const std::string mode = argv[4];

    ProteinAAStore store;
    auto load_start = std::chrono::high_resolution_clock::now();
    if (!store.load(store_path)) {
        std::cerr << "Error: failed to load " << store_path << "\n";
        return 1;
    }
    auto load_end = std::chrono::high_resolution_clock::now();

    const size_t store_size = store.size();
    if (store_size == 0) {
        std::cerr << "Error: empty store\n";
        return 1;
    }

    uint64_t rng_state = seed;
    std::vector<size_t> ids;
    ids.reserve(sample_count);
    for (uint64_t i = 0; i < sample_count; ++i) {
        ids.push_back(static_cast<size_t>(splitmix64(rng_state) % store_size));
    }

    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        load_end - load_start).count();

    std::cout << "store_size=" << store_size << "\n";
    std::cout << "total_length=" << store.total_length() << "\n";
    std::cout << "samples=" << sample_count << "\n";
    std::cout << "seed=" << seed << "\n";
    std::cout << "load_ms=" << load_ms << "\n";
    std::cout << "order=random_no_sort\n";

    if (mode == "checksum" || mode == "both") {
        uint64_t checksum_enc = 0;
        uint64_t total_chars = 0;
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t idx : ids) {
            checksum_enc += store.checksum(idx);
            total_chars += store.length(idx);
        }
        auto end = std::chrono::high_resolution_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        const double mbps = (total_chars / (1024.0 * 1024.0)) / (seconds + 1e-12);
        std::cout << "checksum_only_ms=" << seconds * 1000.0 << "\n";
        std::cout << "checksum_only_chars=" << total_chars << "\n";
        std::cout << "checksum_enc=" << checksum_enc << "\n";
        std::cout << "checksum_only_mbps=" << mbps << "\n";
    }

    if (mode == "get" || mode == "both") {
        uint64_t checksum_dec = 0;
        uint64_t total_chars = 0;
        auto start = std::chrono::high_resolution_clock::now();
        for (size_t idx : ids) {
            std::string seq = store.get(idx);
            total_chars += seq.size();
            for (unsigned char c : seq) {
                checksum_dec += c;
            }
        }
        auto end = std::chrono::high_resolution_clock::now();
        const double seconds = std::chrono::duration<double>(end - start).count();
        const double mbps = (total_chars / (1024.0 * 1024.0)) / (seconds + 1e-12);
        std::cout << "real_get_ms=" << seconds * 1000.0 << "\n";
        std::cout << "real_get_chars=" << total_chars << "\n";
        std::cout << "checksum_dec=" << checksum_dec << "\n";
        std::cout << "real_get_mbps=" << mbps << "\n";
    }

    return 0;
}
'''


PAGED_CPP_SOURCE = r'''
#include "PagedProteinAAStore.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static uint64_t splitmix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
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

int main(int argc, char** argv) {
    if (argc < 19) {
        std::cerr << "Usage: " << argv[0] << " <paged_store_dir> <n> <seed> <memory_mb> <mode> <preload> <preload_threads> <prefetch_mb> <prefetch_threads> <prefetch_depth> <single_thread_aio> <single_thread_aio_completions> <aio_completion_min> <drop_page_cache> <io_backend> <uring_sqpoll> <no_preload_cache> <cache_policy>\n";
        return 2;
    }

    const std::string store_dir = argv[1];
    const uint64_t sample_count = std::stoull(argv[2]);
    const uint64_t seed = std::stoull(argv[3]);
    const uint64_t memory_mb = std::stoull(argv[4]);
    const std::string mode = argv[5];
    const bool preload = (std::string(argv[6]) == "1");
    const uint64_t preload_threads = std::stoull(argv[7]);
    const uint64_t prefetch_mb = std::stoull(argv[8]);
    const uint64_t prefetch_threads = std::stoull(argv[9]);
    const uint64_t prefetch_depth = std::stoull(argv[10]);
    const bool single_thread_aio = (std::string(argv[11]) == "1") || prefetch_threads == 0;
    const uint64_t effective_prefetch_threads = single_thread_aio ? 1 : prefetch_threads;
    const bool caller_only_io = effective_prefetch_threads <= 1;
    const uint64_t single_thread_aio_completions = std::stoull(argv[12]);
    const uint64_t aio_completion_min = std::stoull(argv[13]);
    const bool drop_page_cache = (std::string(argv[14]) == "1");
    const std::string io_backend = argv[15];
    const bool uring_sqpoll = (std::string(argv[16]) == "1");
    const bool no_preload_cache = (std::string(argv[17]) == "1");
    const std::string cache_policy = argv[18];
    const bool batch = (mode == "batch");

    PagedProteinAAStore store;
    auto load_start = std::chrono::high_resolution_clock::now();
    const uint64_t memory_bytes = memory_mb == 0 ? 0 : memory_mb * 1024ULL * 1024ULL;
    const uint64_t prefetch_bytes = prefetch_mb * 1024ULL * 1024ULL;
    if (!store.open(store_dir, memory_bytes)) {
        std::cerr << "Error: failed to open " << store_dir << "\n";
        return 1;
    }
    store.set_prefetch_depth(static_cast<size_t>(prefetch_depth));
    store.set_prefetch_io_backend(io_backend);
    store.set_uring_sqpoll(uring_sqpoll);
    store.set_resident_cache_policy(cache_policy);
    store.set_io_threads(static_cast<size_t>(effective_prefetch_threads));
    store.set_prefetch_memory_bytes(prefetch_bytes);
    store.set_single_thread_aio(caller_only_io);
    store.set_single_thread_aio_completion_batch(static_cast<size_t>(single_thread_aio_completions));
    store.set_aio_completion_min(static_cast<size_t>(aio_completion_min));
    auto load_end = std::chrono::high_resolution_clock::now();
    PagedProteinAAStore::MemoryBreakdown open_mem = store.memory_breakdown();
    const bool full_cache_fits = memory_mb == 0 ||
                                 (open_mem.disk_pages_bytes > 0 &&
                                  memory_bytes >= open_mem.disk_pages_bytes);
    const bool auto_full_cache = !no_preload_cache && full_cache_fits;
    const bool use_batch_access = batch || prefetch_bytes > 0;

    const size_t store_size = store.size();
    if (store_size == 0) {
        std::cerr << "Error: empty store\n";
        return 1;
    }

    uint64_t rng_state = seed;
    std::vector<size_t> ids;
    ids.reserve(sample_count);
    for (uint64_t i = 0; i < sample_count; ++i) {
        ids.push_back(static_cast<size_t>(splitmix64(rng_state) % store_size));
    }

    uint64_t preload_pages = 0;
    uint64_t preload_bytes = 0;
    auto preload_start = std::chrono::high_resolution_clock::now();
    const bool auto_preload_cache = !no_preload_cache &&
                                    (preload || auto_full_cache || memory_mb > 0);
    const size_t effective_preload_threads = preload_threads == 0
        ? PagedProteinAAStore::automatic_preload_thread_count()
        : static_cast<size_t>(preload_threads);
    if (auto_preload_cache) {
        const uint64_t preload_limit = memory_mb != 0 ? memory_bytes : 0;
        preload_pages = store.preload_cache_parallel(preload_limit,
                                                     effective_preload_threads);
        preload_bytes = store.cache_bytes();
        store.reset_stats();
    }
    auto preload_end = std::chrono::high_resolution_clock::now();

    auto drop_start = std::chrono::high_resolution_clock::now();
    if (drop_page_cache) {
        store.drop_pages_file_cache();
    }
    auto drop_end = std::chrono::high_resolution_clock::now();

    store.reset_stats();
    uint64_t checksum_dec = 0;
    uint64_t total_chars = 0;
    std::chrono::nanoseconds store_get_ns{0};
    std::chrono::nanoseconds checksum_ns{0};
    auto start = std::chrono::high_resolution_clock::now();
    if (use_batch_access) {
        auto get_start = std::chrono::high_resolution_clock::now();
        std::vector<std::string> seqs = store.batch_get(ids);
        auto get_end = std::chrono::high_resolution_clock::now();
        store_get_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            get_end - get_start);
        auto checksum_start = std::chrono::high_resolution_clock::now();
        checksum_dec = checksum_strings(seqs, total_chars);
        checksum_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now() - checksum_start);
    } else {
        for (size_t idx : ids) {
            std::string seq = store.get(idx);
            total_chars += seq.size();
            for (unsigned char c : seq) checksum_dec += c;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();

    const auto load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        load_end - load_start).count();
    const auto preload_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        preload_end - preload_start).count();
    const auto drop_page_cache_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        drop_end - drop_start).count();
    const double seconds = std::chrono::duration<double>(end - start).count();
    const double store_seconds = store_get_ns.count() > 0
        ? store_get_ns.count() / 1000000000.0
        : seconds;
    const double real_get_mbps = (total_chars / (1024.0 * 1024.0)) / (store_seconds + 1e-12);
    const double end_to_end_mbps = (total_chars / (1024.0 * 1024.0)) / (seconds + 1e-12);
    PagedProteinAAStore::CacheStats stats = store.stats();
    PagedProteinAAStore::MemoryBreakdown mem = store.memory_breakdown();
    uint64_t prefetch_window_est_bytes = 0;
    if (use_batch_access && prefetch_mb > 0) {
        prefetch_window_est_bytes = stats.prefetch_active_peak_bytes;
    }
    std::string access_order = "random_no_sort";
    std::string strategy = "random_get";
    if (auto_full_cache) {
        access_order = "full_cache_random_no_sort";
        strategy = "full_raw_cache";
    } else if (use_batch_access) {
        access_order = preload_pages >= store.page_count()
            ? "batch_full_cache_no_sort"
            : "batch_page_sorted";
        strategy = prefetch_mb > 0 ? "batch_prefetch" : "batch";
    }

    std::cout << "backend=paged_aastore\n";
    std::cout << "strategy=" << strategy << "\n";
    std::cout << "store_size=" << store_size << "\n";
    std::cout << "total_length=" << store.total_length() << "\n";
    std::cout << "page_residues=" << store.page_residues() << "\n";
    std::cout << "page_count=" << store.page_count() << "\n";
    std::cout << "memory_mb=" << memory_mb << "\n";
    std::cout << "cache_policy=" << store.resident_cache_policy_name() << "\n";
    std::cout << "samples=" << sample_count << "\n";
    std::cout << "seed=" << seed << "\n";
    std::cout << "load_ms=" << load_ms << "\n";
    std::cout << "preload_ms=" << preload_ms << "\n";
    std::cout << "preload_pages=" << preload_pages << "\n";
    std::cout << "preload_bytes=" << preload_bytes << "\n";
    std::cout << "preload_threads=" << (auto_preload_cache ? effective_preload_threads : 0) << "\n";
    std::cout << "drop_page_cache=" << (drop_page_cache ? 1 : 0) << "\n";
    std::cout << "drop_page_cache_ms=" << drop_page_cache_ms << "\n";
    std::cout << "auto_full_cache=" << (auto_full_cache ? 1 : 0) << "\n";
    std::cout << "auto_preload_cache=" << (auto_preload_cache ? 1 : 0) << "\n";
    std::cout << "no_preload_cache=" << (no_preload_cache ? 1 : 0) << "\n";
    std::cout << "prefetch_mb=" << prefetch_mb << "\n";
    std::cout << "prefetch_effective_mb=" << (store.prefetch_memory_bytes() / (1024ULL * 1024ULL)) << "\n";
    std::cout << "prefetch_threads=" << (use_batch_access && prefetch_mb > 0 ? store.io_threads() : 0) << "\n";
    std::cout << "prefetch_depth=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_depth : 1) << "\n";
    std::cout << "prefetch_io=" << PagedProteinAAStore::prefetch_io_backend_name(
        use_batch_access && prefetch_mb > 0
            ? stats.prefetch_io_backend
            : static_cast<uint64_t>(PagedProteinAAStore::PrefetchIoBackend::Pread)) << "\n";
    std::cout << "uring_sqpoll=" << stats.uring_sqpoll << "\n";
    std::cout << "prefetch_completion_batch=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_completion_batch : 0) << "\n";
    std::cout << "prefetch_completion_min=" << (use_batch_access && prefetch_mb > 0 ? stats.prefetch_completion_min : 0) << "\n";
    std::cout << "single_thread_aio=" << (use_batch_access && prefetch_mb > 0 && caller_only_io ? 1 : 0) << "\n";
    std::cout << "order=" << access_order << "\n";
    std::cout << "split_timing=" << (use_batch_access ? 1 : 0) << "\n";
    std::cout << "real_get_ms=" << seconds * 1000.0 << "\n";
    std::cout << "store_get_ms=" << (store_get_ns.count() / 1000000.0) << "\n";
    std::cout << "checksum_ms=" << (checksum_ns.count() / 1000000.0) << "\n";
    std::cout << "real_get_chars=" << total_chars << "\n";
    std::cout << "checksum_dec=" << checksum_dec << "\n";
    std::cout << "real_get_mbps=" << real_get_mbps << "\n";
    std::cout << "end_to_end_mbps=" << end_to_end_mbps << "\n";
    std::cout << "cache_hits=" << stats.hits << "\n";
    std::cout << "cache_misses=" << stats.misses << "\n";
    std::cout << "cache_evictions=" << stats.evictions << "\n";
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
    std::cout << "memory_prefetch_window_est_bytes=" << prefetch_window_est_bytes << "\n";
    std::cout << "memory_path_string_bytes=" << mem.path_string_bytes << "\n";
    std::cout << "memory_total_resident_est_bytes=" << mem.total_resident_est_bytes << "\n";
    std::cout << "disk_meta_bytes=" << mem.disk_meta_bytes << "\n";
    std::cout << "disk_pages_bytes=" << mem.disk_pages_bytes << "\n";
    return 0;
}
'''


PYFASTX_SOURCE = r'''
import sys
import time

import pyfastx

MASK = (1 << 64) - 1


def splitmix64(state):
    state = (state + 0x9E3779B97F4A7C15) & MASK
    z = state
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
    return state, (z ^ (z >> 31)) & MASK


if len(sys.argv) < 5:
    print("Usage: pyfastx_runner <fasta> <n> <seed> <access>", file=sys.stderr)
    raise SystemExit(2)

fasta_path = sys.argv[1]
sample_count = int(sys.argv[2])
seed = int(sys.argv[3])
access = sys.argv[4]

load_start = time.perf_counter()
fa = pyfastx.Fasta(fasta_path)
load_end = time.perf_counter()

store_size = len(fa)
if store_size == 0:
    print("Error: empty FASTA", file=sys.stderr)
    raise SystemExit(1)

rng_state = seed
ids = []
for _ in range(sample_count):
    rng_state, value = splitmix64(rng_state)
    ids.append(value % store_size)

keys = None
if access == "name":
    keys = fa.keys()
elif access != "index":
    print(f"Error: invalid pyfastx access mode: {access}", file=sys.stderr)
    raise SystemExit(2)

print("backend=pyfastx")
print(f"store_size={store_size}")
try:
    print(f"total_length={fa.size}")
except Exception:
    pass
print(f"samples={sample_count}")
print(f"seed={seed}")
print(f"load_ms={(load_end - load_start) * 1000.0}")
print("order=random_no_sort")
print(f"pyfastx_access={access}")

checksum_dec = 0
total_chars = 0
start = time.perf_counter()
if access == "name":
    for idx in ids:
        seq = fa[keys[idx]].seq
        total_chars += len(seq)
        for c in seq:
            checksum_dec += ord(c)
else:
    for idx in ids:
        seq = fa[idx].seq
        total_chars += len(seq)
        for c in seq:
            checksum_dec += ord(c)
end = time.perf_counter()

seconds = end - start
mbps = (total_chars / (1024.0 * 1024.0)) / (seconds + 1e-12)
print(f"real_get_ms={seconds * 1000.0}")
print(f"real_get_chars={total_chars}")
print(f"checksum_dec={checksum_dec}")
print(f"real_get_mbps={mbps}")
'''


def repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def temp_parent() -> Path:
    parent = Path(os.environ.get("TMPDIR", str(Path.home() / "tmp")))
    parent.mkdir(parents=True, exist_ok=True)
    return parent


def compile_runner(
    root: Path,
    compiler: str,
    output: Path,
    source: str = CPP_SOURCE,
    enable_uring: bool = False,
) -> None:
    cpp_path = output.with_suffix(".cpp")
    cpp_path.write_text(source, encoding="utf-8")
    cmd = [
        compiler,
        "-std=c++17",
        "-O3",
        "-march=native",
    ]
    if enable_uring:
        cmd.append("-DPAGED_AASTORE_ENABLE_IO_URING")
    cmd += [
        "-I",
        str(root / "src"),
        str(cpp_path),
        "-o",
        str(output),
        "-pthread",
        "-lstdc++fs",
    ]
    if enable_uring:
        cmd.append("-luring")
    subprocess.run(cmd, check=True)


def python_from_pyfastx_bin(pyfastx_bin: Path) -> Path:
    return pyfastx_bin.resolve().parent / "python"


def run_pyfastx(input_path: Path, sample_count: int, seed: int, access: str, python: Path | None) -> int:
    if python is None:
        default_pyfastx = Path.home() / "miniconda3/envs/pyfastx_env/bin/pyfastx"
        if default_pyfastx.exists():
            python = python_from_pyfastx_bin(default_pyfastx)
        else:
            python = Path(sys.executable)

    cmd = [
        str(python),
        "-c",
        PYFASTX_SOURCE,
        str(input_path),
        str(sample_count),
        str(seed),
        access,
    ]
    return subprocess.run(cmd).returncode


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark random access for N sequences in ProteinAAStore, paged AAStore, or pyfastx."
    )
    parser.add_argument("input", help="ProteinAAStore binary, paged store directory, or FASTA file")
    parser.add_argument(
        "--backend",
        choices=("aastore", "paged", "pyfastx"),
        default="aastore",
        help="aastore uses ProteinAAStore::get(); paged uses PagedProteinAAStore::get(); pyfastx uses pyfastx.Fasta",
    )
    parser.add_argument("-n", "--num", type=int, required=True, help="number of random accesses")
    parser.add_argument("--seed", type=int, default=12345, help="random seed")
    parser.add_argument(
        "--mode",
        choices=("get", "checksum", "both"),
        default="get",
        help="get measures real get() decode + string allocation; checksum avoids string allocation",
    )
    parser.add_argument("--compiler", default=os.environ.get("CXX", "g++"))
    parser.add_argument(
        "--pyfastx-bin",
        type=Path,
        default=Path.home() / "miniconda3/envs/pyfastx_env/bin/pyfastx",
        help="pyfastx executable; its sibling python is used unless --pyfastx-python is set",
    )
    parser.add_argument(
        "--pyfastx-python",
        type=Path,
        help="Python executable with pyfastx installed",
    )
    parser.add_argument(
        "--pyfastx-access",
        choices=("index", "name"),
        default="index",
        help="access pyfastx by numeric index or by sequence name",
    )
    parser.add_argument("--memory-mb", "--cache-mb", dest="memory_mb", type=int, default=0, help="paged backend resident memory budget in MiB; 0/default loads all pages; --cache-mb is a legacy alias")
    parser.add_argument("--paged-batch", action="store_true", help="force paged backend batch_get even when --prefetch-mb 0; default nonzero prefetch already uses batch_get")
    parser.add_argument("--preload-cache", action="store_true", help="legacy flag; preload is automatic for --memory-mb > 0 and full preload is automatic when memory fits pages.bin")
    parser.add_argument("--no-preload-cache", action="store_true", help="paged backend benchmark only: do not preload resident PageCache before timed access")
    parser.add_argument("--preload-threads", type=int, default=0, help="advanced override: paged backend preload threads; 0 chooses up to 16 automatically")
    parser.add_argument("--prefetch-mb", type=int, default=16, help="paged backend temporary miss-page buffer upper bound in MiB; effective value is capped by the library default, 0 disables batch prefetch")
    parser.add_argument("--prefetch-threads", type=int, default=2, help="paged backend total I/O streams including caller; 1 or 0 uses caller only")
    parser.add_argument("--prefetch-depth", type=int, default=0, help="advanced override: paged backend outstanding reads per I/O stream; 0 lets the library choose from --prefetch-mb")
    parser.add_argument("--io-backend", choices=("auto", "pread", "aio", "uring", "uring_file", "uring_fixed"), default="auto", help="advanced override: auto prefers io_uring, then native AIO, then pread")
    parser.add_argument("--cache-policy", choices=("lru", "prefix", "non-lru", "non_lru"), default="lru", help="paged backend resident cache policy: lru promotes miss pages; prefix keeps only the preloaded prefix")
    parser.add_argument("--uring-sqpoll", action="store_true", help="paged backend uses io_uring SQPOLL for uring backends")
    parser.add_argument("--single-thread-aio", action="store_true", help="legacy alias for caller-only async I/O; equivalent to --prefetch-threads 0")
    parser.add_argument("--single-thread-aio-completions", "--aio-completions", type=int, default=64, help="advanced override: max completions drained before refilling I/O")
    parser.add_argument("--aio-completion-min", type=int, default=4, help="paged backend min Linux AIO completions to wait for before non-blocking drain")
    parser.add_argument("--drop-page-cache", action="store_true", help="paged backend asks the kernel to evict pages.bin from OS page cache before timed access")
    parser.add_argument(
        "--keep-binary",
        type=Path,
        help="optional path to keep the compiled AAStore/paged benchmark runner",
    )
    args = parser.parse_args()

    if args.num < 0:
        parser.error("--num must be non-negative")

    root = repo_root()
    input_path = Path(args.input).resolve()
    if not input_path.exists():
        parser.error(f"input file does not exist: {input_path}")

    if args.backend == "pyfastx":
        pyfastx_python = args.pyfastx_python
        if pyfastx_python is None and args.pyfastx_bin and args.pyfastx_bin.exists():
            pyfastx_python = python_from_pyfastx_bin(args.pyfastx_bin)
        if pyfastx_python is not None and not pyfastx_python.exists():
            parser.error(f"pyfastx python does not exist: {pyfastx_python}")
        return run_pyfastx(input_path, args.num, args.seed, args.pyfastx_access, pyfastx_python)

    if args.backend == "paged" and args.memory_mb < 0:
        parser.error("--memory-mb must be non-negative")
    if args.backend == "paged" and args.preload_threads < 0:
        parser.error("--preload-threads must be non-negative")
    if args.backend == "paged" and args.prefetch_threads < 0:
        parser.error("--prefetch-threads must be non-negative")
    if args.backend == "paged" and args.prefetch_mb < 0:
        parser.error("--prefetch-mb must be non-negative")
    if args.backend == "paged" and args.prefetch_depth < 0:
        parser.error("--prefetch-depth must be non-negative")
    if args.backend == "paged" and args.single_thread_aio_completions < 1:
        parser.error("--single-thread-aio-completions must be at least 1")
    if args.backend == "paged" and args.aio_completion_min < 1:
        parser.error("--aio-completion-min must be at least 1")
    use_single_thread_aio = args.backend == "paged" and (
        args.single_thread_aio or args.prefetch_threads == 0
    )
    if use_single_thread_aio:
        if args.prefetch_mb <= 0:
            parser.error("caller-thread AIO requires --prefetch-mb > 0")
        if args.prefetch_depth == 1:
            parser.error("caller-thread AIO requires --prefetch-depth 0(auto) or >1")

    try:
        if args.keep_binary:
            runner = args.keep_binary.resolve()
            runner.parent.mkdir(parents=True, exist_ok=True)
            if args.backend == "paged":
                compile_runner(root, args.compiler, runner, PAGED_CPP_SOURCE, enable_uring=True)
                paged_mode = "batch" if args.paged_batch else "get"
                preload = "1" if args.preload_cache else "0"
                cmd = [
                    str(runner),
                    str(input_path),
                    str(args.num),
                    str(args.seed),
                    str(args.memory_mb),
                    paged_mode,
                    preload,
                    str(args.preload_threads),
                    str(args.prefetch_mb),
                    str(args.prefetch_threads),
                    str(args.prefetch_depth),
                    "1" if use_single_thread_aio else "0",
                    str(args.single_thread_aio_completions),
                    str(args.aio_completion_min),
                    "1" if args.drop_page_cache else "0",
                    args.io_backend,
                    "1" if args.uring_sqpoll else "0",
                    "1" if args.no_preload_cache else "0",
                    args.cache_policy,
                ]
            else:
                compile_runner(root, args.compiler, runner)
                cmd = [str(runner), str(input_path), str(args.num), str(args.seed), args.mode]
            return subprocess.run(cmd).returncode

        with tempfile.TemporaryDirectory(prefix=f"{args.backend}-random-access-", dir=temp_parent()) as tmp:
            runner = Path(tmp) / "random_access_runner"
            if args.backend == "paged":
                compile_runner(root, args.compiler, runner, PAGED_CPP_SOURCE, enable_uring=True)
                paged_mode = "batch" if args.paged_batch else "get"
                preload = "1" if args.preload_cache else "0"
                cmd = [
                    str(runner),
                    str(input_path),
                    str(args.num),
                    str(args.seed),
                    str(args.memory_mb),
                    paged_mode,
                    preload,
                    str(args.preload_threads),
                    str(args.prefetch_mb),
                    str(args.prefetch_threads),
                    str(args.prefetch_depth),
                    "1" if use_single_thread_aio else "0",
                    str(args.single_thread_aio_completions),
                    str(args.aio_completion_min),
                    "1" if args.drop_page_cache else "0",
                    args.io_backend,
                    "1" if args.uring_sqpoll else "0",
                    "1" if args.no_preload_cache else "0",
                    args.cache_policy,
                ]
            else:
                compile_runner(root, args.compiler, runner)
                cmd = [str(runner), str(input_path), str(args.num), str(args.seed), args.mode]
            return subprocess.run(cmd).returncode
    except subprocess.CalledProcessError as exc:
        print(f"command failed with exit code {exc.returncode}: {' '.join(map(str, exc.cmd))}", file=sys.stderr)
        return exc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
