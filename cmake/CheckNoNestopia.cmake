# Check every built executable/archive, including undefined references in archives.
set_property(GLOBAL PROPERTY RN_NO_NESTOPIA_SCRIPT
  "${CMAKE_CURRENT_LIST_DIR}/../tests/check_no_nestopia_symbols.cmake")
function(rn_check_no_nestopia_targets directory)
  if(RN_PRODUCT_NES)
    return()
  endif()
  if(NOT CMAKE_NM)
    message(FATAL_ERROR "NES-OFF products require nm for the Nestopia symbol check")
  endif()
  get_property(script GLOBAL PROPERTY RN_NO_NESTOPIA_SCRIPT)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    get_target_property(checked ${target} RN_NO_NESTOPIA_CHECKED)
    if(NOT checked AND type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY)$")
      # A separate ALL target works across directory scopes on CMake 3.16.
      add_custom_target(${target}_no_nestopia ALL
        COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}"
          "-DARTIFACT=$<TARGET_FILE:${target}>" -P "${script}"
        DEPENDS ${target} VERBATIM)
      add_test(NAME ${target}_no_nestopia
        COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}"
          "-DARTIFACT=$<TARGET_FILE:${target}>" -P "${script}")
      set_property(TARGET ${target} PROPERTY RN_NO_NESTOPIA_CHECKED TRUE)
    endif()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    rn_check_no_nestopia_targets("${child}")
  endforeach()
endfunction()
