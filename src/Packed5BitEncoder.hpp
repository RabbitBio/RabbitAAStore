#ifndef AASTORE_PACKED_5BIT_ENCODER_HPP
#define AASTORE_PACKED_5BIT_ENCODER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define AASTORE_X86_ENCODE_SIMD 1
#else
#define AASTORE_X86_ENCODE_SIMD 0
#endif

namespace aastore_detail {

enum class Packed5BitEncodeKernel : uint8_t {
    Scalar8 = 0,
    Bmi2_32 = 1
};

static inline uint64_t load_8_bytes_le(const char* src) {
    uint64_t value = 0;
    std::memcpy(&value, src, sizeof(value));
    return value;
}

static inline void store_5_bytes_le(uint8_t* dst, uint64_t value) {
    std::memcpy(dst, &value, 5);
}

static inline void pack_8_ascii_5bit_scalar(const char* src, uint8_t* dst) {
    uint64_t packed =
        (static_cast<uint64_t>(static_cast<uint8_t>(src[0]) & 0x1F)) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[1]) & 0x1F) << 5) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[2]) & 0x1F) << 10) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[3]) & 0x1F) << 15) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[4]) & 0x1F) << 20) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[5]) & 0x1F) << 25) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[6]) & 0x1F) << 30) |
        (static_cast<uint64_t>(static_cast<uint8_t>(src[7]) & 0x1F) << 35);
    store_5_bytes_le(dst, packed);
}

static inline uint8_t encode_aa_5bit_stop_branchless(char c) {
    uint8_t x = static_cast<uint8_t>(c);
    uint8_t bit = static_cast<uint8_t>(((x >> 5) ^ 2) & 1);
    return static_cast<uint8_t>((x & 0x1F) ^ ((bit << 4) | bit));
}

static inline void pack_8_ascii_5bit_stop_scalar(const char* src, uint8_t* dst) {
    uint64_t packed =
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[0]))) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[1])) << 5) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[2])) << 10) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[3])) << 15) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[4])) << 20) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[5])) << 25) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[6])) << 30) |
        (static_cast<uint64_t>(encode_aa_5bit_stop_branchless(src[7])) << 35);
    store_5_bytes_le(dst, packed);
}

static inline bool has_stop_char_64_scalar(const char* src) {
    for (size_t i = 0; i < 64; ++i) {
        if (src[i] == '*') return true;
    }
    return false;
}

#if AASTORE_X86_ENCODE_SIMD && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("bmi2")))
static inline uint64_t pack_8_ascii_5bit_bmi2_word(const char* src) {
    constexpr uint64_t ascii_code_mask = 0x1F1F1F1F1F1F1F1FULL;
    return _pext_u64(load_8_bytes_le(src), ascii_code_mask);
}

__attribute__((target("bmi2")))
static inline void pack_32_ascii_5bit_bmi2(const char* src, uint8_t* dst) {
    store_5_bytes_le(dst, pack_8_ascii_5bit_bmi2_word(src));
    store_5_bytes_le(dst + 5, pack_8_ascii_5bit_bmi2_word(src + 8));
    store_5_bytes_le(dst + 10, pack_8_ascii_5bit_bmi2_word(src + 16));
    store_5_bytes_le(dst + 15, pack_8_ascii_5bit_bmi2_word(src + 24));
}

__attribute__((target("bmi2")))
static inline void pack_64_ascii_5bit_bmi2(const char* src, uint8_t* dst) {
    pack_32_ascii_5bit_bmi2(src, dst);
    pack_32_ascii_5bit_bmi2(src + 32, dst + 20);
}

__attribute__((target("avx2")))
static inline bool has_stop_char_64_avx2(const char* src) {
    const __m256i stop = _mm256_set1_epi8('*');
    const __m256i a = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(src));
    const __m256i b = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(src + 32));
    const __m256i eq_a = _mm256_cmpeq_epi8(a, stop);
    const __m256i eq_b = _mm256_cmpeq_epi8(b, stop);
    return (_mm256_movemask_epi8(eq_a) | _mm256_movemask_epi8(eq_b)) != 0;
}
#endif

static inline void pack_64_ascii_5bit_stop_scalar(const char* src,
                                                  uint8_t* dst) {
    for (size_t i = 0; i < 8; ++i) {
        pack_8_ascii_5bit_stop_scalar(src + i * 8, dst + i * 5);
    }
}

static inline void pack_64_ascii_5bit_hybrid_stop(const char* src,
                                                  uint8_t* dst) {
#if AASTORE_X86_ENCODE_SIMD && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2")) {
        if (!has_stop_char_64_avx2(src)) {
            pack_64_ascii_5bit_bmi2(src, dst);
        } else {
            pack_64_ascii_5bit_stop_scalar(src, dst);
        }
        return;
    }
    if (__builtin_cpu_supports("bmi2")) {
        if (!has_stop_char_64_scalar(src)) {
            pack_64_ascii_5bit_bmi2(src, dst);
        } else {
            pack_64_ascii_5bit_stop_scalar(src, dst);
        }
        return;
    }
#endif
    if (!has_stop_char_64_scalar(src)) {
        for (size_t i = 0; i < 8; ++i) {
            pack_8_ascii_5bit_scalar(src + i * 8, dst + i * 5);
        }
    } else {
        pack_64_ascii_5bit_stop_scalar(src, dst);
    }
}

static inline Packed5BitEncodeKernel detect_packed_5bit_encode_kernel() {
#if AASTORE_X86_ENCODE_SIMD && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("bmi2")) {
        return Packed5BitEncodeKernel::Bmi2_32;
    }
#endif
    return Packed5BitEncodeKernel::Scalar8;
}

static inline Packed5BitEncodeKernel packed_5bit_encode_kernel() {
    static const Packed5BitEncodeKernel kernel =
        detect_packed_5bit_encode_kernel();
    return kernel;
}

static inline const char* packed_5bit_encode_kernel_name(
    Packed5BitEncodeKernel kernel) {
    switch (kernel) {
        case Packed5BitEncodeKernel::Bmi2_32:
            return "bmi2_pext_32";
        case Packed5BitEncodeKernel::Scalar8:
        default:
            return "scalar_8";
    }
}

}  // namespace aastore_detail

#undef AASTORE_X86_ENCODE_SIMD

#endif
