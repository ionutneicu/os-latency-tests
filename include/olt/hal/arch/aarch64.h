/*
 * olt/hal/arch/aarch64.h - AArch64 implementation of the arch HAL contract.
 * See olt/hal/arch.h for the contract.
 *
 * Notes:
 *  - olt_cycles() reads the generic timer (CNTVCT_EL0), which is always
 *    accessible from EL0 on Linux. Its frequency (CNTFRQ_EL0) is often much
 *    lower than the core clock (e.g. 19.2-100 MHz on many boards, 1 GHz on
 *    ARMv8.6+), so its resolution is reported alongside results. The PMU cycle
 *    counter (PMCCNTR_EL0) is not used because Linux disables EL0 access by
 *    default.
 *  - DC CIVAC is allowed from EL0 on Linux (SCTLR_EL1.UCI is set).
 */
#ifndef OLT_HAL_ARCH_AARCH64_H
#define OLT_HAL_ARCH_AARCH64_H

#include <stddef.h>
#include <stdint.h>

#define OLT_ARCH_AARCH64 1

static inline const char* olt_arch_name(void) { return "aarch64"; }

static inline uint64_t olt_cycles(void)
{
    uint64_t t;
    __asm__ __volatile__("isb\n\tmrs %0, cntvct_el0" : "=r"(t) :: "memory");
    return t;
}

static inline uint64_t olt_cycles_hz_hint(void)
{
    uint64_t f;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static inline void olt_prefetch_r(const void* p)     { __asm__ __volatile__("prfm pldl1keep, [%0]" :: "r"(p)); }
static inline void olt_prefetch_r_l2(const void* p)  { __asm__ __volatile__("prfm pldl2keep, [%0]" :: "r"(p)); }
static inline void olt_prefetch_r_l3(const void* p)  { __asm__ __volatile__("prfm pldl3keep, [%0]" :: "r"(p)); }
static inline void olt_prefetch_r_nta(const void* p) { __asm__ __volatile__("prfm pldl1strm, [%0]" :: "r"(p)); }
static inline void olt_prefetch_w(const void* p)     { __asm__ __volatile__("prfm pstl1keep, [%0]" :: "r"(p)); }

static inline void olt_compiler_barrier(void) { __asm__ __volatile__("" ::: "memory"); }
static inline void olt_mb(void)  { __asm__ __volatile__("dmb ish" ::: "memory"); }
static inline void olt_rmb(void) { __asm__ __volatile__("dmb ishld" ::: "memory"); }
static inline void olt_wmb(void) { __asm__ __volatile__("dmb ishst" ::: "memory"); }
static inline void olt_cpu_relax(void) { __asm__ __volatile__("yield" ::: "memory"); }
static inline void olt_opaque_u64(uint64_t* v) { __asm__ __volatile__("" : "+r"(*v)); }
static inline void olt_opaque_f64(double* v)   { __asm__ __volatile__("" : "+w"(*v)); }

static inline void olt_cache_flush_line(const void* p)
{
    __asm__ __volatile__("dc civac, %0\n\tdsb ish" :: "r"(p) : "memory");
}

static inline size_t olt_cache_line_size(void)
{
    uint64_t ctr;
    __asm__ __volatile__("mrs %0, ctr_el0" : "=r"(ctr));
    return (size_t)4 << ((ctr >> 16) & 0xf); /* DminLine: log2 of words */
}

#endif /* OLT_HAL_ARCH_AARCH64_H */
