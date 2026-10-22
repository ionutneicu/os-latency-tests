# Regression test for issue #12. A configure that runs before the C++ compiler
# is installed caches CMAKE_CXX_FLAGS_RELWITHDEBINFO empty, and later configures
# kept it, so the C++ code built at -O0 without a word. CMakeLists.txt must put
# the default back and warn.
#
#   cmake -DSRC=<source dir> -DBIN=<scratch build dir> -DGEN=<generator>
#         -DCC=<C compiler> -DCXX=<C++ compiler> [-DTOOLCHAIN=<file>] -P cmake_flag_repair.cmake

file(REMOVE_RECURSE "${BIN}")
set(args -S "${SRC}" -B "${BIN}" -G "${GEN}" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOLT_BUILD_TESTS=OFF
         "-DCMAKE_C_COMPILER=${CC}" "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=")
if(TOOLCHAIN)
  list(APPEND args "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${args} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "configure failed (${rc}):\n${out}\n${err}")
endif()

file(STRINGS "${BIN}/CMakeCache.txt" cached REGEX "^CMAKE_CXX_FLAGS_RELWITHDEBINFO:")
if(NOT cached MATCHES "=.*-O")
  message(FATAL_ERROR "empty cached flags were not repaired: ${cached}")
endif()
if(NOT err MATCHES "cached empty")
  message(FATAL_ERROR "no warning about the repaired flags:\n${err}")
endif()
message(STATUS "repaired: ${cached}")
file(REMOVE_RECURSE "${BIN}")
