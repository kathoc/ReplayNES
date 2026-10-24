# Included in both the root project and the standalone Linux app scope.
set(RN_PRODUCT_FILE "${CMAKE_CURRENT_LIST_DIR}/products/replaynes.cmake" CACHE FILEPATH "Product profile CMake file")
include("${RN_PRODUCT_FILE}")
# Optional: frames from power-on to the library picture of a game without a project (an arcade
# board's start-up self test lasts longer than an NES game's first screen).
if(NOT DEFINED RN_PRODUCT_LIBRARY_PICTURE_FRAME)
  set(RN_PRODUCT_LIBRARY_PICTURE_FRAME 300)
endif()
foreach(key NAME APP_ID EXECUTABLE DATA_DIR ENV_PREFIX PROJECT_EXT PROJECT_FORMAT ROM_EXTS TEMP_PREFIX L10N_OVERRIDES LINUX_DESKTOP_FILE LINUX_METAINFO_FILE STEAM_ARTWORK_DIR STARTUP_TARGET DEFAULT_CORE GAMEDB NES)
  if(NOT DEFINED RN_PRODUCT_${key})
    message(FATAL_ERROR "Product profile must define RN_PRODUCT_${key}")
  endif()
  string(REPLACE "\\" "\\\\" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}}")
  string(REPLACE "\"" "\\\"" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
  string(REPLACE "\n" "\\n" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
  string(REPLACE "\r" "\\r" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
endforeach()
