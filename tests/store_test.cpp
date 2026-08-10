#include "aastore/store.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

class TempDir {
public:
    TempDir()
        : path_(std::filesystem::temp_directory_path() /
                ("aastore_byte_range_test_" + std::to_string(::getpid()))) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

int main() {
    TempDir temp;
    const std::filesystem::path fasta = temp.path() / "proteins.fa";
    const std::string page_filler(aastore::kPageResidues, 'A');
    {
        std::ofstream output(fasta);
        output << ">empty\n"
               << ">p0 description\nacdefghi\n"
               << ">filler1\n" << page_filler << "\n"
               << ">p1\nKLMNPQRS\n"
               << ">filler2\n" << page_filler << "\n"
               << ">p2\nTVWYBXZJ\n"
               << ">long\nACDEFGHIKLMNPQRSTVWY*--OU\n";
    }
    const std::filesystem::path store_path = temp.path() / "store";
    const aastore::BuildStats built =
        aastore::Builder::build_fasta(fasta, store_path);
    const std::vector<std::string> expected = {
        "", "ACDEFGHI", page_filler, "KLMNPQRS", page_filler,
        "TVWYBXZJ", "ACDEFGHIKLMNPQRSTVWY*--OU"};
    expect(built.sequences == expected.size(), "sequence count");
    expect(std::filesystem::exists(store_path / "meta.bin"),
           "original meta.bin exists");
    expect(std::filesystem::exists(store_path / "pages.bin"),
           "original pages.bin exists");
    expect(!std::filesystem::exists(store_path / "manifest.bin"),
           "no V2 manifest");
    {
        std::ifstream meta(store_path / "meta.bin", std::ios::binary);
        char magic[8] = {};
        meta.read(magic, sizeof(magic));
        expect(std::string(magic, 7) == "AAPCMP2",
               "builder writes current original compact metadata");
    }
    {
        std::ifstream names(store_path / "seq_names.txt");
        const std::string content((std::istreambuf_iterator<char>(names)),
                                  std::istreambuf_iterator<char>());
        expect(content ==
                   "empty\np0\nfiller1\np1\nfiller2\np2\nlong\n",
               "original seq_names.txt format");
    }

    aastore::Store store(store_path);
    expect(store.size() == expected.size(), "opened sequence count");
    for (size_t i = 0; i < expected.size(); ++i) {
        expect(store.get(i) == expected[i],
               "single get " + std::to_string(i));
        expect(store.sequence_length(i) == expected[i].size(),
               "sequence length " + std::to_string(i));
    }

    const std::vector<aastore::SequenceId> ids = {6, 1, 0, 3, 5, 1};
    const aastore::Batch batch = store.batch_get(ids);
    expect(batch.size() == ids.size(), "batch size");
    for (size_t i = 0; i < ids.size(); ++i) {
        expect(batch.view(i) == expected[ids[i]],
               "batch preserves request order " + std::to_string(i));
        expect(batch.storage[batch.offsets[i] + batch.lengths[i]] == '\0',
               "batch string terminator");
    }

    // p0 and p1 begin exactly one 4 KiB packed-page gap apart after alignment.
    const aastore::Batch merged = store.batch_get({1, 3});
    expect(merged.stats.ranges == 1, "4 KiB gap merges");
    expect(merged.stats.merged_gaps == 1, "merged gap is counted");
    expect(merged.stats.selected_merge_gap_bytes == 4096,
           "fixed merge gap is reported");

    aastore::ReadOptions exact_options;
    exact_options.merge_gap_bytes = 0;
    const aastore::Batch exact = store.batch_get({1, 3}, exact_options);
    expect(exact.stats.ranges == 2, "zero gap keeps disjoint extents");
    expect(exact.stats.selected_merge_gap_bytes == 0,
           "zero merge gap is reported");

    // p2 is another packed page away, so the gap from p0 exceeds 4 KiB.
    const aastore::Batch separate = store.batch_get({1, 5});
    expect(separate.stats.ranges == 2, "gap above 4 KiB stays separate");

    aastore::ReadOptions transfer_dominated;
    transfer_dominated.adaptive_merge_gap = true;
    transfer_dominated.adaptive.cache_aware = false;
    transfer_dominated.adaptive.io_latency_ns = 0;
    transfer_dominated.adaptive.io_bandwidth_bytes_per_second = 1;
    transfer_dominated.adaptive.max_merged_range_bytes = 0;
    const aastore::Batch adaptive_exact =
        store.batch_get({1, 3}, transfer_dominated);
    expect(adaptive_exact.stats.ranges == 2,
           "adaptive planner avoids overread when latency is free");
    expect(adaptive_exact.stats.selected_merge_gap_bytes == 0,
           "adaptive planner selects zero gap for transfer-only cost");
    expect(adaptive_exact.stats.merge_candidates > 1,
           "adaptive planner evaluates candidate gaps");

    aastore::ReadOptions latency_dominated;
    latency_dominated.adaptive_merge_gap = true;
    latency_dominated.adaptive.cache_aware = false;
    latency_dominated.adaptive.io_latency_ns = 1000000000ULL;
    latency_dominated.adaptive.io_bandwidth_bytes_per_second =
        1000000000000ULL;
    latency_dominated.adaptive.max_merged_range_bytes = 0;
    latency_dominated.io_threads = 1;
    const aastore::Batch adaptive_merged =
        store.batch_get({1, 5}, latency_dominated);
    expect(adaptive_merged.stats.ranges == 1,
           "adaptive planner merges wider gap when latency dominates");
    expect(adaptive_merged.stats.selected_merge_gap_bytes > 4096,
           "adaptive planner reports the wider selected gap");
    expect(adaptive_merged.view(0) == expected[1] &&
               adaptive_merged.view(1) == expected[5],
           "adaptive merge preserves decoded sequences");

    aastore::ReadOptions cache_aware;
    cache_aware.adaptive_merge_gap = true;
    const aastore::Batch cache_aware_batch =
        store.batch_get({1, 3, 5}, cache_aware);
    expect(cache_aware_batch.view(0) == expected[1] &&
               cache_aware_batch.view(1) == expected[3] &&
               cache_aware_batch.view(2) == expected[5],
           "cache-aware adaptive read preserves decoded sequences");
#ifdef __linux__
    expect(cache_aware_batch.stats.page_cache_residency_known,
           "Linux page-cache residency is available");
    expect(cache_aware_batch.stats.page_cache_total_bytes ==
               store.packed_bytes(),
           "page-cache residency covers pages.bin");
#endif

    bool invalid_model_rejected = false;
    try {
        aastore::ReadOptions invalid_model;
        invalid_model.adaptive_merge_gap = true;
        invalid_model.adaptive.cache_aware = false;
        invalid_model.adaptive.io_bandwidth_bytes_per_second = 0;
        (void)store.batch_get({1}, invalid_model);
    } catch (const std::invalid_argument&) {
        invalid_model_rejected = true;
    }
    expect(invalid_model_rejected,
           "adaptive planner rejects zero I/O bandwidth");

    aastore::ReadOptions fixed_without_model;
    fixed_without_model.adaptive.io_bandwidth_bytes_per_second = 0;
    const aastore::Batch fixed_without_estimate =
        store.batch_get({1}, fixed_without_model);
    expect(fixed_without_estimate.view(0) == expected[1],
           "fixed planner does not require an I/O model");

    const aastore::Batch empty = store.batch_get({});
    expect(empty.size() == 0 && empty.stats.ranges == 0,
           "empty batch");
    bool rejected = false;
    try {
        (void)store.get(store.size());
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    expect(rejected, "out-of-range id rejected");

    bool overwrite_rejected = false;
    try {
        (void)aastore::Builder::build_fasta(fasta, store_path);
    } catch (const std::runtime_error&) {
        overwrite_rejected = true;
    }
    expect(overwrite_rejected, "overwrite is explicit");

    if (failures != 0) {
        std::cerr << "aastore test: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "aastore test: ok\n";
    return 0;
}
