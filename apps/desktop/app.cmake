# The shared SDL3 + Dear ImGui application (rnl_app) and Dear ImGui (rn_imgui: core + SDL3
# platform backend; the renderer backend - Vulkan / DX11 - is compiled by the platform project).
# Included by apps/linux/CMakeLists.txt and apps/windows/CMakeLists.txt after rnl_logic exists
# (apps/desktop/CMakeLists.txt, added through the root CMakeLists.txt). Inputs:
#   RNL_SDL3_TARGET    SDL3::SDL3 (Linux, the runtime's) / SDL3::SDL3-static (Windows, third_party/SDL)
#   RNL_APP_VERSION    the release version (root CMakeLists.txt)
#   RNL_APP_FEATURES   compile definitions of ui_features.h (RNL_HAVE_STEAM=1, RNL_HAVE_MP4_EXPORT=0 ...)
# The platform executable adds: main(), a Renderer, fonts.h, the MP4 export (export/mp4_export.h)
# and the CRT export processor (render/crt_export.h), or their "not available" stubs.
get_filename_component(RNL_REPO_ROOT ${RNL_DESKTOP_DIR}/../.. ABSOLUTE)
set(IMGUI_DIR ${RNL_REPO_ROOT}/third_party/imgui)
add_library(rn_imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp)
target_include_directories(rn_imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_definitions(rn_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(rn_imgui PUBLIC ${RNL_SDL3_TARGET})

set(RNL_APP_SRC ${RNL_DESKTOP_DIR}/src)
add_library(rnl_app STATIC
  ${RNL_APP_SRC}/app.cpp
  ${RNL_APP_SRC}/audio_out.cpp
  ${RNL_APP_SRC}/game_rect.cpp
  ${RNL_APP_SRC}/input_router.cpp
  ${RNL_APP_SRC}/perf_stats.cpp
  ${RNL_APP_SRC}/script.cpp
  ${RNL_APP_SRC}/ui.cpp
  ${RNL_APP_SRC}/ui_pages.cpp
  ${RNL_APP_SRC}/ui_play.cpp
  ${RNL_APP_SRC}/ui_settings.cpp
  ${RNL_APP_SRC}/ui_export.cpp
  ${RNL_APP_SRC}/ui_osk.cpp
  ${RNL_APP_SRC}/ui_update.cpp
  ${RNL_APP_SRC}/platform/shell.cpp)
target_include_directories(rnl_app PUBLIC ${RNL_APP_SRC})
target_link_libraries(rnl_app PUBLIC rnl_logic rn_imgui ${RNL_SDL3_TARGET})
target_compile_definitions(rnl_app PUBLIC RNL_APP_VERSION="${RNL_APP_VERSION}" ${RNL_APP_FEATURES})
target_compile_options(rnl_app PRIVATE ${RNL_WARNINGS})
if(WIN32)
  target_link_libraries(rnl_app PUBLIC shell32)
endif()
