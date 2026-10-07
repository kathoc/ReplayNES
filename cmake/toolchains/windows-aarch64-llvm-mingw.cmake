# Cross-compile for Windows aarch64 with llvm-mingw (UCRT). Usage:
#   cmake -B build/windows-aarch64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-aarch64-llvm-mingw.cmake
set(RN_MINGW_ARCH aarch64)
include(${CMAKE_CURRENT_LIST_DIR}/windows-llvm-mingw.cmake)
