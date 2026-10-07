# Cross-compile for Windows x86_64 with llvm-mingw (UCRT). Usage:
#   cmake -B build/windows-x86_64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-x86_64-llvm-mingw.cmake
set(RN_MINGW_ARCH x86_64)
include(${CMAKE_CURRENT_LIST_DIR}/windows-llvm-mingw.cmake)
