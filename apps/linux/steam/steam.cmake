# "Add to Steam" (src/steam_shortcut.*: shortcuts.vdf + grid artwork) and the Steam library
# artwork / hicolor icons rendered from steam/artwork/src (see steam/artwork/render.sh).
# Included from apps/linux/CMakeLists.txt after the replaynes-linux target.
set(RNL_STEAM_DIR ${CMAKE_CURRENT_LIST_DIR})

add_library(rnl_steam STATIC ${RNL_STEAM_DIR}/../src/steam_shortcut.cpp)
target_include_directories(rnl_steam PUBLIC ${RNL_STEAM_DIR}/../src)
target_compile_options(rnl_steam PRIVATE ${RNL_WARNINGS})
target_link_libraries(replaynes-linux PRIVATE rnl_steam)

if(RNL_BUILD_TESTS)
  add_executable(test_steam_shortcut ${RNL_STEAM_DIR}/../tests/test_steam_shortcut.cpp
    ${REPLAYNES_ROOT}/tests/support/rn_test_main.cpp)
  target_include_directories(test_steam_shortcut PRIVATE ${REPLAYNES_ROOT}/tests)
  target_link_libraries(test_steam_shortcut PRIVATE rnl_steam)
  target_compile_definitions(test_steam_shortcut PRIVATE RNL_TEST_TMP="${RNL_TEST_TMP}")
  add_test(NAME test_steam_shortcut COMMAND test_steam_shortcut)
endif()

# /app/share/replaynes/steam-artwork: copied into Steam's grid folder by --add-to-steam.
install(FILES
  ${RNL_STEAM_DIR}/artwork/png/capsule.png
  ${RNL_STEAM_DIR}/artwork/png/wide.png
  ${RNL_STEAM_DIR}/artwork/png/hero.png
  ${RNL_STEAM_DIR}/artwork/png/logo.png
  ${RNL_STEAM_DIR}/artwork/png/icon.png
  DESTINATION ${CMAKE_INSTALL_DATADIR}/replaynes/steam-artwork)
foreach(size 64 128 256 512)
  install(FILES ${RNL_STEAM_DIR}/artwork/png/hicolor/${size}.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps RENAME ${APP_ID}.png)
endforeach()
install(FILES ${RNL_STEAM_DIR}/artwork/src/icon.svg
  DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME ${APP_ID}.svg)
