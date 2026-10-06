add_library(rn_test_main STATIC support/rn_test_main.cpp)
target_include_directories(rn_test_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(RN_TEST_TMP ${CMAKE_CURRENT_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${RN_TEST_TMP})

function(rn_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE replaynes_engine rn_test_main)
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
# Uses the developer's own ROMs in <repo>/roms when present (gitignored); skipped otherwise.
rn_add_test(test_local_roms)
target_compile_definitions(test_local_roms PRIVATE RN_LOCAL_ROMS="${PROJECT_SOURCE_DIR}/roms")
# Display-side photosensitive flash reduction (synthetic frames; local ROMs when present).
rn_add_test(test_flash_filter)
target_compile_definitions(test_flash_filter PRIVATE RN_LOCAL_ROMS="${PROJECT_SOURCE_DIR}/roms")
