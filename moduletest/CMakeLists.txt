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
# No file(GLOB) at configure time to no silent reconfiguration surprises.
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
