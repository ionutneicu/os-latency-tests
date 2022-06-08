add_executable (os-latency-tests src/os-latency-tests.cpp)
target_include_directories(os-latency-tests PUBLIC src)
target_link_libraries (os-latency-tests pthread)
