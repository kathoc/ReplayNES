# Shared frontend core (C++17, no platform UI / device APIs): static library replaynes_frontend
# with the C API in include/replaynes/frontend.h, used by the macOS app (Swift, via the bridging
# header), the Linux frontend and future ones. See docs/PORTING.md.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# Localization: apps/macos/Resources/Localizable.xcstrings is the single source of truth; the
# table compiled into this library (and l10n.json for tooling) is generated from it.
set(RNF_XCSTRINGS ${PROJECT_SOURCE_DIR}/apps/macos/Resources/Localizable.xcstrings)
set(RNF_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_custom_command(
  OUTPUT ${RNF_GEN_DIR}/l10n_table.inc ${RNF_GEN_DIR}/l10n.json
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_l10n.py ${RNF_XCSTRINGS}
          ${RNF_GEN_DIR}/l10n_table.inc --json ${RNF_GEN_DIR}/l10n.json
  DEPENDS ${RNF_XCSTRINGS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_l10n.py
  COMMENT "Generating the localization table from Localizable.xcstrings"
  VERBATIM)

file(GLOB RNF_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_library(replaynes_frontend STATIC ${RNF_SOURCES} ${RNF_GEN_DIR}/l10n_table.inc)
target_include_directories(replaynes_frontend
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
         # frontend.h includes "replaynes.h" like the frontends' bridging headers do.
         ${PROJECT_SOURCE_DIR}/engine/include/replaynes
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${RNF_GEN_DIR})
# Uses the engine's C API plus its internal JSON / file helpers (engine/src/util).
target_link_libraries(replaynes_frontend PUBLIC replaynes_engine)
set_target_properties(replaynes_frontend PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(replaynes_frontend PRIVATE /W4 /fp:precise /utf-8)
else()
  # No FMA contraction: the pacing / geometry math must match the original Swift bit for bit.
  target_compile_options(replaynes_frontend PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-fast-math -ffp-contract=off)
endif()
