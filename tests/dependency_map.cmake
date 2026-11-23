# Keeps docs/DEPENDENCIES.md complete: every registered benchmark and every
# shared helper (src/benchmarks/*_util.hpp) must be named in the map, and the
# map must not name a helper that does not exist, unless its line says "planned".
# Coverage only: whether a link is still correct is part of reviewing a change.
#
#   cmake -DSRC=<source dir> -P dependency_map.cmake

set(map_file "${SRC}/docs/DEPENDENCIES.md")
if(NOT EXISTS "${map_file}")
  message(FATAL_ERROR "docs/DEPENDENCIES.md is missing")
endif()
file(READ "${map_file}" map)

set(problems "")

# 1. every OLT_BENCHMARK("name", ...) appears as `name`
file(GLOB sources "${SRC}/src/benchmarks/*.cpp")
set(benchmarks "")
foreach(f ${sources})
  file(READ "${f}" text)
  string(REGEX MATCHALL "OLT_BENCHMARK\\([ \t\r\n]*\"[^\"]+\"" hits "${text}")
  foreach(h ${hits})
    string(REGEX REPLACE ".*\"([^\"]+)\"" "\\1" name "${h}")
    list(APPEND benchmarks "${name}")
    string(FIND "${map}" "`${name}`" at)
    if(at EQUAL -1)
      get_filename_component(short "${f}" NAME)
      list(APPEND problems "benchmark `${name}` (${short}) is not in the map")
    endif()
  endforeach()
endforeach()

# 2. every shared helper appears as `file_util.hpp`
file(GLOB helpers RELATIVE "${SRC}/src/benchmarks" "${SRC}/src/benchmarks/*_util.hpp")
foreach(h ${helpers})
  string(FIND "${map}" "`${h}`" at)
  if(at EQUAL -1)
    list(APPEND problems "shared helper `${h}` is not in the map")
  endif()
endforeach()

# 3. a helper named in the map exists, or its line says "planned"
file(STRINGS "${map_file}" lines)
foreach(line IN LISTS lines)
  string(REGEX MATCHALL "`[a-z0-9_]+_util\\.hpp`" named "${line}")
  foreach(n ${named})
    string(REPLACE "`" "" file "${n}")
    if(NOT EXISTS "${SRC}/src/benchmarks/${file}" AND NOT line MATCHES "planned")
      list(APPEND problems "the map names `${file}`, which does not exist and is not marked planned")
    endif()
  endforeach()
endforeach()

list(REMOVE_DUPLICATES problems)
list(LENGTH problems count)
if(count GREATER 0)
  string(REPLACE ";" "\n  " text "${problems}")
  message(FATAL_ERROR "docs/DEPENDENCIES.md is out of date:\n  ${text}")
endif()
list(LENGTH benchmarks nb)
list(LENGTH helpers nh)
message(STATUS "dependency_map: ${nb} benchmarks and ${nh} shared helpers are in the map")
