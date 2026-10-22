/*
 * olt/hal/arch.h - CPU architecture abstraction layer.
 *
 * Every non-portable, instruction-level primitive used by the benchmarks lives
 * behind this header. Each architecture provides the same set of static inline
 * functions in its own header; this file only selects one.
 *
 * Contract (all functions are static inline, C and C++ compatible):
 *
 *   const char* olt_arch_name(void);
 *
 *   uint64_t olt_cycles(void);          ordered read of the CPU timestamp counter
 *   uint64_t olt_cycles_hz_hint(void);  counter frequency if architecturally known, else 0
 *
 *   void olt_prefetch_r(const void*);      prefetch for read, keep in all cache levels (L1)
 *   void olt_prefetch_r_l2(const void*);   prefetch for read into L2 and outer levels
 *   void olt_prefetch_r_l3(const void*);   prefetch for read into L3/LLC
 *   void olt_prefetch_r_nta(const void*);  prefetch for read, streaming / non-temporal
 *   void olt_prefetch_w(const void*);      prefetch for write (exclusive ownership)
 *
 *   void olt_compiler_barrier(void);
 *   void olt_mb(void);                  full memory barrier
 *   void olt_rmb(void);                 load-load barrier
 *   void olt_wmb(void);                 store-store barrier
 *   void olt_cpu_relax(void);           spin-wait hint (pause / yield)
 *
 *   void   olt_cache_flush_line(const void*);  write back + invalidate the line holding the address
 *   size_t olt_cache_line_size(void);          data cache line size in bytes
 *
 * To port to a new architecture, add olt/hal/arch/<arch>.h implementing the
 * contract above and add a branch below. arch/generic.h is a compiler-builtin
 * fallback that works anywhere GCC/Clang do, with reduced precision.
 */
#ifndef OLT_HAL_ARCH_H
#define OLT_HAL_ARCH_H

#include <stddef.h>
#include <stdint.h>

#if defined(OLT_FORCE_GENERIC_ARCH)
#include "arch/generic.h"
#elif defined(__x86_64__) || defined(_M_X64)
#include "arch/x86_64.h"
#elif defined(__aarch64__) || defined(_M_ARM64)
#include "arch/aarch64.h"
#else
#include "arch/generic.h"
#endif

#endif /* OLT_HAL_ARCH_H */
