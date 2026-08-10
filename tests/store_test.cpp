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

    // p2 is another packed page away, so the gap from p0 exceeds 4 KiB.
    const aastore::Batch separate = store.batch_get({1, 5});
    expect(separate.stats.ranges == 2, "gap above 4 KiB stays separate");

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
