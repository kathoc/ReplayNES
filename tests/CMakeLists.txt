add_library(rn_test_main STATIC support/rn_test_main.cpp)
target_include_directories(rn_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

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
  add_test(NAME ${name} COMMAND ${name})
  # macOS malloc fills new allocations with garbage: uninitialised core state cannot hide.
  set_tests_properties(${name} PROPERTIES TIMEOUT 300 ENVIRONMENT "MallocPreScribble=1;MallocScribble=1")
endfunction()

rn_add_test(test_core)
rn_add_test(test_determinism)
rn_add_test(test_timeline)
rn_add_test(test_persist)
rn_add_test(test_input)
rn_add_test(test_render)
rn_add_test(test_longrun)
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
  rn_add_frontend_test(test_frontend_export)
endif()
