if(NOT DEFINED MAP_EDITOR_CPP_EXE)
  message(FATAL_ERROR "MAP_EDITOR_CPP_EXE is required")
endif()
if(NOT DEFINED FIXTURE)
  message(FATAL_ERROR "FIXTURE is required")
endif()
if(NOT DEFINED WORK)
  message(FATAL_ERROR "WORK is required")
endif()

file(MAKE_DIRECTORY "${WORK}")
set(input "${WORK}/edit_sbl_layer_input.sbl")
set(output "${WORK}/edit_sbl_layer_output.sbl")
file(COPY_FILE "${FIXTURE}" "${input}" ONLY_IF_DIFFERENT)

execute_process(
  COMMAND "${MAP_EDITOR_CPP_EXE}" --set-sbl-layer-portal "${input}" 1 123 "${output}"
  RESULT_VARIABLE set_result
  OUTPUT_VARIABLE set_output
  ERROR_VARIABLE set_error
)
if(NOT set_result EQUAL 0)
  message(FATAL_ERROR "set failed: ${set_output}\n${set_error}")
endif()
if(NOT EXISTS "${output}")
  message(FATAL_ERROR "edit command did not create output")
endif()

execute_process(
  COMMAND "${MAP_EDITOR_CPP_EXE}" --dump-sbl-layer "${output}" 1
  RESULT_VARIABLE dump_result
  OUTPUT_VARIABLE dump_output
  ERROR_VARIABLE dump_error
)
if(NOT dump_result EQUAL 0)
  message(FATAL_ERROR "dump failed: ${dump_output}\n${dump_error}")
endif()
if(NOT dump_output MATCHES "portal_type=123")
  message(FATAL_ERROR "edited portal type was not persisted: ${dump_output}")
endif()
