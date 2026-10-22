/*
 * olt/hal/arch/generic.h - portable fallback for the arch HAL contract.
 * See olt/hal/arch.h for the contract.
 *
 * Uses only GCC/Clang builtins and the POSIX monotonic clock. Cache flush is a
 * no-op and the "cycle" counter is nanoseconds, so benchmarks relying on those
 * report reduced precision on such targets.
 */
#ifndef OLT_HAL_ARCH_GENERIC_H
#define OLT_HAL_ARCH_GENERIC_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define OLT_ARCH_GENERIC 1

static inline const char* olt_arch_name(void) { return "generic"; }

static inline uint64_t olt_cycles(void)
{
    struct timespec ts;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t olt_cycles_hz_hint(void) { return 1000000000ull; }

static inline void olt_prefetch_r(const void* p)     { __builtin_prefetch(p, 0, 3); }
static inline void olt_prefetch_r_l2(const void* p)  { __builtin_prefetch(p, 0, 2); }
static inline void olt_prefetch_r_l3(const void* p)  { __builtin_prefetch(p, 0, 1); }
static inline void olt_prefetch_r_nta(const void* p) { __builtin_prefetch(p, 0, 0); }
static inline void olt_prefetch_w(const void* p)     { __builtin_prefetch(p, 1, 3); }

static inline void olt_compiler_barrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void olt_mb(void)  { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void olt_rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void olt_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline void olt_cpu_relax(void) { olt_compiler_barrier(); }
/* Generic: integers stay in registers, doubles may round-trip through memory. */
static inline void olt_opaque_u64(uint64_t* v) { __asm__ __volatile__("" : "+r"(*v)); }
static inline void olt_opaque_f64(double* v)   { __asm__ __volatile__("" : "+m"(*v)); }

/* Generic: no cache-bypass hint, a plain store. */
static inline void olt_stream_store_16(void* p, uint64_t lo, uint64_t hi)
{
    uint64_t* q = (uint64_t*)p;
    q[0] = lo;
    q[1] = hi;
}
static inline void olt_stream_fence(void) { olt_wmb(); }

static inline void olt_cache_flush_line(const void* p) { (void)p; olt_mb(); }

static inline size_t olt_cache_line_size(void) { return 64; }

#endif /* OLT_HAL_ARCH_GENERIC_H */
