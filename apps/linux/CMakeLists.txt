# ReplayNES Linux / Steam Deck frontend (SDL3 + Vulkan + Dear ImGui) on the engine C API.
# Standalone project: configure this directory (the Flatpak manifest does, see
# apps/linux/flatpak/io.github.replaynes.ReplayNES.yml); the engine comes from the repository root.
#   cmake -S apps/linux -B build-linux -G Ninja && cmake --build build-linux
# Needs SDL3 (>= 3.2), Vulkan headers + loader and glslc (all in org.freedesktop.Sdk 25.08).
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
add_subdirectory(${REPLAYNES_ROOT} replaynes)

find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
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
target_link_libraries(replaynes-linux PRIVATE replaynes_engine rn_imgui SDL3::SDL3 Vulkan::Vulkan Threads::Threads)
target_compile_options(replaynes-linux PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

include(GNUInstallDirs)
install(TARGETS replaynes-linux RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(REPLAYNES_BUILD_CLI)
  install(TARGETS replaynes-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
set(APP_ID io.github.replaynes.ReplayNES)
install(FILES flatpak/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES flatpak/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES ${REPLAYNES_ROOT}/apps/macos/Resources/IconSource/replaynes.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME ${APP_ID}.svg)
