# Enforces docs/REQUIREMENTS.md section 1: intrinsics, inline assembly and
# compiler builtins live only in the HAL (include/olt/hal/). Outside it, the
# only assembly allowed is the empty "keep this value opaque" statement, which
# has no standard C++ equivalent (benchmark idiom, as in Google Benchmark's
# DoNotOptimize). Any other match fails the test.
#
#   cmake -DSRC=<source dir> -P hal_scope.cmake

set(scanned "")
foreach(dir src/benchmarks src/core include/olt)
  file(GLOB_RECURSE files RELATIVE "${SRC}" "${SRC}/${dir}/*.cpp" "${SRC}/${dir}/*.hpp" "${SRC}/${dir}/*.h" "${SRC}/${dir}/*.c")
  list(APPEND scanned ${files})
endforeach()
list(FILTER scanned EXCLUDE REGEX "^include/olt/hal/")

set(violations "")
set(opaque_ok "^[ \t]*asm volatile\\(\"\"[ \t]*:")
foreach(f ${scanned})
  file(STRINGS "${SRC}/${f}" lines)
  set(n 0)
  foreach(line IN LISTS lines)
    math(EXPR n "${n} + 1")
    if(line MATCHES "__asm__|asm volatile|asm[ \t]*\\(|_mm[0-9]*_|__builtin_")
      if(NOT line MATCHES "${opaque_ok}")
        list(APPEND violations "${f}:${n}: ${line}")
      endif()
    endif()
  endforeach()
endforeach()

list(LENGTH violations count)
if(count GREATER 0)
  string(REPLACE ";" "\n  " text "${violations}")
  message(FATAL_ERROR "Intrinsics, assembly or builtins outside include/olt/hal/ (docs/REQUIREMENTS.md, section 1):\n  ${text}")
endif()
message(STATUS "hal_scope: ${count} violations in ${scanned}")
