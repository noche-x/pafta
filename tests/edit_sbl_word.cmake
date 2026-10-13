# SPDX-License-Identifier: GPL-2.0-only
if(NOT DEFINED PAFTA_EXE)
  message(FATAL_ERROR "PAFTA_EXE is required")
endif()
if(NOT DEFINED FIXTURE)
  message(FATAL_ERROR "FIXTURE is required")
endif()
if(NOT DEFINED WORK)
  message(FATAL_ERROR "WORK is required")
endif()

file(MAKE_DIRECTORY "${WORK}")
set(input "${WORK}/edit_sbl_word_input.sbl")
set(output "${WORK}/edit_sbl_word_output.sbl")
file(COPY_FILE "${FIXTURE}" "${input}" ONLY_IF_DIFFERENT)

execute_process(
  COMMAND "${PAFTA_EXE}" --set-sbl-word "${input}" 1 77 "${output}"
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
  COMMAND "${PAFTA_EXE}" --dump-sbl-word "${output}" 1
  RESULT_VARIABLE dump_result
  OUTPUT_VARIABLE dump_output
  ERROR_VARIABLE dump_error
)
if(NOT dump_result EQUAL 0)
  message(FATAL_ERROR "dump failed: ${dump_output}\n${dump_error}")
endif()
if(NOT dump_output MATCHES "value=77")
  message(FATAL_ERROR "edited word was not persisted: ${dump_output}")
endif()
