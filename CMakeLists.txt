cmake_minimum_required(VERSION 3.16)
project(os-latency-tests VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---- optimisation guard -------------------------------------------------------
# Unoptimised code gives numbers that measure the compiler's stack traffic, not
# the hardware (issue #12). Two ways it happens:
# - CMake first ran before a compiler was installed. That configure fails but
#   caches the compiler's build-type flags (CMAKE_<LANG>_FLAGS_<CONFIG>) empty,
#   and every later configure keeps them, so that language builds at -O0.
#   The defaults are put back, unless the general flags set a level of their own.
# - The build type or the flags have no optimisation level at all: warn.

function(olt_last_opt_level out flags)
  string(REGEX MATCHALL " -O[^ ]*" levels " ${flags}")
  set(level "")
  if(levels)
    list(GET levels -1 level)
    string(STRIP "${level}" level)
  endif()
  set(${out} "${level}" PARENT_SCOPE)
endfunction()

function(olt_check_optimisation)
  set(repaired "")
  set(unoptimised "")
  foreach(lang C CXX)
    olt_last_opt_level(general "${CMAKE_${lang}_FLAGS}")
    foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
      set(var CMAKE_${lang}_FLAGS_${cfg})
      string(STRIP "${CMAKE_${lang}_FLAGS_${cfg}_INIT}" init)
      if("${${var}}" STREQUAL "" AND NOT init STREQUAL "" AND general STREQUAL "")
        set(${var} "${init}" CACHE STRING "Flags used by the ${lang} compiler during ${cfg} builds." FORCE)
        set(${var} "${init}")
        list(APPEND repaired "${var}='${init}'")
      endif()
    endforeach()
    if(CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${CMAKE_BUILD_TYPE}" cfg)
      string(STRIP "${CMAKE_${lang}_FLAGS} ${CMAKE_${lang}_FLAGS_${cfg}}" flags)
      olt_last_opt_level(level "${flags}")
      if(level STREQUAL "" OR level STREQUAL "-O0")
        string(REPLACE "CXX" "C++" name "${lang}")
        list(APPEND unoptimised "${name} (flags '${flags}')")
      endif()
    endif()
  endforeach()
  if(repaired)
    string(REPLACE ";" ", " repaired "${repaired}")
    message(WARNING "Optimisation flags were cached empty, probably because CMake first ran before the "
                    "compiler was installed. Restored the defaults: ${repaired}.")
  endif()
  if(unoptimised)
    string(REPLACE ";" " and " unoptimised "${unoptimised}")
    message(WARNING "The ${unoptimised} code builds without optimisation (build type "
                    "${CMAKE_BUILD_TYPE}). Benchmark numbers from such a build are wrong: use "
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo or Release, without -O0 in the flags.")
  endif()
endfunction()
olt_check_optimisation()

# ---- feature options ---------------------------------------------------------
# Optional features follow one pattern: OLT_WITH_<FEATURE> = AUTO | ON | OFF.
# AUTO enables the feature when its dependencies are found, ON fails the
# configure step if they are missing, OFF builds a stub that reports the
# feature as unavailable at run time.

set(OLT_WITH_PERF AUTO CACHE STRING "Hardware/software perf counters via perf_event_open (AUTO|ON|OFF)")
set_property(CACHE OLT_WITH_PERF PROPERTY STRINGS AUTO ON OFF)
option(OLT_FORCE_GENERIC_ARCH "Use the portable compiler-builtin arch HAL instead of the native one" OFF)
option(OLT_BUILD_TESTS "Build unit tests" ON)

include(CheckIncludeFile)

# ---- HAL selection ------------------------------------------------------------

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(OLT_OS_SOURCES src/hal/linux/os_linux.c)
else()
  message(FATAL_ERROR "No OS HAL for ${CMAKE_SYSTEM_NAME} yet: implement include/olt/hal/os.h in src/hal/<os>/")
endif()

set(OLT_PERF_ENABLED OFF)
if(NOT OLT_WITH_PERF STREQUAL "OFF")
  check_include_file("linux/perf_event.h" OLT_HAVE_PERF_EVENT_H)
  if(OLT_HAVE_PERF_EVENT_H)
    set(OLT_PERF_ENABLED ON)
  elseif(OLT_WITH_PERF STREQUAL "ON")
    message(FATAL_ERROR "OLT_WITH_PERF=ON but <linux/perf_event.h> was not found (install linux-libc-dev)")
  endif()
endif()
if(OLT_PERF_ENABLED)
  set(OLT_PERF_SOURCES src/hal/linux/perf_linux.c)
else()
  set(OLT_PERF_SOURCES src/hal/stub/perf_stub.c)
endif()

find_package(Threads REQUIRED)

# ---- core library -------------------------------------------------------------

add_library(olt_core STATIC
  ${OLT_OS_SOURCES}
  ${OLT_PERF_SOURCES}
  src/hal/stream.c
  src/core/benchmark.cpp
  src/core/clock.cpp
  src/core/platform.cpp
  src/core/preflight.cpp
  src/core/report.cpp
  src/core/stats.cpp
)
target_include_directories(olt_core PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The git commit of the sources, refreshed at every build (cmake/git_commit.cmake)
add_custom_target(olt_git_commit
  COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/olt_git.h"
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/git_commit.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/generated/olt_git.h
  COMMENT "Reading the git commit")
add_dependencies(olt_core olt_git_commit)
target_link_libraries(olt_core PUBLIC Threads::Threads)
target_compile_definitions(olt_core PUBLIC
  OLT_VERSION="${PROJECT_VERSION}"
  OLT_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  OLT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
  $<$<BOOL:${OLT_FORCE_GENERIC_ARCH}>:OLT_FORCE_GENERIC_ARCH>
)
target_compile_options(olt_core PUBLIC
  $<$<OR:$<C_COMPILER_ID:GNU>,$<C_COMPILER_ID:Clang>>:-Wall -Wextra>
)

# ---- benchmark executable -----------------------------------------------------
# Benchmarks self-register through static objects, so they are compiled into
# the executable directly rather than into a static library (where unreferenced
# objects would be dropped by the linker).

add_executable(os-latency-tests
  src/main.cpp
  src/benchmarks/alloc.cpp
  src/benchmarks/atomic.cpp
  src/benchmarks/branch.cpp
  src/benchmarks/ilp.cpp
  src/benchmarks/memory.cpp
  src/benchmarks/multicore.cpp
  src/benchmarks/pattern_alloc.cpp
  src/benchmarks/pattern_dispatch.cpp
  src/benchmarks/pattern_spsc.cpp
  src/benchmarks/sync.cpp
  src/benchmarks/threads.cpp
)
target_link_libraries(os-latency-tests PRIVATE olt_core)
# cpu.ilp measures how many independent chains the core overlaps, and
# cpu.branch compares scalar branchy and branchless code: the compiler must not
# turn either into SIMD instructions.
set_source_files_properties(src/benchmarks/ilp.cpp src/benchmarks/branch.cpp PROPERTIES COMPILE_OPTIONS
  "$<$<CXX_COMPILER_ID:GNU>:-fno-tree-vectorize>;$<$<CXX_COMPILER_ID:Clang,AppleClang>:-fno-vectorize;-fno-slp-vectorize>")

include(GNUInstallDirs)
install(TARGETS os-latency-tests RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS tools/olt-report.py DESTINATION ${CMAKE_INSTALL_BINDIR} RENAME olt-report)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---- tests --------------------------------------------------------------------

if(OLT_BUILD_TESTS)
  enable_testing()
  add_executable(olt_unit_tests tests/unit_tests.cpp)
  target_link_libraries(olt_unit_tests PRIVATE olt_core)
  add_test(NAME unit_tests COMMAND olt_unit_tests)
  # End-to-end smoke run with few samples
  add_test(NAME smoke_run COMMAND os-latency-tests -n 200 -w 10 --park-us 20 --mem-max 1M --sweep-samples 3
                                  -o ${CMAKE_CURRENT_BINARY_DIR}/smoke-results)
  # Flags cached empty by a configure that ran before the compiler was installed get repaired (#12)
  add_test(NAME cmake_flag_repair
           COMMAND ${CMAKE_COMMAND} "-DSRC=${CMAKE_CURRENT_SOURCE_DIR}" "-DBIN=${CMAKE_CURRENT_BINARY_DIR}/flag-repair"
                   "-DGEN=${CMAKE_GENERATOR}" "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}"
                   "-DTOOLCHAIN=${CMAKE_TOOLCHAIN_FILE}" -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_flag_repair.cmake)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME report COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/olt-report.py
                                 ${CMAKE_CURRENT_BINARY_DIR}/smoke-results/results.json)
    set_tests_properties(report PROPERTIES DEPENDS smoke_run)
  endif()
endif()

if(OLT_FORCE_GENERIC_ARCH)
  set(OLT_ARCH_LABEL "generic")
else()
  set(OLT_ARCH_LABEL "${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "os-latency-tests ${PROJECT_VERSION}: arch HAL ${OLT_ARCH_LABEL}, perf counters ${OLT_PERF_ENABLED}")
