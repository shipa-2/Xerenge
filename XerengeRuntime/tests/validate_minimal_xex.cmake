set(FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/minimal.xex")
file(WRITE "${FIXTURE}" "XEX2")
execute_process(
    COMMAND "${XERENGE_RUNTIME}" --validate "${FIXTURE}"
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "XEX validation failed: ${RESULT}\n${OUTPUT}\n${ERROR}")
endif()
