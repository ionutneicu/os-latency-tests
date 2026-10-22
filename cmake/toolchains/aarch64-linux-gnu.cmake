# Cross-compile for 64-bit ARM Linux with the GNU toolchain, e.g. on Debian/Ubuntu:
#   sudo apt install g++-aarch64-linux-gnu
#   cmake -S . -B build-arm64 -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake
# Override the prefix for other toolchains (Linaro, Yocto SDK, ...) with
# -DOLT_CROSS_PREFIX=/path/to/bin/aarch64-none-linux-gnu-
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(OLT_CROSS_PREFIX "aarch64-linux-gnu-" CACHE STRING "Cross toolchain prefix")
set(CMAKE_C_COMPILER   ${OLT_CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${OLT_CROSS_PREFIX}g++)

# Lets ctest run the target binaries under qemu-user when it is installed.
find_program(OLT_QEMU_AARCH64 qemu-aarch64)
if(OLT_QEMU_AARCH64)
  set(CMAKE_CROSSCOMPILING_EMULATOR ${OLT_QEMU_AARCH64} -L /usr/aarch64-linux-gnu)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
