#ifndef AASTORE_PACKED_5BIT_DECODER_HPP
#define AASTORE_PACKED_5BIT_DECODER_HPP

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define AASTORE_X86_SIMD 1
#else
#define AASTORE_X86_SIMD 0
#endif

namespace aastore_detail {

enum class Packed5BitDecodeKernel : uint8_t {
    Scalar32 = 0,
    Avx2Bmi2 = 1,
    Avx512Vbmi2 = 2
};

static inline uint8_t packed_5bit_code_at(const uint8_t* packed,
                                          uint64_t offset) {
    uint64_t bit_pos = offset * 5;
    size_t byte_pos = static_cast<size_t>(bit_pos >> 3);
    int shift = static_cast<int>(bit_pos & 7);
    uint16_t value = static_cast<uint16_t>(packed[byte_pos] >> shift);
    if (shift > 3) {
        value |= static_cast<uint16_t>(packed[byte_pos + 1]) << (8 - shift);
    }
    return static_cast<uint8_t>(value & 0x1F);
}

static inline uint64_t load_5_bytes_le(const uint8_t* src) {
    return static_cast<uint64_t>(src[0]) |
           (static_cast<uint64_t>(src[1]) << 8) |
           (static_cast<uint64_t>(src[2]) << 16) |
           (static_cast<uint64_t>(src[3]) << 24) |
           (static_cast<uint64_t>(src[4]) << 32);
}

static inline char decode_5bit_code_to_char(uint8_t code) {
    uint8_t ascii = static_cast<uint8_t>(0x40 | (code & 0x1F));
    uint8_t x = static_cast<uint8_t>((code & 0x1F) ^ 27U);
    uint8_t is_stop = static_cast<uint8_t>((static_cast<uint8_t>(x - 1U) >> 7) & 1U);
    return static_cast<char>(ascii ^ (is_stop * 0x71U));
}

static inline uint64_t fix_stop_chars_u64(uint64_t ascii) {
    constexpr uint64_t stop_ascii = 0x5B5B5B5B5B5B5B5BULL;
    constexpr uint64_t low_bits = 0x0101010101010101ULL;
    constexpr uint64_t high_bits = 0x8080808080808080ULL;
    uint64_t x = ascii ^ stop_ascii;
    uint64_t high_mask = (x - low_bits) & ~x & high_bits;
    uint64_t xor_mask = (high_mask >> 7) * 0x71ULL;
    return ascii ^ xor_mask;
}

static inline void decode_8_scalar(const uint8_t* packed,
                                   uint64_t offset,
                                   char* dst) {
    const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
    uint64_t x = load_5_bytes_le(src);

    dst[0] = decode_5bit_code_to_char(static_cast<uint8_t>(x & 0x1FULL));
    dst[1] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 5) & 0x1FULL));
    dst[2] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 10) & 0x1FULL));
    dst[3] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 15) & 0x1FULL));
    dst[4] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 20) & 0x1FULL));
    dst[5] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 25) & 0x1FULL));
    dst[6] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 30) & 0x1FULL));
    dst[7] = decode_5bit_code_to_char(static_cast<uint8_t>((x >> 35) & 0x1FULL));
}

static inline void decode_16_scalar(const uint8_t* packed,
                                    uint64_t offset,
                                    char* dst) {
    decode_8_scalar(packed, offset, dst);
    decode_8_scalar(packed, offset + 8, dst + 8);
}

static inline void decode_32_scalar(const uint8_t* packed,
                                    uint64_t offset,
                                    char* dst) {
    decode_16_scalar(packed, offset, dst);
    decode_16_scalar(packed, offset + 16, dst + 16);
}

#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("bmi2")))
static inline uint64_t decode_8_bmi2_word_raw(const uint8_t* src) {
    constexpr uint64_t code_mask = 0x1F1F1F1F1F1F1F1FULL;
    constexpr uint64_t ascii_mask = 0x4040404040404040ULL;
    return _pdep_u64(load_5_bytes_le(src), code_mask) | ascii_mask;
}

__attribute__((target("bmi2")))
static inline uint64_t decode_8_bmi2_word(const uint8_t* src) {
    return fix_stop_chars_u64(decode_8_bmi2_word_raw(src));
}

__attribute__((target("avx2")))
static inline __m128i fix_stop_chars_avx2_128(__m128i decoded) {
    const __m128i stop = _mm_set1_epi8('[');
    const __m128i xor_value = _mm_set1_epi8(static_cast<char>(0x71));
    const __m128i match = _mm_cmpeq_epi8(decoded, stop);
    return _mm_xor_si128(decoded, _mm_and_si128(match, xor_value));
}

__attribute__((target("avx2")))
static inline __m256i fix_stop_chars_avx2_256(__m256i decoded) {
    const __m256i stop = _mm256_set1_epi8('[');
    const __m256i xor_value = _mm256_set1_epi8(static_cast<char>(0x71));
    const __m256i match = _mm256_cmpeq_epi8(decoded, stop);
    return _mm256_xor_si256(decoded, _mm256_and_si256(match, xor_value));
}

__attribute__((target("avx2,bmi2")))
static inline void decode_16_avx2_bmi2(const uint8_t* packed,
                                       uint64_t offset,
                                       char* dst) {
    const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
    uint64_t y0 = decode_8_bmi2_word_raw(src);
    uint64_t y1 = decode_8_bmi2_word_raw(src + 5);
    __m128i decoded = _mm_set_epi64x(static_cast<long long>(y1),
                                     static_cast<long long>(y0));
    decoded = fix_stop_chars_avx2_128(decoded);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), decoded);
}

__attribute__((target("avx2,bmi2")))
static inline void decode_32_avx2_bmi2(const uint8_t* packed,
                                       uint64_t offset,
                                       char* dst) {
    const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
    uint64_t y0 = decode_8_bmi2_word_raw(src);
    uint64_t y1 = decode_8_bmi2_word_raw(src + 5);
    uint64_t y2 = decode_8_bmi2_word_raw(src + 10);
    uint64_t y3 = decode_8_bmi2_word_raw(src + 15);
    __m256i decoded = _mm256_set_epi64x(static_cast<long long>(y3),
                                        static_cast<long long>(y2),
                                        static_cast<long long>(y1),
                                        static_cast<long long>(y0));
    decoded = fix_stop_chars_avx2_256(decoded);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), decoded);
}

__attribute__((target("avx512f,avx512bw,avx512vl")))
static inline __m512i fix_stop_chars_avx512(__m512i decoded) {
    __mmask64 match = _mm512_cmpeq_epi8_mask(decoded, _mm512_set1_epi8('['));
    __m512i xor_value = _mm512_maskz_mov_epi8(
        match, _mm512_set1_epi8(static_cast<char>(0x71)));
    return _mm512_xor_si512(decoded, xor_value);
}

__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi,avx512vbmi2")))
static inline void decode_16_avx512_vbmi2(const uint8_t* packed,
                                          uint64_t offset,
                                          char* dst) {
    constexpr __mmask64 load_mask = static_cast<__mmask64>(0x1F1FULL);
    const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
    __m512i expanded = _mm512_maskz_expandloadu_epi8(load_mask, src);
    __m512i shifts = _mm512_set1_epi64(0x231E19140F0A0500ULL);
    __m512i decoded = _mm512_multishift_epi64_epi8(shifts, expanded);
    decoded = _mm512_and_si512(decoded, _mm512_set1_epi8(0x1F));
    decoded = _mm512_or_si512(decoded, _mm512_set1_epi8(0x40));
    decoded = fix_stop_chars_avx512(decoded);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst),
                     _mm512_castsi512_si128(decoded));
}

__attribute__((target("avx512f,avx512bw,avx512vl,avx512vbmi,avx512vbmi2")))
static inline void decode_32_avx512_vbmi2(const uint8_t* packed,
                                          uint64_t offset,
                                          char* dst) {
    constexpr __mmask64 load_mask = static_cast<__mmask64>(0x1F1F1F1FULL);
    const uint8_t* src = packed + static_cast<size_t>((offset >> 3) * 5);
    __m512i expanded = _mm512_maskz_expandloadu_epi8(load_mask, src);
    __m512i shifts = _mm512_set1_epi64(0x231E19140F0A0500ULL);
    __m512i decoded = _mm512_multishift_epi64_epi8(shifts, expanded);
    decoded = _mm512_and_si512(decoded, _mm512_set1_epi8(0x1F));
    decoded = _mm512_or_si512(decoded, _mm512_set1_epi8(0x40));
    decoded = fix_stop_chars_avx512(decoded);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst),
                        _mm512_castsi512_si256(decoded));
}
#endif

static inline Packed5BitDecodeKernel detect_packed_5bit_decode_kernel() {
#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") &&
        __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vl") &&
        __builtin_cpu_supports("avx512vbmi") &&
        __builtin_cpu_supports("avx512vbmi2")) {
        return Packed5BitDecodeKernel::Avx512Vbmi2;
    }
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("bmi2")) {
        return Packed5BitDecodeKernel::Avx2Bmi2;
    }
#endif
    return Packed5BitDecodeKernel::Scalar32;
}

static inline Packed5BitDecodeKernel packed_5bit_decode_kernel() {
    static const Packed5BitDecodeKernel kernel =
        detect_packed_5bit_decode_kernel();
    return kernel;
}

static inline const char* packed_5bit_decode_kernel_name(
    Packed5BitDecodeKernel kernel) {
    switch (kernel) {
        case Packed5BitDecodeKernel::Avx512Vbmi2:
            return "avx512_vbmi2_32";
        case Packed5BitDecodeKernel::Avx2Bmi2:
            return "avx2_bmi2_32";
        case Packed5BitDecodeKernel::Scalar32:
        default:
            return "scalar_unrolled_32";
    }
}

static inline void decode_packed_5bit_range_with_kernel(
    const uint8_t* packed,
    uint64_t offset,
    uint64_t length,
    char* dst,
    Packed5BitDecodeKernel kernel) {
    uint64_t pos = 0;
    while (pos < length && ((offset + pos) & 7ULL) != 0) {
        dst[static_cast<size_t>(pos)] =
            decode_5bit_code_to_char(packed_5bit_code_at(packed, offset + pos));
        ++pos;
    }

    switch (kernel) {
#if AASTORE_X86_SIMD && (defined(__GNUC__) || defined(__clang__))
        case Packed5BitDecodeKernel::Avx512Vbmi2:
            while (pos + 32 <= length) {
                decode_32_avx512_vbmi2(packed, offset + pos,
                                       dst + static_cast<size_t>(pos));
                pos += 32;
            }
            if (pos + 16 <= length) {
                decode_16_avx512_vbmi2(packed, offset + pos,
                                       dst + static_cast<size_t>(pos));
                pos += 16;
            }
            break;
        case Packed5BitDecodeKernel::Avx2Bmi2:
            while (pos + 32 <= length) {
                decode_32_avx2_bmi2(packed, offset + pos,
                                    dst + static_cast<size_t>(pos));
                pos += 32;
            }
            if (pos + 16 <= length) {
                decode_16_avx2_bmi2(packed, offset + pos,
                                    dst + static_cast<size_t>(pos));
                pos += 16;
            }
            if (pos + 8 <= length) {
                const uint8_t* src =
                    packed + static_cast<size_t>(((offset + pos) >> 3) * 5);
                uint64_t decoded = decode_8_bmi2_word(src);
                std::memcpy(dst + static_cast<size_t>(pos), &decoded,
                            sizeof(decoded));
                pos += 8;
            }
            break;
#endif
        case Packed5BitDecodeKernel::Scalar32:
        default:
            while (pos + 32 <= length) {
                decode_32_scalar(packed, offset + pos,
                                 dst + static_cast<size_t>(pos));
                pos += 32;
            }
            if (pos + 16 <= length) {
                decode_16_scalar(packed, offset + pos,
                                 dst + static_cast<size_t>(pos));
                pos += 16;
            }
            break;
    }

    while (pos + 8 <= length) {
        decode_8_scalar(packed, offset + pos, dst + static_cast<size_t>(pos));
        pos += 8;
    }
    while (pos < length) {
        dst[static_cast<size_t>(pos)] =
            decode_5bit_code_to_char(packed_5bit_code_at(packed, offset + pos));
        ++pos;
    }
}

static inline void decode_packed_5bit_range(const uint8_t* packed,
                                            uint64_t offset,
                                            uint64_t length,
                                            char* dst) {
    decode_packed_5bit_range_with_kernel(
        packed, offset, length, dst, packed_5bit_decode_kernel());
}

static inline void decode_packed_5bit_range_stop_aware(const uint8_t* packed,
                                                       uint64_t offset,
                                                       uint64_t length,
                                                       char* dst) {
    decode_packed_5bit_range(packed, offset, length, dst);
}

}  // namespace aastore_detail

#undef AASTORE_X86_SIMD

#endif
