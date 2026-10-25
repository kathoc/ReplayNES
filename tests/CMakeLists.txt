add_library(rn_test_main STATIC support/rn_test_main.cpp)
target_include_directories(rn_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)

set(RN_TEST_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${RN_TEST_TMP})

function(rn_add_test name)
  set(libs ${ARGN})
  if(NOT libs)
    set(libs replaynes_engine)
  endif()
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${libs} rn_test_main)
  target_compile_definitions(${name} PRIVATE RN_TEST_TMP="${RN_TEST_TMP}")
  if(MSVC)
    target_compile_options(${name} PRIVATE /utf-8)  # UTF-8 string literals in the sources
  endif()
  add_test(NAME ${name} COMMAND ${name})
  # macOS malloc fills new allocations with garbage: uninitialised core state cannot hide.
  set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "MallocPreScribble=1;MallocScribble=1")
endfunction()

rn_add_test(test_core)
rn_add_test(test_determinism)
rn_add_test(test_timeline)
rn_add_test(test_rewind_cache)
rn_add_test(test_retry_takes)
rn_add_test(test_persist)
rn_add_test(test_input)
rn_add_test(test_render)
rn_add_test(test_longrun)
# ReplayNES Test Cartridge (tools/testcart): determinism, raster effects, rapid-fire meter.
rn_add_test(test_testcart)
# The committed ROM image (engine/src/testrom/TestCartridge*.inc) matches its 6502 source.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME product_release_identity
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_product_release.py
             --source ${PROJECT_SOURCE_DIR} --cmake ${CMAKE_COMMAND})
  add_test(NAME testcart_generated_up_to_date
           COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/testcart/build.py --check)
endif()
# Fixed per-frame video/audio digests (Nestopia + Test Cartridge through the C API): any change of
# an emulated frame fails here. RN_GOLDEN_UPDATE=1 rewrites tests/fixtures/golden/ when a change is intended.
rn_add_test(test_golden)
# Media info and the external core registration with a non-NES test core (size, stride, rate, stereo).
rn_add_test(test_media)
# Input layouts of other cores: pipeline (turbo on any button, axes), records, projects.
rn_add_test(test_input_layout)
target_compile_definitions(test_golden PRIVATE RN_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/golden")
# Practice mode + A/B repeat slots (engine + persistence + C API).
rn_add_test(test_practice)
# A/B slots defined from take frames (timeline selection: rn_practice_set_range).
rn_add_test(test_practice_range)
# "Reset Project": session reset + persistence + crash safety (rn_session_reset).
rn_add_test(test_reset)
# Uses the developer's own ROMs in <repo>/roms when present (gitignored); skipped otherwise.
rn_add_test(test_local_roms)
target_compile_definitions(test_local_roms PRIVATE RN_LOCAL_ROMS="${PROJECT_SOURCE_DIR}/roms")
# Display-side photosensitive flash reduction (synthetic frames; local ROMs when present).
rn_add_test(test_flash_filter)
target_compile_definitions(test_flash_filter PRIVATE RN_LOCAL_ROMS="${PROJECT_SOURCE_DIR}/roms")
# The optimised flash filter is bit-exact against a frozen copy of the original implementation.
rn_add_test(test_flash_filter_exact)
target_sources(test_flash_filter_exact PRIVATE support/FlashFilterRef.cpp)
if(NOT MSVC)
  target_compile_options(test_flash_filter_exact PRIVATE -fno-fast-math -ffp-contract=off)
endif()

# Shared frontend core (frontend/: C API replaynes/frontend.h). Ported from the macOS XCTests of the
# logic that moved there; they run on every platform.
if(TARGET replaynes_frontend)
  function(rn_add_frontend_test name)
    rn_add_test(${name} replaynes_frontend)  # brings replaynes_engine
  endfunction()
  rn_add_frontend_test(test_frontend_pacing)
  rn_add_frontend_test(test_frontend_input)
  rn_add_frontend_test(test_frontend_timeline)
  rn_add_frontend_test(test_frontend_resume)
  rn_add_frontend_test(test_frontend_library)
  rn_add_frontend_test(test_frontend_playback)
  rn_add_frontend_test(test_frontend_practice_return)
  rn_add_frontend_test(test_frontend_resume_countdown)
  rn_add_frontend_test(test_frontend_tape)
  rn_add_frontend_test(test_frontend_export)
  rn_add_frontend_test(test_frontend_menu)
  target_compile_definitions(test_frontend_menu PRIVATE RN_MENU_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/menu-desktop-before-noflash.txt")
  rn_add_frontend_test(test_frontend_seekbar)
  rn_add_frontend_test(test_frontend_crt)
  # Game database + library catalog; also identifies the developer's own ROMs in <repo>/roms.
  rn_add_frontend_test(test_frontend_gamedb)
  target_compile_definitions(test_frontend_gamedb PRIVATE RN_LOCAL_ROMS="${PROJECT_SOURCE_DIR}/roms")
endif()

rn_add_test(test_core_selection)
# Always validate the generated reference waveforms; compare both cores when built.
rn_add_test(test_replaynescore_volume)
# Nine independent cartridges each run for two emulated seconds. Keep the
# diagnostic bounded while allowing the experimental core to run under CI load.
set_tests_properties(test_replaynescore_volume PROPERTIES TIMEOUT 900)
if(REPLAYNES_ENABLE_REPLAYNESCORE)
  rn_add_test(test_replaynescore_audio)
  rn_add_test(test_replaynescore_adapter)
endif()
