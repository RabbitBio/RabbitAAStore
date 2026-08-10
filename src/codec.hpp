#ifndef AASTORE_CODEC_HPP
#define AASTORE_CODEC_HPP

#include <cstddef>
#include <cstdint>

namespace aastore::detail {

uint64_t packed_bytes_for_residues(uint64_t residues) noexcept;

// Appends uppercase FASTA residues to one zero-initialized original AAStore
// page. offset is measured in residues within that page.
void append_packed_5bit(uint8_t* packed,
                        uint64_t& offset,
                        const char* residues,
                        size_t length);

void decode_packed_5bit_range(const uint8_t* packed,
                              uint64_t offset,
                              uint64_t length,
                              char* output);

const char* packed_5bit_decode_kernel_name() noexcept;

}  // namespace aastore::detail

#endif
