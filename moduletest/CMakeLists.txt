# moduletest/ - ctltest framework for user-authored CTL modules
#
# Layout:
#   lib/         ctltest_core static library (case model, loader, runner, oracles)
#   driver/      ctltest_run_one, internal binary invoked by ctest
#   cli/         ctltest, installed alongside ctlrender (v0.4+)
#   cmake/       CtlTestDiscover.cmake, exposes ctltest_add_yaml(path)
#   tests/       manifest.txt + self-test YAMLs + CTL fixtures
#
# Dependency: yaml-cpp (find_package preferred, FetchContent fallback).

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CtlTestDiscover.cmake)

option(CTLTEST_FETCH_YAML_CPP
  "If yaml-cpp is not found via find_package, fetch and build it via FetchContent"
  ON)

find_package(yaml-cpp CONFIG QUIET)
if(yaml-cpp_FOUND OR TARGET yaml-cpp::yaml-cpp OR TARGET yaml-cpp)
  message(STATUS "yaml-cpp found via find_package")
elseif(CTLTEST_FETCH_YAML_CPP)
  message(STATUS "yaml-cpp not found; fetching via FetchContent")
  include(FetchContent)
  set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
  set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
  # yaml-cpp 0.8.0's cmake_minimum_required is below CMake 4's floor; override
  # the policy version so the nested project configures on new CMake.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
  )
  FetchContent_MakeAvailable(yaml-cpp)
else()
  message(FATAL_ERROR
    "CTL_BUILD_MODULETEST=ON but yaml-cpp not found and "
    "CTLTEST_FETCH_YAML_CPP=OFF. Install yaml-cpp "
    "(apt libyaml-cpp-dev / brew yaml-cpp / vcpkg yaml-cpp) "
    "or enable CTLTEST_FETCH_YAML_CPP.")
endif()

# Pick whichever target name the installed/fetched yaml-cpp provides.
if(TARGET yaml-cpp::yaml-cpp)
  set(CTLTEST_YAML_CPP_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
  set(CTLTEST_YAML_CPP_TARGET yaml-cpp)
else()
  message(FATAL_ERROR "yaml-cpp was located but exports no recognizable target")
endif()

add_subdirectory(lib)
add_subdirectory(driver)
add_subdirectory(cli)
add_subdirectory(unittest)

# Manifest-driven ctest registration.
# Adding a test = appending one line to tests/manifest.txt (git-tracked).
# No file(GLOB) at configure time -> no silent reconfiguration surprises.
set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/tests/manifest.txt")
if(EXISTS "${_manifest}")
  file(STRINGS "${_manifest}" _manifest_entries)
  foreach(_entry IN LISTS _manifest_entries)
    string(STRIP "${_entry}" _entry)
    if(_entry AND NOT _entry MATCHES "^#")
      ctltest_add_yaml("${_entry}")
    endif()
  endforeach()
endif()

# End-to-end ctest cases for ctltest --coverage. Only meaningful in coverage
# builds, so gated on CTL_ENABLE_COVERAGE. All three cases share the
# RunCoverageSmokeAssertions.cmake driver and pass their own MUST_MATCH
# regex list — adding another case is one add_test() call.
if(CTL_ENABLE_COVERAGE)
    set(_cov_driver ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunCoverageSmokeAssertions.cmake)

    # Single-case smoke: only the false branch is reached → DA:13,N>0,
    # DA:9,0 (true branch unreached).
    add_test(
        NAME ctltest::coverage_smoke
        COMMAND ${CMAKE_COMMAND}
            -DCTLTEST=$<TARGET_FILE:ctltest>
            -DSUITE=${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest/coverage_smoke.yaml
            -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/coverage_smoke.info
            -DMUST_MATCH=SF:.*coverage_smoke\\.ctl;\nDA:13,[1-9];\nDA:9,0\n
            -P ${_cov_driver}
    )

    # Multi-case aggregation: two cases on the same module with opposite
    # values of `negate` cover BOTH branches → DA:13,N>0 AND DA:9,N>0.
    add_test(
        NAME ctltest::coverage_smoke_multi_case
        COMMAND ${CMAKE_COMMAND}
            -DCTLTEST=$<TARGET_FILE:ctltest>
            -DSUITE=${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest/coverage_smoke_multi_case.yaml
            -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/coverage_smoke_multi_case.info
            -DMUST_MATCH=SF:.*coverage_smoke\\.ctl;\nDA:9,[1-9];\nDA:13,[1-9]
            -P ${_cov_driver}
    )

    # Multi-module aggregation: two cases hit functions in two different
    # modules; lcov output must contain SF: blocks for BOTH source files.
    add_test(
        NAME ctltest::coverage_smoke_multi_module
        COMMAND ${CMAKE_COMMAND}
            -DCTLTEST=$<TARGET_FILE:ctltest>
            -DSUITE=${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest/coverage_smoke_multi_module.yaml
            -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/coverage_smoke_multi_module.info
            -DMUST_MATCH=SF:.*coverage_smoke\\.ctl;SF:.*coverage_helper\\.ctl
            -P ${_cov_driver}
    )

    set_tests_properties(
        ctltest::coverage_smoke
        ctltest::coverage_smoke_multi_case
        ctltest::coverage_smoke_multi_module
        PROPERTIES LABELS "ctltest"
    )
endif()
