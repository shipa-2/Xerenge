set(FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/minimal.xex")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
        "import struct,sys; b=bytearray(0x180); struct.pack_into('>6I',b,0,0x58455832,1,0x20,0,0x20,1); struct.pack_into('>2I',b,0x20,0x120,0x1000); open(sys.argv[1],'wb').write(b)"
        "${FIXTURE}"
    RESULT_VARIABLE CREATE_RESULT
)
if(NOT CREATE_RESULT EQUAL 0)
    message(FATAL_ERROR "could not create XEX fixture: ${CREATE_RESULT}")
endif()
execute_process(
    COMMAND "${XERENGE_RUNTIME}" --validate "${FIXTURE}"
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR
)
if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "XEX validation failed: ${RESULT}\n${OUTPUT}\n${ERROR}")
endif()
