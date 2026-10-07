# Shared part of the llvm-mingw cross toolchains (windows-<arch>-llvm-mingw.cmake set
# RN_MINGW_ARCH and include this file). llvm-mingw: https://github.com/mstorsjo/llvm-mingw
# Toolchain root: $LLVM_MINGW, else ~/.local/opt/llvm-mingw (see docs/WINDOWS.md).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR ${RN_MINGW_ARCH})

if(DEFINED ENV{LLVM_MINGW})
  set(RN_LLVM_MINGW_ROOT $ENV{LLVM_MINGW})
else()
  set(RN_LLVM_MINGW_ROOT $ENV{HOME}/.local/opt/llvm-mingw)
endif()
set(RN_LLVM_MINGW_ROOT ${RN_LLVM_MINGW_ROOT} CACHE PATH "llvm-mingw toolchain root")
set(_rn_triple ${RN_MINGW_ARCH}-w64-mingw32)
if(NOT EXISTS ${RN_LLVM_MINGW_ROOT}/bin/${_rn_triple}-clang++)
  message(FATAL_ERROR "llvm-mingw not found at ${RN_LLVM_MINGW_ROOT} (set LLVM_MINGW)")
endif()

set(CMAKE_C_COMPILER ${RN_LLVM_MINGW_ROOT}/bin/${_rn_triple}-clang)
set(CMAKE_CXX_COMPILER ${RN_LLVM_MINGW_ROOT}/bin/${_rn_triple}-clang++)
set(CMAKE_RC_COMPILER ${RN_LLVM_MINGW_ROOT}/bin/${_rn_triple}-windres)
set(CMAKE_AR ${RN_LLVM_MINGW_ROOT}/bin/llvm-ar CACHE FILEPATH "")
set(CMAKE_RANLIB ${RN_LLVM_MINGW_ROOT}/bin/llvm-ranlib CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH ${RN_LLVM_MINGW_ROOT}/${_rn_triple})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Self-contained executables: no libc++.dll / libunwind.dll to ship next to them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
