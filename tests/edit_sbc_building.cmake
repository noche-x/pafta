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
set(input "${WORK}/edit_sbc_building_input.sbc")
set(output "${WORK}/edit_sbc_building_output.sbc")
file(COPY_FILE "${FIXTURE}" "${input}" ONLY_IF_DIFFERENT)

execute_process(
  COMMAND "${PAFTA_EXE}" --set-sbc-building "${input}" 0 x 4321 "${output}"
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
  COMMAND "${PAFTA_EXE}" --dump-sbc-building "${output}" 0
  RESULT_VARIABLE dump_result
  OUTPUT_VARIABLE dump_output
  ERROR_VARIABLE dump_error
)
if(NOT dump_result EQUAL 0)
  message(FATAL_ERROR "dump failed: ${dump_output}\n${dump_error}")
endif()
if(NOT dump_output MATCHES "x=4321")
  message(FATAL_ERROR "edited x was not persisted: ${dump_output}")
endif()
