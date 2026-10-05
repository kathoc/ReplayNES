# Portable ReplayNES engine (C++17, no platform UI APIs). Exposes the C API in include/replaynes.
file(GLOB_RECURSE RN_ENGINE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(replaynes_engine STATIC ${RN_ENGINE_SOURCES})
target_include_directories(replaynes_engine
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  # Internal headers are visible to tests and the CLI; frontends must only use include/.
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(replaynes_engine PUBLIC nestopia_core)
target_compile_definitions(replaynes_engine PRIVATE
  RN_NESTOPIA_COMMIT="${REPLAYNES_NESTOPIA_COMMIT}"
  RN_NESTOPIA_PATCHLEVEL=${REPLAYNES_NESTOPIA_PATCHLEVEL}
  RN_ENGINE_VERSION="${PROJECT_VERSION}")
set_target_properties(replaynes_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(replaynes_engine PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(replaynes_engine PRIVATE /W4 /fp:precise)
else()
  target_compile_options(replaynes_engine PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-fast-math -ffp-contract=off)
endif()
