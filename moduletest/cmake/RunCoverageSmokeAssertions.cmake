# Parameterised driver for ctltest --coverage end-to-end ctest cases.
#
# Inputs (all required):
#   CTLTEST     — path to ctltest binary
#   SUITE       — path to YAML suite to run
#   OUTFILE     — path to write lcov .info
#   MUST_MATCH  — semicolon-separated list of regexes; every regex must
#                 appear somewhere in the produced .info body, else the
#                 driver fails the ctest with a diff-style error.
#
# Used by all ctltest::coverage_* ctest cases registered in
# moduletest/CMakeLists.txt under CTL_ENABLE_COVERAGE.

execute_process(
    COMMAND "${CTLTEST}" --coverage "${OUTFILE}" "${SUITE}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE  err
)

if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ctltest exited with ${rc}\n--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()

if(NOT EXISTS "${OUTFILE}")
    message(FATAL_ERROR "coverage file not written: ${OUTFILE}\n--- stderr ---\n${err}")
endif()

file(READ "${OUTFILE}" body)

foreach(_pattern IN LISTS MUST_MATCH)
    if(NOT body MATCHES "${_pattern}")
        message(FATAL_ERROR
            "expected regex '${_pattern}' to match ${OUTFILE}\n--- body ---\n${body}")
    endif()
endforeach()
