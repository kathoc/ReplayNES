# ReplayNES for Windows (replaynes-win -> ReplayNES.exe): the shared desktop frontend (apps/desktop:
# SDL3 + Dear ImGui UI, frame loop, logic) with the Windows parts here - the Direct3D 11 renderer,
# Windows fonts, the WIN32-subsystem entry point and resources (icon, UTF-8 / per-monitor DPI v2
# manifest, version). SDL3 is built from third_party/SDL (pinned release, static: no SDL3.dll).
# Added by the root CMakeLists.txt on Windows (MSVC, clang-cl, llvm-mingw cross builds:
# scripts/build-windows.sh). CRT display and MP4 export are not ported yet (export_unavailable.cpp).
enable_language(RC)
get_filename_component(RNW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

if(NOT EXISTS ${RNW_ROOT}/third_party/SDL/CMakeLists.txt)
  message(WARNING "third_party/SDL is missing (git submodule update --init third_party/SDL): replaynes-win is not built")
  return()
endif()

# SDL3, static, without its tests / examples / install rules.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
add_subdirectory(${RNW_ROOT}/third_party/SDL ${CMAKE_CURRENT_BINARY_DIR}/SDL EXCLUDE_FROM_ALL)

# The release version (root CMakeLists.txt).
file(STRINGS ${RNW_ROOT}/CMakeLists.txt RNW_ROOT_PROJECT REGEX "^project\\(ReplayNES VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RNL_APP_VERSION "${RNW_ROOT_PROJECT}")

set(RNL_SDL3_TARGET SDL3::SDL3-static)
set(RNL_APP_FEATURES RNL_HAVE_STEAM=0 RNL_HAVE_MP4_EXPORT=0 RNL_HAVE_CRT=0)
include(${RNL_DESKTOP_DIR}/app.cmake)

# Resources: icon, manifest (MSVC embeds the .manifest source with mt instead), version.
set(RNW_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/replaynes.ico)
set(RNW_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources/replaynes.manifest)
set(RNW_VERSION ${RNL_APP_VERSION})
string(REPLACE "." "," RNW_VERSION_COMMA ${RNL_APP_VERSION})
file(TO_CMAKE_PATH "${RNW_ICON}" RNW_ICON)
file(TO_CMAKE_PATH "${RNW_MANIFEST}" RNW_MANIFEST)
set(RNW_RC ${CMAKE_CURRENT_BINARY_DIR}/replaynes.rc)
configure_file(resources/replaynes.rc.in ${RNW_RC} @ONLY)
if(MSVC)
  file(READ ${RNW_RC} rc)
  string(REGEX REPLACE "1 24 \"[^\"]*\"\n" "" rc "${rc}")
  file(WRITE ${RNW_RC} "${rc}")
endif()

add_executable(replaynes-win WIN32
  src/main_windows.cpp
  src/d3d11_renderer.cpp
  src/fonts_windows.cpp
  src/export_unavailable.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${RNW_RC})
if(MSVC)
  target_sources(replaynes-win PRIVATE ${RNW_MANIFEST})
endif()
set_target_properties(replaynes-win PROPERTIES OUTPUT_NAME ReplayNES)
target_include_directories(replaynes-win PRIVATE src)
target_link_libraries(replaynes-win PRIVATE rnl_app SDL3::SDL3-static d3d11 dxgi d3dcompiler avrt ole32 shell32 uuid)
target_compile_definitions(replaynes-win PRIVATE UNICODE _UNICODE)
target_compile_options(replaynes-win PRIVATE ${RNL_WARNINGS})
if(MINGW)
  # Static C++ runtime (no libc++.dll / libunwind.dll next to the exe); see the toolchain file.
  # -municode: UNICODE makes SDL_main.h define wWinMain as the entry point.
  target_link_options(replaynes-win PRIVATE -static -municode $<$<CONFIG:Release>:-s>)
endif()
