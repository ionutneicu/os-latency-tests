project("os-latency-tests")
cmake_minimum_required(VERSION 3.10)

macro(use_cxx11)
  if (CMAKE_VERSION VERSION_LESS "3.1")
    if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++11")
    endif ()
  else ()
    set (CMAKE_CXX_STANDARD 11)
  endif ()
endmacro(use_cxx11)

use_cxx11()

add_executable (os-latency-tests src/os-latency-tests.cpp)
target_include_directories(os-latency-tests PUBLIC src)
target_link_libraries (os-latency-tests pthread)
