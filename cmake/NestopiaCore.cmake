# Builds the Nestopia UE emulation core (third_party/nestopia/source/core) as a static library.
# Deterministic, dependency-free configuration: no zlib (uncompressed states), no optional
# upscaling filters. The submodule is used unmodified; see docs/COMPATIBILITY.md.
set(NST_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party/nestopia/source)
if(NOT EXISTS ${NST_ROOT}/core/NstMachine.cpp)
  message(FATAL_ERROR "Nestopia submodule missing: run `git submodule update --init`")
endif()

file(GLOB NST_CORE_SOURCES CONFIGURE_DEPENDS
  ${NST_ROOT}/core/*.cpp
  ${NST_ROOT}/core/api/*.cpp
  ${NST_ROOT}/core/board/*.cpp
  ${NST_ROOT}/core/input/*.cpp
  ${NST_ROOT}/core/vssystem/*.cpp)
# Optional upscaling filters are disabled via NST_NO_* below; their sources are not self-guarded.
list(FILTER NST_CORE_SOURCES EXCLUDE REGEX "NstVideoFilter(xBR|HqX|ScaleX|2xSaI)\\.cpp$")
include(${CMAKE_CURRENT_LIST_DIR}/NestopiaPatches.cmake)

add_library(nestopia_core STATIC ${NST_CORE_SOURCES})
target_include_directories(nestopia_core PUBLIC ${NST_ROOT})
# Patched copies live in the build tree; their relative includes resolve against the originals.
target_include_directories(nestopia_core PRIVATE ${NST_ROOT}/core)
target_compile_definitions(nestopia_core PUBLIC
  NST_NO_ZLIB NST_NO_HQ2X NST_NO_SCALEX NST_NO_2XSAI NST_NO_XBR)
set_target_properties(nestopia_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(nestopia_core PRIVATE /W0 /fp:precise)
else()
  # Third-party code: silence warnings; never allow value-changing float optimizations.
  target_compile_options(nestopia_core PRIVATE -w -fno-fast-math -ffp-contract=off)
endif()

# ---- Core revision pin -----------------------------------------------------------------
# The core compatibility ID (docs/COMPATIBILITY.md) embeds this commit. Building against a
# different core revision would silently produce projects that claim the wrong core, so the
# configure step verifies the checked-out submodule matches the pin.
set(REPLAYNES_NESTOPIA_COMMIT "7b5c87d8dc3c817211545449683d1fcd897324ca")
option(REPLAYNES_SKIP_CORE_PIN_CHECK "Do not verify the Nestopia submodule commit (source tarballs without .git)" OFF)
find_package(Git QUIET)
if(NOT REPLAYNES_SKIP_CORE_PIN_CHECK)
  if(NOT GIT_FOUND)
    message(FATAL_ERROR "git not found; cannot verify the Nestopia pin (set REPLAYNES_SKIP_CORE_PIN_CHECK=ON for tarball builds)")
  endif()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${NST_ROOT}/.. rev-parse HEAD
    OUTPUT_VARIABLE _nst_head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE _nst_rc ERROR_QUIET)
  if(NOT _nst_rc EQUAL 0 OR NOT _nst_head STREQUAL REPLAYNES_NESTOPIA_COMMIT)
    message(FATAL_ERROR "Nestopia submodule is at '${_nst_head}', expected pinned ${REPLAYNES_NESTOPIA_COMMIT}. "
      "Changing the core changes the core compatibility ID: update the pin + docs/COMPATIBILITY.md deliberately.")
  endif()
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${NST_ROOT}/.. status --porcelain --untracked-files=no
    OUTPUT_VARIABLE _nst_dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _nst_dirty STREQUAL "")
    message(FATAL_ERROR "Nestopia submodule has local modifications; the core must be the pinned upstream source.")
  endif()
endif()
