if(NOT DEFINED TOAD_SOURCE_ROOT)
  message(FATAL_ERROR "TOAD_SOURCE_ROOT is required")
endif()

file(GLOB MIR_RUNTIME_SOURCES
  "${TOAD_SOURCE_ROOT}/script/src/mir/*.c")
list(APPEND MIR_RUNTIME_SOURCES
  "${TOAD_SOURCE_ROOT}/script/src/mir.c")

set(canonical_uses 0)
foreach(source IN LISTS MIR_RUNTIME_SOURCES)
  file(READ "${source}" content)
  string(FIND "${content}" "toad_value_destroy(" direct_destroy)
  if(NOT direct_destroy EQUAL -1)
    message(FATAL_ERROR
      "MIR source bypasses canonical Engine value lifecycle: ${source}")
  endif()

  string(REGEX MATCHALL
    "cmeta_data_value_destroy\\(toad_value_cmeta_data\\(\\),"
    matches "${content}")
  list(LENGTH matches match_count)
  math(EXPR canonical_uses "${canonical_uses} + ${match_count}")
endforeach()

if(canonical_uses LESS 1)
  message(FATAL_ERROR
    "MIR sources do not consume toad_value_cmeta_data() lifecycle")
endif()

message(STATUS
  "MIR canonical Engine value lifecycle contract: ${canonical_uses} cleanup sites")
