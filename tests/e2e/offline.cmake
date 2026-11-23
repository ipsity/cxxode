function(check_cli expected_exit expected_output)
    execute_process(
        COMMAND "${CLI}" ${ARGN}
        INPUT_FILE "${CMAKE_CURRENT_LIST_DIR}/../fixtures/question.txt"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 5)
    if(NOT "${status}" STREQUAL "${expected_exit}" OR NOT "${output}" STREQUAL "${expected_output}" OR error)
        message(FATAL_ERROR "Unexpected CLI result (${status}):\n${output}${error}")
    endif()
endfunction()

check_cli(0 "lay-code> [fixture read] config/defaults.txt\nconfig/defaults.txt reports: empty_config = allow\n[session] end_turn requests=2 dispatches=1\n")
check_cli(1 "lay-code> [session] protocol_error requests=1 dispatches=0\n" --truncated)

execute_process(
    COMMAND "${CLI}" --tui --demo approval
    INPUT_FILE "${CMAKE_CURRENT_LIST_DIR}/../fixtures/question.txt"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
if(NOT "${status}" STREQUAL "2" OR output OR NOT error MATCHES "interactive terminal")
    message(FATAL_ERROR "The TUI opened with redirected input or output (${status}):\n${output}${error}")
endif()
