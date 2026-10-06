# ReplayNES Linux / Steam Deck frontend (SDL3 + Vulkan + Dear ImGui) on the engine C API.
# Standalone project: configure this directory (the Flatpak manifest does, see
# apps/linux/flatpak/io.github.replaynes.ReplayNES.yml); the engine comes from the repository root.
#   cmake -S apps/linux -B build-linux -G Ninja && cmake --build build-linux
# Needs SDL3 (>= 3.2), Vulkan headers + loader and glslc (all in org.freedesktop.Sdk 25.08), and
# FFmpeg's libavcodec / libavformat / libavutil (MP4 export; FFmpeg 7.1 in the SDK, 8 on Homebrew).
# -DRNL_EXPORT_ONLY=ON builds only the exporter (rnl_export + replaynes-export), e.g. on macOS:
#   cmake -S apps/linux -B build-export -G Ninja -DRNL_EXPORT_ONLY=ON && ninja -C build-export replaynes-export
cmake_minimum_required(VERSION 3.16)
project(ReplayNESLinux VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(REPLAYNES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# Engine static libraries + replaynes-cli (make-test-rom for the perf runs); no engine tests here.
set(REPLAYNES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
option(REPLAYNES_BUILD_CLI "Build replaynes-cli" ON)
option(RNL_EXPORT_ONLY "Build only the MP4 exporter (rnl_export, replaynes-export), no SDL/Vulkan app" OFF)
add_subdirectory(${REPLAYNES_ROOT} replaynes)
find_package(Threads REQUIRED)
include(GNUInstallDirs)

# MP4 export (FFmpeg libav*): static library used by the app + the headless replaynes-export tool.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil)
add_library(rnl_export STATIC src/export/mp4_export.cpp)
target_include_directories(rnl_export PUBLIC src/export)
target_link_libraries(rnl_export PUBLIC replaynes_frontend replaynes_engine PkgConfig::FFMPEG Threads::Threads)
target_compile_options(rnl_export PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
add_executable(replaynes-export src/export/export_cli.cpp)
target_link_libraries(replaynes-export PRIVATE rnl_export)
target_compile_options(replaynes-export PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
install(TARGETS replaynes-export RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(RNL_EXPORT_ONLY)
  return()
endif()

find_package(SDL3 REQUIRED CONFIG)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

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

add_executable(replaynes-linux
  src/main.cpp
  src/vk_renderer.cpp
  src/audio_out.cpp
  src/paths.cpp
  src/perf_stats.cpp
  src/interim/emu_host.cpp
  ${SHADER_INCS})
target_include_directories(replaynes-linux PRIVATE src ${SHADER_OUT})
target_link_libraries(replaynes-linux PRIVATE replaynes_engine rnl_export rn_imgui SDL3::SDL3 Vulkan::Vulkan Threads::Threads)
target_compile_options(replaynes-linux PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

install(TARGETS replaynes-linux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(REPLAYNES_BUILD_CLI)
  install(TARGETS replaynes-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
set(APP_ID io.github.replaynes.ReplayNES)
install(FILES flatpak/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES flatpak/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${REPLAYNES_ROOT}/apps/macos/Resources/IconSource/replaynes.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME ${APP_ID}.svg)
