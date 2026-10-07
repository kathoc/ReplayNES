# ReplayNES for Windows (replaynes-win -> ReplayNES.exe): the shared desktop frontend (apps/desktop:
# SDL3 + Dear ImGui UI, frame loop, logic) with the Windows parts here - the Direct3D 11 renderer,
# Windows fonts, the WIN32-subsystem entry point and resources (icon, UTF-8 / per-monitor DPI v2
# manifest, version). SDL3 is built from third_party/SDL (pinned release, static: no SDL3.dll).
# Added by the root CMakeLists.txt on Windows (MSVC, clang-cl, llvm-mingw cross builds:
# scripts/build-windows.sh). The CRT display runs the HLSL generated from the Vulkan GLSL
# (shaders/crt, scripts/generate-crt-hlsl.sh) on Direct3D 11 compute (rnw_crt).
enable_language(RC)
get_filename_component(RNW_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)

# CRT (nesterm physical model) on Direct3D 11 compute: the generated HLSL embedded at build time,
# compiled at run time by D3DCompile (d3dcompiler_47.dll ships with Windows 10/11).
file(GLOB RNW_CRT_HLSL ${CMAKE_CURRENT_SOURCE_DIR}/shaders/crt/*.hlsl)
set(RNW_CRT_INC ${CMAKE_CURRENT_BINARY_DIR}/generated/crt_hlsl.inc)
add_custom_command(OUTPUT ${RNW_CRT_INC}
  COMMAND ${CMAKE_COMMAND} -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders/crt -DOUT=${RNW_CRT_INC} -P ${RNW_ROOT}/cmake/embed_hlsl.cmake
  DEPENDS ${RNW_CRT_HLSL} ${RNW_ROOT}/cmake/embed_hlsl.cmake VERBATIM)
add_library(rnw_crt STATIC src/crt_d3d11.cpp src/crt_export_d3d11.cpp ${RNW_CRT_INC})
target_include_directories(rnw_crt PUBLIC src PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(rnw_crt PUBLIC rnl_logic d3d11 dxgi d3dcompiler)
target_compile_definitions(rnw_crt PRIVATE UNICODE _UNICODE)
target_compile_options(rnw_crt PRIVATE ${RNL_WARNINGS})
# MP4 export through Media Foundation (H.264 + AAC sink writer) with the frame helpers and
# ExportJob shared with the Linux FFmpeg exporter; replaynes-export.exe = the headless CLI + self-test.
add_library(rnw_export STATIC src/mp4_export_mf.cpp ${RNL_DESKTOP_DIR}/src/export/export_frame.cpp
  ${RNL_DESKTOP_DIR}/src/export/export_job.cpp ${RNL_DESKTOP_DIR}/src/export/mp4_retime.cpp)
target_link_libraries(rnw_export PUBLIC rnl_logic mfplat mfreadwrite mfuuid ole32 oleaut32 uuid)
target_compile_definitions(rnw_export PRIVATE UNICODE _UNICODE)
target_compile_options(rnw_export PRIVATE ${RNL_WARNINGS})
add_executable(replaynes-export ${RNL_DESKTOP_DIR}/src/export/export_cli.cpp)
target_link_libraries(replaynes-export PRIVATE rnw_export rnw_crt)
target_compile_definitions(replaynes-export PRIVATE RNL_EXPORT_CRT=1)
target_compile_options(replaynes-export PRIVATE ${RNL_WARNINGS})
rnl_windows_manifest(replaynes-export)

if(REPLAYNES_BUILD_TESTS)
  # Conformance against tests/fixtures/crt/reference.json (copied next to the executable for the
  # VM / CI runs) + --bench. Hardware adapter, or WARP (--warp; CI runners have no GPU).
  add_executable(test_crt_d3d11 src/crt_test_d3d11.cpp)
  target_link_libraries(test_crt_d3d11 PRIVATE rnw_crt)
  target_compile_definitions(test_crt_d3d11 PRIVATE UNICODE _UNICODE RN_CRT_FIXTURE="${RNW_ROOT}/tests/fixtures/crt/reference.json")
  target_compile_options(test_crt_d3d11 PRIVATE ${RNL_WARNINGS})
  rnl_windows_manifest(test_crt_d3d11)
  set_target_properties(test_crt_d3d11 PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  add_custom_command(TARGET test_crt_d3d11 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RNW_ROOT}/tests/fixtures/crt/reference.json $<TARGET_FILE_DIR:test_crt_d3d11>/crt_reference.json)
  add_test(NAME crt_conformance_d3d11 COMMAND test_crt_d3d11)
  set_tests_properties(crt_conformance_d3d11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()

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

# The release version (root CMakeLists.txt); REPLAYNES_VERSION_OVERRIDE (e.g. 0.3.2-test) for
# update tests.
file(STRINGS ${RNW_ROOT}/CMakeLists.txt RNW_ROOT_PROJECT REGEX "^project\\(ReplayNES VERSION")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RNL_APP_VERSION "${RNW_ROOT_PROJECT}")
set(REPLAYNES_VERSION_OVERRIDE "" CACHE STRING "Version string of ReplayNES.exe instead of the project version (update tests)")
if(REPLAYNES_VERSION_OVERRIDE)
  set(RNL_APP_VERSION ${REPLAYNES_VERSION_OVERRIDE})
endif()
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" RNW_VERSION_NUMBER "${RNL_APP_VERSION}")

# In-app updates: WinSparkle (MIT, prebuilt WinSparkle.dll per architecture next to the exe,
# loaded at run time; the download is pinned by SHA-256). RNW_WINSPARKLE=OFF: no updates (offline
# builds). The EdDSA public key is Sparkle's SUPublicEDKey (apps/macos/project.yml, the same key
# signs both feeds); RNW_UPDATE_PUBLIC_KEY / RNW_APPCAST_URL override it for update tests.
option(RNW_WINSPARKLE "Bundle WinSparkle.dll (in-app updates)" ON)
set(RNW_WINSPARKLE_VERSION 0.9.4)
set(RNW_WINSPARKLE_SHA256 6037df37fc263bd1650a1c4949681a9d40ffe991d01f35892a406cb5d103c976)
file(STRINGS ${RNW_ROOT}/apps/macos/project.yml RNW_SU_KEY_LINE REGEX "SUPublicEDKey:")
string(REGEX REPLACE ".*SUPublicEDKey: *\"?([A-Za-z0-9+/=]+)\"?.*" "\\1" RNW_SU_KEY "${RNW_SU_KEY_LINE}")
set(RNW_UPDATE_PUBLIC_KEY "${RNW_SU_KEY}" CACHE STRING "EdDSA public key of the update feed (base64)")
set(RNW_APPCAST_URL "https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml" CACHE STRING "Update feed")
if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|ARM64|arm64)$")
  set(RNW_WINSPARKLE_ARCH ARM64)
else()
  set(RNW_WINSPARKLE_ARCH x64)
endif()
set(RNW_WINSPARKLE_DLL "")
if(RNW_WINSPARKLE)
  set(RNW_WS_CACHE ${RNW_ROOT}/build/cache)
  set(RNW_WS_ZIP ${RNW_WS_CACHE}/WinSparkle-${RNW_WINSPARKLE_VERSION}.zip)
  if(NOT EXISTS ${RNW_WS_ZIP})
    message(STATUS "Downloading WinSparkle ${RNW_WINSPARKLE_VERSION}")
    file(DOWNLOAD https://github.com/vslavik/winsparkle/releases/download/v${RNW_WINSPARKLE_VERSION}/WinSparkle-${RNW_WINSPARKLE_VERSION}.zip
      ${RNW_WS_ZIP} EXPECTED_HASH SHA256=${RNW_WINSPARKLE_SHA256} STATUS ws_status)
    list(GET ws_status 0 ws_code)
    if(NOT ws_code EQUAL 0)
      file(REMOVE ${RNW_WS_ZIP})
      message(FATAL_ERROR "WinSparkle download failed (${ws_status}); configure with -DRNW_WINSPARKLE=OFF to build without in-app updates")
    endif()
  endif()
  file(SHA256 ${RNW_WS_ZIP} ws_hash)
  if(NOT ws_hash STREQUAL RNW_WINSPARKLE_SHA256)
    message(FATAL_ERROR "${RNW_WS_ZIP}: SHA-256 mismatch")
  endif()
  set(RNW_WS_DIR ${CMAKE_CURRENT_BINARY_DIR}/WinSparkle-${RNW_WINSPARKLE_VERSION})
  if(NOT EXISTS ${RNW_WS_DIR}/${RNW_WINSPARKLE_ARCH}/Release/WinSparkle.dll)
    file(ARCHIVE_EXTRACT INPUT ${RNW_WS_ZIP} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
      PATTERNS WinSparkle-${RNW_WINSPARKLE_VERSION}/${RNW_WINSPARKLE_ARCH}/Release/WinSparkle.dll
               WinSparkle-${RNW_WINSPARKLE_VERSION}/COPYING)
  endif()
  set(RNW_WINSPARKLE_DLL ${RNW_WS_DIR}/${RNW_WINSPARKLE_ARCH}/Release/WinSparkle.dll)
endif()

set(RNL_SDL3_TARGET SDL3::SDL3-static)
set(RNL_APP_FEATURES RNL_HAVE_STEAM=0 RNL_HAVE_MP4_EXPORT=1 RNL_HAVE_CRT=1)
include(${RNL_DESKTOP_DIR}/app.cmake)

# Resources: icon, manifest (MSVC embeds the .manifest source with mt instead), version.
set(RNW_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/replaynes.ico)
set(RNW_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/resources/replaynes.manifest)
set(RNW_VERSION ${RNL_APP_VERSION})
string(REPLACE "." "," RNW_VERSION_COMMA ${RNW_VERSION_NUMBER})
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
  src/update_winsparkle.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${RNW_RC})
if(MSVC)
  target_sources(replaynes-win PRIVATE ${RNW_MANIFEST})
endif()
set_target_properties(replaynes-win PROPERTIES OUTPUT_NAME ReplayNES)
target_include_directories(replaynes-win PRIVATE src)
target_link_libraries(replaynes-win PRIVATE rnl_app rnw_crt rnw_export SDL3::SDL3-static d3d11 dxgi d3dcompiler avrt ole32 shell32 uuid)
target_compile_definitions(replaynes-win PRIVATE UNICODE _UNICODE RNW_UPDATE_PUBLIC_KEY="${RNW_UPDATE_PUBLIC_KEY}"
  RNW_APPCAST_URL="${RNW_APPCAST_URL}")
if(RNW_WINSPARKLE_DLL)
  add_custom_command(TARGET replaynes-win POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RNW_WINSPARKLE_DLL} $<TARGET_FILE_DIR:replaynes-win>/WinSparkle.dll)
endif()
target_compile_options(replaynes-win PRIVATE ${RNL_WARNINGS})
if(MINGW)
  # Static C++ runtime (no libc++.dll / libunwind.dll next to the exe); see the toolchain file.
  # -municode: UNICODE makes SDL_main.h define wWinMain as the entry point.
  target_link_options(replaynes-win PRIVATE -static -municode $<$<CONFIG:Release>:-s>)
endif()
