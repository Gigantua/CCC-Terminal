#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define CCC_X86 1
#include <immintrin.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#define CCC_AVX2
#else
#define CCC_AVX2 __attribute__((target("avx2")))
#endif
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#define CCC_NEON 1
#include <arm_neon.h>
#define CCC_AVX2
#else
#error "ccc needs SSE2 or NEON"
#endif

namespace simd {

#ifdef CCC_X86
inline void pause() { _mm_pause(); }

inline bool wideFast() {
    static const bool yes = [] {
#if defined(_MSC_VER) && !defined(__clang__)
        int r[4];
        __cpuid(r, 1);
        if (!(r[2] & (1 << 27)) || (_xgetbv(0) & 6) != 6) return false;
        __cpuidex(r, 7, 0);
        return (r[1] & (1 << 5)) != 0;
#else
        return __builtin_cpu_supports("avx2") != 0;
#endif
    }();
    return yes;
}

struct Mask16 {
    unsigned bits;
    explicit operator bool() const { return bits != 0; }
    unsigned first() const { return (unsigned)std::countr_zero(bits); }
};

struct U8x16 {
    __m128i v;
    static U8x16 load(const void* p) { return { _mm_loadu_si128(static_cast<const __m128i*>(p)) }; }
    static U8x16 splat(uint8_t b) { return { _mm_set1_epi8((char)b) }; }
    friend U8x16 operator|(U8x16 a, U8x16 b) { return { _mm_or_si128(a.v, b.v) }; }
    friend U8x16 operator-(U8x16 a, U8x16 b) { return { _mm_sub_epi8(a.v, b.v) }; }
    friend U8x16 operator==(U8x16 a, U8x16 b) { return { _mm_cmpeq_epi8(a.v, b.v) }; }
    friend U8x16 atLeast(U8x16 a, U8x16 b) { return { _mm_cmpeq_epi8(_mm_max_epu8(a.v, b.v), a.v) }; }
    Mask16 mask() const { return { (unsigned)_mm_movemask_epi8(v) }; }
};

struct U16x8 {
    __m128i v;
    static U16x8 load(const void* p) { return { _mm_loadu_si128(static_cast<const __m128i*>(p)) }; }
    static U16x8 splat(uint16_t h) { return { _mm_set1_epi16((short)h) }; }
    friend U16x8 operator-(U16x8 a, U16x8 b) { return { _mm_sub_epi16(a.v, b.v) }; }
    friend U16x8 atLeast(U16x8 a, U16x8 b) { return { _mm_cmpeq_epi16(_mm_max_epu16(a.v, b.v), a.v) }; }
    unsigned firstSet() const {
        unsigned bits = (unsigned)_mm_movemask_epi8(v);
        return bits ? (unsigned)std::countr_zero(bits) / 2 : 8;
    }
};

struct Mask32 {
    uint32_t bits;
    explicit operator bool() const { return bits != 0; }
    unsigned first() const { return (unsigned)std::countr_zero(bits); }
    Mask32 operator>>(size_t n) const { return { bits >> n }; }
};

struct U8x32 {
    __m256i v;
    CCC_AVX2 static U8x32 loadAligned(const void* p) { return { _mm256_load_si256(static_cast<const __m256i*>(p)) }; }
    CCC_AVX2 static U8x32 splat(uint8_t b) { return { _mm256_set1_epi8((char)b) }; }
    CCC_AVX2 friend U8x32 operator|(U8x32 a, U8x32 b) { return { _mm256_or_si256(a.v, b.v) }; }
    CCC_AVX2 friend U8x32 operator==(U8x32 a, U8x32 b) { return { _mm256_cmpeq_epi8(a.v, b.v) }; }
    CCC_AVX2 Mask32 mask() const { return { (uint32_t)_mm256_movemask_epi8(v) }; }
};
#else
inline void pause() { __asm__ __volatile__("yield"); }

inline bool wideFast() { return true; }

struct Mask16 {
    uint64_t nibbles;
    explicit operator bool() const { return nibbles != 0; }
    unsigned first() const { return (unsigned)std::countr_zero(nibbles) / 4; }
};

inline uint64_t nibbleMask(uint8x16_t m) { return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0); }

struct U8x16 {
    uint8x16_t v;
    static U8x16 load(const void* p) { return { vld1q_u8(static_cast<const uint8_t*>(p)) }; }
    static U8x16 splat(uint8_t b) { return { vdupq_n_u8(b) }; }
    friend U8x16 operator|(U8x16 a, U8x16 b) { return { vorrq_u8(a.v, b.v) }; }
    friend U8x16 operator-(U8x16 a, U8x16 b) { return { vsubq_u8(a.v, b.v) }; }
    friend U8x16 operator==(U8x16 a, U8x16 b) { return { vceqq_u8(a.v, b.v) }; }
    friend U8x16 atLeast(U8x16 a, U8x16 b) { return { vcgeq_u8(a.v, b.v) }; }
    Mask16 mask() const { return { nibbleMask(v) }; }
};

struct U16x8 {
    uint16x8_t v;
    static U16x8 load(const void* p) { return { vld1q_u16(static_cast<const uint16_t*>(p)) }; }
    static U16x8 splat(uint16_t h) { return { vdupq_n_u16(h) }; }
    friend U16x8 operator-(U16x8 a, U16x8 b) { return { vsubq_u16(a.v, b.v) }; }
    friend U16x8 atLeast(U16x8 a, U16x8 b) { return { vcgeq_u16(a.v, b.v) }; }
    unsigned firstSet() const {
        uint64_t bytes = vget_lane_u64(vreinterpret_u64_u8(vmovn_u16(v)), 0);
        return bytes ? (unsigned)std::countr_zero(bytes) / 8 : 8;
    }
};

struct Mask32 {
    uint64_t lo, hi;
    explicit operator bool() const { return (lo | hi) != 0; }
    unsigned first() const { return lo ? (unsigned)std::countr_zero(lo) / 4 : 16 + (unsigned)std::countr_zero(hi) / 4; }
    Mask32 operator>>(size_t n) const {
        size_t s = n * 4;
        if (s >= 64) return { hi >> (s - 64), 0 };
        return { s ? lo >> s | hi << (64 - s) : lo, s ? hi >> s : hi };
    }
};

struct U8x32 {
    uint8x16x2_t v;
    static U8x32 loadAligned(const void* p) { return { vld1q_u8_x2(static_cast<const uint8_t*>(p)) }; }
    static U8x32 splat(uint8_t b) { return { { vdupq_n_u8(b), vdupq_n_u8(b) } }; }
    friend U8x32 operator|(U8x32 a, U8x32 b) { return { { vorrq_u8(a.v.val[0], b.v.val[0]), vorrq_u8(a.v.val[1], b.v.val[1]) } }; }
    friend U8x32 operator==(U8x32 a, U8x32 b) { return { { vceqq_u8(a.v.val[0], b.v.val[0]), vceqq_u8(a.v.val[1], b.v.val[1]) } }; }
    Mask32 mask() const { return { nibbleMask(v.val[0]), nibbleMask(v.val[1]) }; }
};
#endif

}
