#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    uint64_t sequence_count = 100000000;
    if (argc == 3 && std::string(argv[1]) == "-n") {
        try {
            sequence_count = std::stoull(argv[2]);
        } catch (...) {
            std::cerr << "Invalid sequence count: " << argv[2] << "\n";
            return 1;
        }
    } else if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " [-n sequence_count]\n";
        return 1;
    }

    constexpr size_t buffer_entries = (8ULL * 1024ULL * 1024ULL) / sizeof(uint64_t);
    constexpr uint64_t lengths[] = {317, 341, 359, 389, 421, 443, 467, 509};

    std::vector<uint64_t> positions;
    positions.reserve(buffer_entries);
    positions.push_back(0);

    uint64_t position = 0;
    uint64_t last_position = 0;
    uint64_t flushes = 0;
    auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < sequence_count; ++i) {
        position += lengths[i % (sizeof(lengths) / sizeof(lengths[0]))];
        if (position < last_position) {
            std::cerr << "Non-monotonic test position\n";
            return 1;
        }
        positions.push_back(position);
        last_position = position;
        if (positions.size() == buffer_entries) {
            positions.clear();
            ++flushes;
        }
    }
    auto end = std::chrono::steady_clock::now();

    const double elapsed_ms = std::chrono::duration<double, std::milli>(end - start).count();
    const double entries_per_second = elapsed_ms > 0.0
        ? static_cast<double>(sequence_count) * 1000.0 / elapsed_ms
        : 0.0;
    std::cout << std::fixed << std::setprecision(3);
    std::cout << "sequence_count=" << sequence_count << "\n";
    std::cout << "index_buffer_bytes=" << (buffer_entries * sizeof(uint64_t)) << "\n";
    std::cout << "index_append_ms=" << elapsed_ms << "\n";
    std::cout << "index_entries_per_sec=" << entries_per_second << "\n";
    std::cout << "index_buffer_flushes=" << flushes << "\n";
    std::cout << "final_position=" << position << "\n";
    return 0;
}
