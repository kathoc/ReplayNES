# ReplayNES Linux / Steam Deck frontend (SDL3 + Vulkan + Dear ImGui) on the engine C API and the
# shared frontend core (frontend/, rnf_*).
# Standalone project: configure this directory (the Flatpak manifest does, see
# apps/linux/flatpak/io.github.replaynes.ReplayNES.yml); the engine comes from the repository root.
#   cmake -S apps/linux -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux
# Needs SDL3 (>= 3.2), Vulkan headers + loader, glslc, fontconfig and FFmpeg's libavcodec /
# libavformat / libavutil (MP4 export; FFmpeg 7.1 in the SDK, 8 on Homebrew) - all in
# org.freedesktop.Sdk 25.08.
# -DRNL_EXPORT_ONLY=ON builds only the exporter (rnl_export + replaynes-export), e.g. on macOS:
#   cmake -S apps/linux -B build-export -G Ninja -DRNL_EXPORT_ONLY=ON && ninja -C build-export replaynes-export
cmake_minimum_required(VERSION 3.16)
project(ReplayNESLinux VERSION 0.3.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(RNL_BUILD_TESTS "Build the Linux frontend tests (ctest)" ON)
option(RNL_ENGINE_TESTS "Also build the engine + frontend core tests" OFF)
option(RNL_EXPORT_ONLY "Build only the MP4 exporter (rnl_export, replaynes-export), no SDL/Vulkan app" OFF)

set(REPLAYNES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# Engine + frontend core static libraries + replaynes-cli (make-test-rom for the perf runs).
set(REPLAYNES_BUILD_TESTS ${RNL_ENGINE_TESTS} CACHE BOOL "" FORCE)
option(REPLAYNES_BUILD_CLI "Build replaynes-cli" ON)
add_subdirectory(${REPLAYNES_ROOT} replaynes)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
include(GNUInstallDirs)
set(RNL_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

# MP4 export (FFmpeg libav*): static library used by the app + the headless replaynes-export tool.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
add_library(rnl_export STATIC src/export/mp4_export.cpp)
target_include_directories(rnl_export PUBLIC src/export)
target_link_libraries(rnl_export PUBLIC replaynes_frontend replaynes_engine PkgConfig::FFMPEG Threads::Threads)
target_compile_options(rnl_export PRIVATE ${RNL_WARNINGS})
add_executable(replaynes-export src/export/export_cli.cpp)
target_link_libraries(replaynes-export PRIVATE rnl_export)
target_compile_options(replaynes-export PRIVATE ${RNL_WARNINGS})
install(TARGETS replaynes-export RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(RNL_EXPORT_ONLY)
  return()
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

# CRT (Vulkan compute port of the nesterm physical model) + its headless conformance test.
enable_testing()
add_subdirectory(src/render)
if(APPLE)
  # Development on a Mac (MoltenVK): only the platform-independent Vulkan parts build here.
  message(STATUS "macOS: building the render module and its tests only")
  return()
endif()

find_package(SDL3 REQUIRED CONFIG)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

# Shaders: GLSL -> SPIR-V at build time, included as uint32 initialisers.
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(sh blit.vert blit.frag)
  set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh})
  set(out ${SHADER_OUT}/${sh}.spv.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.1 -mfmt=num -o ${out} ${src}
    DEPENDS ${src} VERBATIM)
  list(APPEND SHADER_INCS ${out})
endforeach()

# Dear ImGui (vendored, MIT, third_party/imgui): core + SDL3 / Vulkan backends.
set(IMGUI_DIR ${REPLAYNES_ROOT}/third_party/imgui)
add_library(rn_imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(rn_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(rn_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(rn_imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)

# Frontend logic without SDL / Vulkan / ImGui (unit-tested headless): session lifecycle, the
# emulation controller, library, thumbnails, settings, trash, l10n helpers.
add_library(rnl_logic STATIC
  src/app_model.cpp
  src/emulation.cpp
  src/l10n.cpp
  src/library.cpp
  src/manifest.cpp
  src/paths.cpp
  src/png_writer.cpp
  src/settings.cpp
  src/thumbnails.cpp
  src/trash.cpp
  src/ui_logic.cpp
  src/osk.cpp
  src/update_model.cpp)
target_include_directories(rnl_logic PUBLIC src)
target_link_libraries(rnl_logic PUBLIC replaynes_frontend replaynes_engine Threads::Threads)
target_compile_options(rnl_logic PRIVATE ${RNL_WARNINGS})

add_executable(replaynes-linux
  src/main.cpp
  src/vk_renderer.cpp
  src/vk_ui_resources.cpp
  src/audio_out.cpp
  src/fonts.cpp
  src/input_router.cpp
  src/perf_stats.cpp
  src/script.cpp
  src/ui.cpp
  src/ui_pages.cpp
  src/ui_play.cpp
  src/ui_settings.cpp
  src/ui_export.cpp
  src/ui_osk.cpp
  src/ui_update.cpp
  src/update_service.cpp
  ${SHADER_INCS})
target_include_directories(replaynes-linux PRIVATE src ${SHADER_OUT})
target_link_libraries(replaynes-linux PRIVATE rnl_logic rnl_crt_export rnl_render rnl_export rn_imgui SDL3::SDL3 Vulkan::Vulkan
  PkgConfig::FONTCONFIG Threads::Threads)
target_compile_options(replaynes-linux PRIVATE ${RNL_WARNINGS})
# The version of the release (root CMakeLists.txt): --version, the update notice.
file(STRINGS ${REPLAYNES_ROOT}/CMakeLists.txt RNL_ROOT_PROJECT REGEX "^project\\(ReplayNES VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RNL_APP_VERSION "${RNL_ROOT_PROJECT}")
target_compile_definitions(replaynes-linux PRIVATE RNL_APP_VERSION="${RNL_APP_VERSION}")
# In-app updates through the XDG Flatpak portal (GDBus; GLib is in org.freedesktop.Platform).
pkg_check_modules(GIO IMPORTED_TARGET gio-2.0 gio-unix-2.0)
if(GIO_FOUND)
  target_link_libraries(replaynes-linux PRIVATE PkgConfig::GIO)
  target_compile_definitions(replaynes-linux PRIVATE RNL_HAVE_GIO)
else()
  message(STATUS "gio-2.0 not found: in-app updates (Flatpak portal) disabled")
endif()

if(RNL_BUILD_TESTS)
  set(RNL_TEST_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)
  file(MAKE_DIRECTORY ${RNL_TEST_TMP})
  add_executable(test_linux_frontend tests/test_linux_frontend.cpp ${REPLAYNES_ROOT}/tests/support/rn_test_main.cpp)
  target_include_directories(test_linux_frontend PRIVATE ${REPLAYNES_ROOT}/tests)
  target_link_libraries(test_linux_frontend PRIVATE rnl_logic)
  target_compile_definitions(test_linux_frontend PRIVATE RNL_TEST_TMP="${RNL_TEST_TMP}")
  add_test(NAME test_linux_frontend COMMAND test_linux_frontend)
  set_tests_properties(test_linux_frontend PROPERTIES TIMEOUT 300)
endif()

install(TARGETS replaynes-linux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(REPLAYNES_BUILD_CLI)
  install(TARGETS replaynes-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
set(APP_ID io.github.replaynes.ReplayNES)
install(FILES flatpak/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES flatpak/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
# App icons (hicolor 64-512 + scalable), Steam artwork, Add to Steam module + test.
include(steam/steam.cmake)
