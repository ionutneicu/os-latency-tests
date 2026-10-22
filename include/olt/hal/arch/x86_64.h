/*
 * olt/hal/arch/x86_64.h - x86-64 implementation of the arch HAL contract.
 * See olt/hal/arch.h for the contract.
 */
#ifndef OLT_HAL_ARCH_X86_64_H
#define OLT_HAL_ARCH_X86_64_H

#include <stddef.h>
#include <stdint.h>
#include <x86intrin.h>

#define OLT_ARCH_X86_64 1

static inline const char* olt_arch_name(void) { return "x86_64"; }

/* lfence before rdtsc waits for earlier instructions to complete, lfence after
 * keeps later instructions from starting before the counter is read. */
static inline uint64_t olt_cycles(void)
{
    _mm_lfence();
    uint64_t t = __rdtsc();
    _mm_lfence();
    return t;
}

/* The TSC frequency is not architecturally discoverable from user space;
 * the clock module calibrates it against the OS monotonic clock. */
static inline uint64_t olt_cycles_hz_hint(void) { return 0; }

static inline void olt_prefetch_r(const void* p)     { _mm_prefetch((const char*)p, _MM_HINT_T0); }
static inline void olt_prefetch_r_l2(const void* p)  { _mm_prefetch((const char*)p, _MM_HINT_T1); }
static inline void olt_prefetch_r_l3(const void* p)  { _mm_prefetch((const char*)p, _MM_HINT_T2); }
static inline void olt_prefetch_r_nta(const void* p) { _mm_prefetch((const char*)p, _MM_HINT_NTA); }
/* prefetchw where supported; older CPUs treat it as a NOP */
static inline void olt_prefetch_w(const void* p)     { __builtin_prefetch(p, 1, 3); }

static inline void olt_compiler_barrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void olt_mb(void)  { _mm_mfence(); }
/* x86 TSO already orders load-load and store-store for normal memory */
static inline void olt_rmb(void) { olt_compiler_barrier(); }
static inline void olt_wmb(void) { olt_compiler_barrier(); }
static inline void olt_cpu_relax(void) { _mm_pause(); }

static inline void olt_cache_flush_line(const void* p)
{
    _mm_clflush(p);
    _mm_mfence();
}

static inline size_t olt_cache_line_size(void)
{
    unsigned int eax = 1, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
    size_t line = ((ebx >> 8) & 0xff) * 8; /* CLFLUSH line size, in 8-byte units */
    return line ? line : 64;
}

#endif /* OLT_HAL_ARCH_X86_64_H */
