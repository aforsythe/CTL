# Regression test for ctlrender-metal's -jobs>1 behavior in non-parity
# mode. The historic clamp-to-1 was removed once the MetalInterpreterCache
# and MetalFunctionCall were made thread-safe; this test guards against
# regressions in that concurrency contract.
#
# Expected contract:
#   1) ctlrender-metal -jobs N (N > 1) exits cleanly on a multi-file
#      batch.
#   2) Every input file produces its corresponding output (no races on
#      output-path setup or pipeline state).
#   3) No crash or hang — if someone introduces shared mutable state
#      without a guarding mutex, concurrent workers would surface it as
#      a sporadic crash/wrong-output. Running the batch with a larger
#      -jobs than there are input files exercises any worker-contention
#      paths (second worker starts with the file vector empty).
#
# Inputs (passed via -D):
#   CTLRENDER_METAL  - path to ctlrender-metal executable
#   UNITY_CTL        - path to unity.ctl
#   INPUT_DIR        - directory containing frame_*.exr inputs
#   OUTPUT_DIR       - directory to write outputs into (wiped + recreated)
#   JOBS_REQUESTED   - integer >= 2 to pass to -jobs

if(NOT CTLRENDER_METAL OR NOT UNITY_CTL OR NOT INPUT_DIR
   OR NOT OUTPUT_DIR OR NOT JOBS_REQUESTED)
    message(FATAL_ERROR
        "Required: -DCTLRENDER_METAL -DUNITY_CTL -DINPUT_DIR "
        "-DOUTPUT_DIR -DJOBS_REQUESTED")
endif()

file(GLOB _inputs "${INPUT_DIR}/*.exr")
list(LENGTH _inputs _input_count)
if(_input_count LESS 2)
    message(FATAL_ERROR
        "Need >= 2 input EXRs in ${INPUT_DIR}; found ${_input_count}.")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")

execute_process(
    COMMAND "${CTLRENDER_METAL}"
            -jobs ${JOBS_REQUESTED}
            -ctl "${UNITY_CTL}"
            -format exr32
            -force
            ${_inputs}
            "${OUTPUT_DIR}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr
)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "ctlrender-metal exited ${_rc} (expected 0).\n"
        "stderr:\n${_stderr}")
endif()

file(GLOB _outputs "${OUTPUT_DIR}/*.exr")
list(LENGTH _outputs _output_count)
if(NOT _output_count EQUAL _input_count)
    message(FATAL_ERROR
        "Expected ${_input_count} output files in ${OUTPUT_DIR}, "
        "got ${_output_count}. stderr:\n${_stderr}")
endif()

# Non-parity -jobs>1 must no longer emit a clamp warning. If one shows
# up, it means the clamp was re-introduced for the normal code path.
string(REGEX MATCH "clamping to -jobs=1" _has_clamp "${_stderr}")
if(_has_clamp)
    message(FATAL_ERROR
        "Unexpected clamp warning in stderr — the non-parity -jobs>1 "
        "path should run the batch in parallel now, not clamp.\n"
        "stderr:\n${_stderr}")
endif()
