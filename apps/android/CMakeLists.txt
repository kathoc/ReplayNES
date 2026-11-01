# ReplayNES / ReplayArcade for Android, the native part (docs/design/ANDROID.md): libmain.so = the
# desktop frontend (apps/desktop) with the Vulkan renderer of apps/linux, on SDL3 (libSDL3.so, from
# third_party/SDL; its Java glue is in the APK). Built by Gradle (app/build.gradle) for arm64-v8a.
cmake_minimum_required(VERSION 3.22)
project(ReplayNESAndroid LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(REPLAYNES_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(RNL_LINUX_DIR ${REPLAYNES_ROOT}/apps/linux)
set(REPLAYNES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_WINDOWS_APP OFF CACHE BOOL "" FORCE)
set(REPLAYNES_BUILD_WINDOWS_PROBE OFF CACHE BOOL "" FORCE)
set(RNL_DESKTOP_TESTS OFF)

# SDL3 as a shared library (SDLActivity loads libSDL3.so, then libmain.so).
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${REPLAYNES_ROOT}/third_party/SDL sdl EXCLUDE_FROM_ALL)

# The NDK's Vulkan (headers in the sysroot, libvulkan.so on the device) and shader compiler.
find_library(RNA_VULKAN_LIB vulkan REQUIRED)
add_library(Vulkan::Vulkan INTERFACE IMPORTED)
set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_LINK_LIBRARIES ${RNA_VULKAN_LIB})
file(GLOB RNA_GLSLC "${ANDROID_NDK}/shader-tools/*/glslc")
list(GET RNA_GLSLC 0 GLSLC)

include(${REPLAYNES_ROOT}/cmake/Product.cmake)
add_subdirectory(${REPLAYNES_ROOT} replaynes)
find_package(Threads REQUIRED)

# Shaders of the picture (blit, the blurred backdrop) and of the CRT (apps/linux).
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(sh blit.vert blit.frag backdrop.frag)
  set(out ${SHADER_OUT}/${sh}.spv.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.1 -mfmt=num -o ${out} ${RNL_LINUX_DIR}/shaders/${sh}
    DEPENDS ${RNL_LINUX_DIR}/shaders/${sh} VERBATIM)
  list(APPEND SHADER_INCS ${out})
endforeach()
set(CRT_SHADER_DIR ${RNL_LINUX_DIR}/shaders/crt)
set(CRT_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(MAKE_DIRECTORY ${CRT_SHADER_OUT}/crt)
file(GLOB CRT_SHADER_SRCS RELATIVE ${CRT_SHADER_DIR} ${CRT_SHADER_DIR}/*.comp ${CRT_SHADER_DIR}/*.vert ${CRT_SHADER_DIR}/*.frag)
file(GLOB CRT_SHADER_INCLUDES ${CRT_SHADER_DIR}/*.glsl)
set(CRT_SPV)
foreach(sh ${CRT_SHADER_SRCS})
  set(out ${CRT_SHADER_OUT}/crt/${sh}.spv.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.1 -mfmt=num -I ${CRT_SHADER_DIR} -o ${out} ${CRT_SHADER_DIR}/${sh}
    DEPENDS ${CRT_SHADER_DIR}/${sh} ${CRT_SHADER_INCLUDES} VERBATIM)
  list(APPEND CRT_SPV ${out})
endforeach()

set(RNL_SDL3_TARGET SDL3::SDL3-shared)
set(RNL_APP_VERSION "${RN_PRODUCT_VERSION}")
set(RNL_APP_FEATURES RNL_HAVE_STEAM=0 RNL_HAVE_MP4_EXPORT=0 RNL_HAVE_CRT=1)
include(${RNL_DESKTOP_DIR}/app.cmake)

add_library(main SHARED
  src/main_android.cpp
  src/fonts_android.cpp
  src/export_android.cpp
  ${RNL_DESKTOP_DIR}/src/export/export_job.cpp
  ${RNL_DESKTOP_DIR}/src/export/export_frame.cpp
  ${RNL_LINUX_DIR}/src/vk_renderer.cpp
  ${RNL_LINUX_DIR}/src/vk_backdrop.cpp
  ${RNL_LINUX_DIR}/src/vk_ui_resources.cpp
  ${RNL_LINUX_DIR}/src/render/crt_renderer.cpp
  ${RNL_LINUX_DIR}/src/render/headless_vulkan.cpp
  ${RNL_LINUX_DIR}/src/render/crt_display.cpp
  ${REPLAYNES_ROOT}/third_party/imgui/backends/imgui_impl_vulkan.cpp
  ${SHADER_INCS} ${CRT_SPV})
target_include_directories(main PRIVATE src ${RNL_DESKTOP_DIR}/src/export ${RNL_LINUX_DIR}/src ${SHADER_OUT} ${CRT_SHADER_OUT})
target_link_libraries(main PRIVATE rnl_app Vulkan::Vulkan Threads::Threads android log)
target_compile_options(main PRIVATE ${RNL_WARNINGS})
if(RN_PRODUCT_STARTUP_TARGET)
  target_link_libraries(main PRIVATE ${RN_PRODUCT_STARTUP_TARGET})
  target_compile_definitions(main PRIVATE RN_PRODUCT_HAS_STARTUP=1)
endif()
