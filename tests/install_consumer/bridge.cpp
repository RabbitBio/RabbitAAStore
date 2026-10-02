#include <aastore/store.hpp>

#include <cstdint>

extern "C" uint64_t aastore_consumer_record_count(const char* path) {
    aastore::Store store(path);
    return store.size();
}
