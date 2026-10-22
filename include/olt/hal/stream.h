/*
 * olt/hal/stream.h - bulk read/write kernels at the widest vector width the
 * running CPU supports, so bandwidth sweeps measure the memory level and not
 * the instruction width.
 *
 * Contract:
 *
 *   const char* olt_stream_isa(void);   instruction set the kernels use:
 *                                       "avx512", "avx2", "sse2", "neon" or "generic"
 *   size_t olt_stream_width(void);      bytes per vector load/store
 *
 *   uint64_t olt_stream_read(const void* buf, size_t bytes, size_t passes);
 *       reads buf[0, bytes) `passes` times with vector loads; returns a sum of
 *       the data so the loads cannot be optimised away
 *   void olt_stream_write(void* buf, size_t bytes, size_t passes, uint64_t v);
 *       writes buf[0, bytes) `passes` times with vector stores of a value
 *       derived from v and the pass number
 *   int olt_stream_optimized(void);     1 if the kernels were compiled with
 *                                       optimisation, 0 if not, -1 if unknown
 *
 * buf must be 64-byte aligned and bytes a non-zero multiple of 64. The
 * implementation (src/hal/stream.c) picks the kernels once, at first use:
 * on x86-64 by CPUID (AVX-512F, then AVX2, else SSE2), elsewhere 16-byte
 * compiler vectors (NEON on AArch64).
 */
#ifndef OLT_HAL_STREAM_H
#define OLT_HAL_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char* olt_stream_isa(void);
size_t olt_stream_width(void);
uint64_t olt_stream_read(const void* buf, size_t bytes, size_t passes);
void olt_stream_write(void* buf, size_t bytes, size_t passes, uint64_t v);
int olt_stream_optimized(void);

#ifdef __cplusplus
}
#endif

#endif /* OLT_HAL_STREAM_H */
