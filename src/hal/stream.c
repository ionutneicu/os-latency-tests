/*
 * Bulk read/write kernels, see olt/hal/stream.h. Every kernel keeps four
 * independent accumulators (reads) or two stores per iteration (writes), so
 * the loop overhead never limits it, and handles a 64-byte tail.
 */
#include "olt/hal/stream.h"

#include <pthread.h>

/* 16-byte compiler vectors: SSE2 on x86-64 (baseline), NEON on AArch64. */
typedef uint64_t olt_v2 __attribute__((vector_size(16), may_alias));

static uint64_t read_v16(const void* buf, size_t bytes, size_t passes)
{
    const olt_v2* v = (const olt_v2*)buf;
    const size_t n = bytes / 16;
    olt_v2 a0 = {0, 0}, a1 = {0, 0}, a2 = {0, 0}, a3 = {0, 0};
    for (size_t p = 0; p < passes; ++p) {
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            a0 += v[i];
            a1 += v[i + 1];
            a2 += v[i + 2];
            a3 += v[i + 3];
        }
        for (; i < n; ++i) a0 += v[i];
        __asm__ __volatile__("" ::: "memory");  /* keep passes from being merged */
    }
    const olt_v2 s = (a0 + a1) + (a2 + a3);
    return s[0] + s[1];
}

static void write_v16(void* buf, size_t bytes, size_t passes, uint64_t x)
{
    olt_v2* v = (olt_v2*)buf;
    const size_t n = bytes / 16;  /* even: bytes is a multiple of 64 */
    for (size_t p = 0; p < passes; ++p) {
        const olt_v2 val = {x + p, ~(x + p)};
        for (size_t i = 0; i < n; i += 2) {
            v[i] = val;
            v[i + 1] = val;
        }
        __asm__ __volatile__("" ::: "memory");
    }
}

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(OLT_FORCE_GENERIC_ARCH)
#include <immintrin.h>
#define OLT_STREAM_X86 1

__attribute__((target("avx2")))
static uint64_t read_avx2(const void* buf, size_t bytes, size_t passes)
{
    const char* b = (const char*)buf;
    __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
    for (size_t p = 0; p < passes; ++p) {
        size_t i = 0;
        for (; i + 128 <= bytes; i += 128) {
            a0 = _mm256_add_epi64(a0, _mm256_load_si256((const __m256i*)(b + i)));
            a1 = _mm256_add_epi64(a1, _mm256_load_si256((const __m256i*)(b + i + 32)));
            a2 = _mm256_add_epi64(a2, _mm256_load_si256((const __m256i*)(b + i + 64)));
            a3 = _mm256_add_epi64(a3, _mm256_load_si256((const __m256i*)(b + i + 96)));
        }
        for (; i < bytes; i += 32) a0 = _mm256_add_epi64(a0, _mm256_load_si256((const __m256i*)(b + i)));
        __asm__ __volatile__("" ::: "memory");
    }
    const __m256i s = _mm256_add_epi64(_mm256_add_epi64(a0, a1), _mm256_add_epi64(a2, a3));
    return (uint64_t)_mm256_extract_epi64(s, 0) + (uint64_t)_mm256_extract_epi64(s, 1) +
           (uint64_t)_mm256_extract_epi64(s, 2) + (uint64_t)_mm256_extract_epi64(s, 3);
}

__attribute__((target("avx2")))
static void write_avx2(void* buf, size_t bytes, size_t passes, uint64_t x)
{
    char* b = (char*)buf;
    for (size_t p = 0; p < passes; ++p) {
        const __m256i val = _mm256_set1_epi64x((long long)(x + p));
        for (size_t i = 0; i < bytes; i += 64) {
            _mm256_store_si256((__m256i*)(b + i), val);
            _mm256_store_si256((__m256i*)(b + i + 32), val);
        }
        __asm__ __volatile__("" ::: "memory");
    }
}

__attribute__((target("avx512f")))
static uint64_t read_avx512(const void* buf, size_t bytes, size_t passes)
{
    const char* b = (const char*)buf;
    __m512i a0 = _mm512_setzero_si512(), a1 = a0, a2 = a0, a3 = a0;
    for (size_t p = 0; p < passes; ++p) {
        size_t i = 0;
        for (; i + 256 <= bytes; i += 256) {
            a0 = _mm512_add_epi64(a0, _mm512_load_si512(b + i));
            a1 = _mm512_add_epi64(a1, _mm512_load_si512(b + i + 64));
            a2 = _mm512_add_epi64(a2, _mm512_load_si512(b + i + 128));
            a3 = _mm512_add_epi64(a3, _mm512_load_si512(b + i + 192));
        }
        for (; i < bytes; i += 64) a0 = _mm512_add_epi64(a0, _mm512_load_si512(b + i));
        __asm__ __volatile__("" ::: "memory");
    }
    return (uint64_t)_mm512_reduce_add_epi64(_mm512_add_epi64(_mm512_add_epi64(a0, a1), _mm512_add_epi64(a2, a3)));
}

__attribute__((target("avx512f")))
static void write_avx512(void* buf, size_t bytes, size_t passes, uint64_t x)
{
    char* b = (char*)buf;
    for (size_t p = 0; p < passes; ++p) {
        const __m512i val = _mm512_set1_epi64((long long)(x + p));
        size_t i = 0;
        for (; i + 128 <= bytes; i += 128) {
            _mm512_store_si512(b + i, val);
            _mm512_store_si512(b + i + 64, val);
        }
        for (; i < bytes; i += 64) _mm512_store_si512(b + i, val);
        __asm__ __volatile__("" ::: "memory");
    }
}
#endif

static struct {
    const char* isa;
    size_t width;
    uint64_t (*read)(const void*, size_t, size_t);
    void (*write)(void*, size_t, size_t, uint64_t);
} g_k;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void pick(void)
{
#if defined(OLT_STREAM_X86)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f")) {
        g_k.isa = "avx512"; g_k.width = 64; g_k.read = read_avx512; g_k.write = write_avx512;
        return;
    }
    if (__builtin_cpu_supports("avx2")) {
        g_k.isa = "avx2"; g_k.width = 32; g_k.read = read_avx2; g_k.write = write_avx2;
        return;
    }
    g_k.isa = "sse2";
#elif defined(__aarch64__) && !defined(OLT_FORCE_GENERIC_ARCH)
    g_k.isa = "neon";
#else
    g_k.isa = "generic";
#endif
    g_k.width = 16; g_k.read = read_v16; g_k.write = write_v16;
}

const char* olt_stream_isa(void) { pthread_once(&g_once, pick); return g_k.isa; }
size_t olt_stream_width(void) { pthread_once(&g_once, pick); return g_k.width; }

uint64_t olt_stream_read(const void* buf, size_t bytes, size_t passes)
{
    pthread_once(&g_once, pick);
    return g_k.read(buf, bytes, passes);
}

void olt_stream_write(void* buf, size_t bytes, size_t passes, uint64_t v)
{
    pthread_once(&g_once, pick);
    g_k.write(buf, bytes, passes, v);
}
