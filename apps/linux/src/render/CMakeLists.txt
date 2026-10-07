# CRT (nesterm physical model) on Vulkan compute: rnl_render (CPU setup model + pipeline, SPIR-V
# compiled at build time from apps/linux/shaders/crt) and replaynes-crt-test (headless conformance
# against tests/fixtures/crt/reference.json + GPU benchmark). Needs Vulkan headers/loader + glslc.
set(CRT_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../shaders/crt)
set(CRT_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(MAKE_DIRECTORY ${CRT_SHADER_OUT}/crt)
set(CRT_SHADERS
  rf_forward_tg.comp rf_inverse_tg.comp rf_init_codes.comp rf_butterfly2.comp rf_butterfly.comp rf_multiply.comp
  rf_extract.comp rx_reduce.comp rx_agc.comp rx_prepare.comp rx_decode.comp raster_area.comp supply_mean.comp
  supply_row.comp supply_state.comp supply_resample.comp spot_h.comp tube_h.comp tube_v.comp tube_v_growth.comp
  tube_scatter.comp tube_scatter_x16.comp tube_scatter_y16.comp tube_mix.comp tube_lit.comp tube_persist.comp
  show_kernel.comp tube_scatter_tx.comp tube_h_rows.comp tube_lit_persist.comp show.vert show.frag)
set(CRT_SHADER_INCLUDES ${CRT_SHADER_DIR}/common.glsl ${CRT_SHADER_DIR}/rf_common.glsl ${CRT_SHADER_DIR}/precise_math.glsl)
set(CRT_SPV)
foreach(sh ${CRT_SHADERS})
  set(src ${CRT_SHADER_DIR}/${sh})
  set(out ${CRT_SHADER_OUT}/crt/${sh}.spv.inc)
  add_custom_command(OUTPUT ${out}
    COMMAND ${GLSLC} -O --target-env=vulkan1.1 -mfmt=num -I ${CRT_SHADER_DIR} -o ${out} ${src}
    DEPENDS ${src} ${CRT_SHADER_INCLUDES} VERBATIM)
  list(APPEND CRT_SPV ${out})
endforeach()

add_library(rnl_render STATIC crt_model.cpp crt_renderer.cpp headless_vulkan.cpp crt_display.cpp ${CRT_SPV})
target_include_directories(rnl_render PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. PRIVATE ${CRT_SHADER_OUT})
# rnl_logic: CrtSettings / post_process.h (apps/desktop/src/render) and the frontend core.
target_link_libraries(rnl_render PUBLIC rnl_logic replaynes_frontend Vulkan::Vulkan Threads::Threads)
target_compile_options(rnl_render PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

add_executable(replaynes-crt-test crt_test.cpp)
target_link_libraries(replaynes-crt-test PRIVATE rnl_render)
target_compile_definitions(replaynes-crt-test PRIVATE
  RN_CRT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/fixtures/crt/reference.json")
add_test(NAME crt_conformance COMMAND replaynes-crt-test)
set_tests_properties(crt_conformance PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# "Apply CRT effect" for the MP4 export (offline, window-less device) + the export CLI's --crt.
add_library(rnl_crt_export STATIC crt_export.cpp)
target_link_libraries(rnl_crt_export PUBLIC rnl_render rnl_export)
target_link_libraries(replaynes-export PRIVATE rnl_crt_export)
target_compile_definitions(replaynes-export PRIVATE RNL_EXPORT_CRT=1)
