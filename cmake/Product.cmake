# Included in both the root project and the standalone Linux app scope.
set(RN_PRODUCT_FILE "${CMAKE_CURRENT_LIST_DIR}/products/replaynes.cmake" CACHE FILEPATH "Product profile CMake file")
include("${RN_PRODUCT_FILE}")
# Optional release identity. Read the root project rather than PROJECT_VERSION: this file
# also runs in the standalone Linux project before the root project is added.
if(NOT DEFINED RN_PRODUCT_VERSION)
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt" RN_PRODUCT_ROOT_PROJECT
    REGEX "^project\\(ReplayNES VERSION")
  string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" RN_PRODUCT_VERSION "${RN_PRODUCT_ROOT_PROJECT}")
endif()
if(NOT DEFINED RN_PRODUCT_WINDOWS_APPCAST_URL)
  set(RN_PRODUCT_WINDOWS_APPCAST_URL "https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml")
endif()
if(NOT DEFINED RN_PRODUCT_UPDATE_PUBLIC_KEY)
  # ReplayNES signs its Windows and macOS feeds with the same key.
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../apps/macos/project.yml" RN_PRODUCT_SU_KEY_LINE REGEX "SUPublicEDKey:")
  string(REGEX REPLACE ".*SUPublicEDKey: *\"?([A-Za-z0-9+/=]+)\"?.*" "\\1"
    RN_PRODUCT_UPDATE_PUBLIC_KEY "${RN_PRODUCT_SU_KEY_LINE}")
endif()
# Optional: frames from power-on to the library picture of a game without a project (an arcade
# board's start-up self test lasts longer than an NES game's first screen).
if(NOT DEFINED RN_PRODUCT_LIBRARY_PICTURE_FRAME)
  set(RN_PRODUCT_LIBRARY_PICTURE_FRAME 300)
endif()
if(NOT DEFINED RN_PRODUCT_FLASH_FILTER)
  set(RN_PRODUCT_FLASH_FILTER ON)
endif()
if(NOT DEFINED RN_PRODUCT_OVERSCAN_CROP)
  set(RN_PRODUCT_OVERSCAN_CROP ON)
endif()
# Optional: another window in front pauses the game and keeps the controllers away from it, with
# Settings > System > More > Play in the Background to keep playing instead (off: plays on, as always).
if(NOT DEFINED RN_PRODUCT_FOCUS_PAUSE)
  set(RN_PRODUCT_FOCUS_PAUSE OFF)
endif()
# Optional Windows branding and update capability; old external profiles remain valid.
if(NOT DEFINED RN_PRODUCT_WINDOWS_ICON)
  set(RN_PRODUCT_WINDOWS_ICON "${CMAKE_CURRENT_LIST_DIR}/../apps/windows/resources/replaynes.ico")
endif()
if(NOT DEFINED RN_PRODUCT_UPDATES)
  set(RN_PRODUCT_UPDATES ON)
endif()
foreach(key VERSION WINDOWS_APPCAST_URL UPDATE_PUBLIC_KEY NAME APP_ID EXECUTABLE DATA_DIR ENV_PREFIX PROJECT_EXT PROJECT_FORMAT ROM_EXTS TEMP_PREFIX L10N_OVERRIDES LINUX_DESKTOP_FILE LINUX_METAINFO_FILE STEAM_ARTWORK_DIR STARTUP_TARGET DEFAULT_CORE GAMEDB NES)
  if(NOT DEFINED RN_PRODUCT_${key})
    message(FATAL_ERROR "Product profile must define RN_PRODUCT_${key}")
  endif()
  string(REPLACE "\\" "\\\\" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}}")
  string(REPLACE "\"" "\\\"" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
  string(REPLACE "\n" "\\n" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
  string(REPLACE "\r" "\\r" RN_PRODUCT_${key}_C "${RN_PRODUCT_${key}_C}")
endforeach()
