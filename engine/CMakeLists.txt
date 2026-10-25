# Portable ReplayNES engine (C++17, no platform UI APIs). Exposes the C API in include/replaynes.
file(GLOB_RECURSE RN_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER RN_ENGINE_SOURCES EXCLUDE REGEX "/ReplayNEScoreAdapter\\.cpp$")
if(NOT RN_PRODUCT_NES)
  list(FILTER RN_ENGINE_SOURCES EXCLUDE REGEX "/NestopiaCore\\.cpp$")
endif()
add_library(replaynes_engine STATIC ${RN_ENGINE_SOURCES})
target_include_directories(replaynes_engine
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PUBLIC ${CMAKE_BINARY_DIR}/generated
  # Internal headers are visible to tests and the CLI; frontends must only use include/.
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
if(RN_PRODUCT_NES)
  target_link_libraries(replaynes_engine PUBLIC nestopia_core)
  target_compile_definitions(replaynes_engine PRIVATE
    RN_NESTOPIA_COMMIT="${REPLAYNES_NESTOPIA_COMMIT}"
    RN_NESTOPIA_PATCHLEVEL=${REPLAYNES_NESTOPIA_PATCHLEVEL})
endif()
target_compile_definitions(replaynes_engine PRIVATE RN_ENGINE_VERSION="${PROJECT_VERSION}")
set_target_properties(replaynes_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(replaynes_engine PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(replaynes_engine PRIVATE /W4 /fp:precise)
else()
  target_compile_options(replaynes_engine PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-fast-math -ffp-contract=off)
endif()

option(REPLAYNES_ENABLE_REPLAYNESCORE "Build experimental ReplayNEScore adapter" OFF)
set(REPLAYNESCORE_SOURCE_DIR "" CACHE PATH "ReplayNEScore source checkout")
if(REPLAYNES_ENABLE_REPLAYNESCORE)
  if(NOT EXISTS "${REPLAYNESCORE_SOURCE_DIR}/include/replaynes/nrom_cpu_ppu.hpp")
    message(FATAL_ERROR "Set REPLAYNESCORE_SOURCE_DIR to the independent core checkout")
  endif()
  # Keep its C++20 flags, test registration and PIC policy inside this subtree.
  function(rn_add_experimental_core)
    set(BUILD_TESTING OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${REPLAYNESCORE_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/experimental-core" EXCLUDE_FROM_ALL)
  endfunction()
  rn_add_experimental_core()
  add_library(replaynes_experimental_adapter STATIC src/core/ReplayNEScoreAdapter.cpp)
  target_include_directories(replaynes_experimental_adapter PRIVATE src)
  # The adapter calls engine code (stateenv); the cycle lets CMake repeat both archives at link time.
  target_link_libraries(replaynes_experimental_adapter PRIVATE replaynes_nrom_cpu_ppu replaynes_nrom_cpu_ppu_snapshot replaynes_engine)
  target_compile_features(replaynes_experimental_adapter PRIVATE cxx_std_20)
  set_target_properties(replaynes_experimental_adapter PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(replaynes_engine PRIVATE "$<LINK_ONLY:replaynes_experimental_adapter>")
  target_compile_definitions(replaynes_engine PUBLIC REPLAYNES_HAS_REPLAYNESCORE=1)
endif()
