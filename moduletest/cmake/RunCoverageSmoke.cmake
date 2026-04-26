# Driver for the ctltest_coverage_smoke ctest case.
# Invoked via: cmake -DCTLTEST=<path> -DSUITE=<path> -DOUTFILE=<path> -P RunCoverageSmoke.cmake

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

# SF: must reference our fixture
if(NOT body MATCHES "SF:.*coverage_smoke\\.ctl")
    message(FATAL_ERROR
        "expected SF: record for coverage_smoke.ctl in ${OUTFILE}\n--- body ---\n${body}")
endif()

# False branch (line 13) executed: hit count > 0
if(NOT body MATCHES "\nDA:13,[1-9]")
    message(FATAL_ERROR
        "expected DA:13,N (false-branch hit) with N>=1 in ${OUTFILE}\n--- body ---\n${body}")
endif()

# True branch (line 9) registered but NOT hit: DA:9,0
if(NOT body MATCHES "\nDA:9,0\n")
    message(FATAL_ERROR
        "expected DA:9,0 (true-branch unreached) in ${OUTFILE}\n--- body ---\n${body}")
endif()
