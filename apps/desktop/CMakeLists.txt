# Shared desktop frontend (Linux / Steam Deck and Windows; docs/PORTING.md):
#   rnl_logic  frontend logic without SDL / graphics / ImGui (session lifecycle, the emulation
#              controller, library, thumbnails, settings, paths + platform services, l10n helpers),
#              unit-tested headlessly on every platform (test_desktop_frontend: macOS, Linux, Windows)
#   rnl_app    the SDL3 + Dear ImGui application (app.cpp: frame loop, ui*.cpp, input routing, audio
#              DRC, on-screen keyboard, scripts, perf stats), built by a platform project that provides
#              SDL3 and a Renderer (apps/linux: Vulkan, apps/windows: Direct3D 11) - see app.cmake.
# Added by the root CMakeLists.txt (rnl_logic + tests on every platform); apps/linux and
# apps/windows include app.cmake for rnl_app.
set(RNL_DESKTOP_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")
get_filename_component(RNL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
find_package(Threads REQUIRED)
if(WIN32 AND NOT MSVC)
  enable_language(RC)  # the manifest resource of the test executable (rnl_windows_manifest)
endif()

if(MSVC)
  set(RNL_WARNINGS /W3 /utf-8 /wd4244 /wd4267 /wd4305 /wd4996 CACHE INTERNAL "")
else()
  set(RNL_WARNINGS -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers CACHE INTERNAL "")
endif()

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
  src/ui_logic.cpp
  src/osk.cpp
  src/update_model.cpp
  src/platform/platform.cpp
  src/render/crt_settings.cpp)
if(WIN32)
  target_sources(rnl_logic PRIVATE src/platform/platform_windows.cpp src/platform/host_clock_windows.cpp)
  target_link_libraries(rnl_logic PUBLIC shell32 ole32 uuid)
  target_compile_definitions(rnl_logic PUBLIC UNICODE _UNICODE)
else()
  target_sources(rnl_logic PRIVATE src/platform/platform_xdg.cpp src/platform/trash_xdg.cpp)
endif()
target_include_directories(rnl_logic PUBLIC src src/export)
target_link_libraries(rnl_logic PUBLIC replaynes_frontend replaynes_engine Threads::Threads)
target_compile_options(rnl_logic PRIVATE ${RNL_WARNINGS})
if(MSVC)
  target_compile_options(rnl_logic PUBLIC /utf-8)
endif()

# Windows executables declare UTF-8 as their code page (narrow file APIs, std::filesystem and
# fopen then take the UTF-8 paths the frontend uses) - the tests too.
set(RNL_WINDOWS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/../windows/resources/replaynes.manifest CACHE INTERNAL "")
function(rnl_windows_manifest target)
  if(NOT WIN32)
    return()
  endif()
  if(MSVC)
    target_sources(${target} PRIVATE ${RNL_WINDOWS_MANIFEST})  # mt.exe embeds .manifest sources
  else()
    # llvm-mingw: a resource script with the manifest as RT_MANIFEST (1).
    set(rc ${CMAKE_CURRENT_BINARY_DIR}/${target}_manifest.rc)
    file(TO_CMAKE_PATH "${RNL_WINDOWS_MANIFEST}" m)
    file(WRITE ${rc} "1 24 \"${m}\"\n")
    target_sources(${target} PRIVATE ${rc})
  endif()
endfunction()

if(NOT DEFINED RNL_DESKTOP_TESTS)
  set(RNL_DESKTOP_TESTS ${REPLAYNES_BUILD_TESTS})
endif()
if(RNL_DESKTOP_TESTS)
  enable_testing()
  set(RNL_TEST_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)
  file(MAKE_DIRECTORY ${RNL_TEST_TMP})
  add_executable(test_desktop_frontend tests/test_desktop_frontend.cpp ${RNL_REPO_ROOT}/tests/support/rn_test_main.cpp)
  target_include_directories(test_desktop_frontend PRIVATE ${RNL_REPO_ROOT}/tests)
  target_link_libraries(test_desktop_frontend PRIVATE rnl_logic)
  target_compile_definitions(test_desktop_frontend PRIVATE RNL_TEST_TMP="${RNL_TEST_TMP}")
  target_compile_options(test_desktop_frontend PRIVATE ${RNL_WARNINGS})
  rnl_windows_manifest(test_desktop_frontend)
  # Next to the engine tests (scripts/windows-vm/run-tests.sh and CI run build/tests/test_*.exe).
  if(WIN32)
    set_target_properties(test_desktop_frontend PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
  endif()
  add_test(NAME test_desktop_frontend COMMAND test_desktop_frontend)
  set_tests_properties(test_desktop_frontend PROPERTIES TIMEOUT 300)
endif()
