# Native part of the Android app: the portable ReplayNES engine (+ Nestopia core) from the repo's
# top-level CMake project, and the JNI shim libreplaynes_android.so (emulation thread, ANativeWindow
# video, AAudio audio).
cmake_minimum_required(VERSION 3.16)
project(replaynes_android LANGUAGES C CXX)

get_filename_component(REPLAYNES_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../.." ABSOLUTE)

# Engine only: no tests, CLI, shared frontend core (needs Python), desktop or Windows targets.
set(REPLAYNES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_FRONTEND OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_DESKTOP OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_WINDOWS_APP OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_WINDOWS_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory("${REPLAYNES_ROOT}" replaynes EXCLUDE_FROM_ALL)


add_library(replaynes_android SHARED replaynes_jni.cpp)
target_compile_options(replaynes_android PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(replaynes_android PRIVATE replaynes_engine android aaudio log)
target_link_options(replaynes_android PRIVATE "-Wl,-z,max-page-size=16384")

# Real-time emulation needs optimized code. app/build.gradle.kts passes CMAKE_BUILD_TYPE=Release
# for every variant; should a Debug configuration still get here, optimize the engine anyway
# (target options come after the per-config flags, so they win over -O0).
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  foreach(t nestopia_core replaynes_engine replaynes_android)
    target_compile_options(${t} PRIVATE -O2)
  endforeach()
endif()
