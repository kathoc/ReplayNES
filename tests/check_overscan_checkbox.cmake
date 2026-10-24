# Inspect the compiled UI, not the source or the translation catalog (which retains all keys).
file(STRINGS "${ARCHIVE}" labels REGEX "Hide overscan")
string(FIND "${labels}" "Hide overscan (crop 8 px on each side)" found)
if(EXPECT_CROP AND found EQUAL -1)
  message(FATAL_ERROR "Enabled product lost its overscan export checkbox")
elseif(NOT EXPECT_CROP AND NOT found EQUAL -1)
  message(FATAL_ERROR "Disabled product still contains the overscan export checkbox")
endif()
