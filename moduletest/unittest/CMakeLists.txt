# ctltest_unit - C++ unit tests for ctltest_core's public API.
#
# These are distinct from the YAML self-test suites under tests/selftest/,
# which exercise the framework end-to-end via the ctltest_run_one driver.
# ctltest_unit pokes the library's public surface directly (Value,
# Tolerance::merge, compareTyped, validateTolerancePaths, CsvTable, YamlLoader,
# Marshal round-trips) so failures point at exactly the API that regressed.

add_executable(ctltest_unit ctltest_unit.cc)

target_link_libraries(ctltest_unit
  PRIVATE
    ctltest_core
)

# Marshal round-trip tests drive InterpRunner against the shared st_scalar.ctl
# fixture; the runner's setModulePaths({"."}) resolves against CWD, so copy the
# fixture next to the binary at configure time. Keeping the path relative means
# the binary works both from a plain build dir and from the install tree if
# copied out (self-tests already depend on this fixture being stable).
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/../tests/fixtures/st_scalar.ctl
  ${CMAKE_CURRENT_BINARY_DIR}/st_scalar.ctl
  COPYONLY
)

add_test(
  NAME    ctltest::unit
  COMMAND ctltest_unit
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# ctltest-unit label keeps these out of the ctltest label (which is for the
# YAML-driven self-tests). `ctest -L ctltest-unit` runs the C++ tier only.
set_tests_properties(ctltest::unit PROPERTIES LABELS "ctltest-unit")
