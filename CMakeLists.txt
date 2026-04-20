cmake_minimum_required(VERSION 3.12)
project(CTL VERSION 1.5.5 
  DESCRIPTION "Color Transformation Language (CTL) reference implementation"
  LANGUAGES CXX)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)

set(CMAKE_MODULE_PATH
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules"
	${CMAKE_MODULE_PATH}
)

add_definitions( -DPACKAGE="${PROJECT_NAME}" -DVERSION="${PROJECT_VERSION}" )

option(CTL_BUILD_TESTS "Build the unit tests" ON)
option(CTL_BUILD_TOOLS "Build the utility commands (ctlrender, etc)" ON)
option(BUILD_SHARED_LIBS "Build all libraries as shared" OFF )

# Vectorized stdlib transcendentals.  The fast path routes the 4096-lane
# SIMD inner loop through a batched vendor library instead of scalar libm
# per lane.  Precision contract: <=1 ULP vs platform libm per function (<=2
# ULP end-to-end through chained transcendentals, well inside every CTL
# fixture tolerance).
#
# Defaults are platform-picked so a bare `cmake -B build` gives the fast
# path on every tier-1 target:
#
#   APPLE (arm64 or x86_64): CTL_USE_ACCELERATE=ON.  Accelerate ships with
#       the OS; zero extra configure/build cost.
#   non-APPLE (Linux x86_64/arm64, Windows x86_64/arm64, BSDs): CTL_USE_SLEEF=ON.
#       sleef is looked up via find_package first (apt libsleef-dev, brew
#       sleef, vcpkg sleef); if not installed CTL_SLEEF_FETCH=ON clones and
#       builds sleef v3.6.1 from GitHub at configure time (~60s one-off).
#
# Opt out of the fast path with -DCTL_USE_ACCELERATE=OFF / -DCTL_USE_SLEEF=OFF.
# With both OFF the build is bit-exact against a scalar-libm reference on the
# same host -- useful for pipeline reproducibility against historical CTL
# releases.  Internal bit-exactness across -threads/-jobs values is unaffected
# by these flags.
if(APPLE)
  set(_ctl_use_accelerate_default ON)
  set(_ctl_use_sleef_default OFF)
else()
  set(_ctl_use_accelerate_default OFF)
  set(_ctl_use_sleef_default ON)
endif()

option(CTL_USE_ACCELERATE
  "Use Apple Accelerate vForce for vectorized stdlib transcendentals (APPLE only)"
  ${_ctl_use_accelerate_default})
if(CTL_USE_ACCELERATE AND NOT APPLE)
  message(WARNING "CTL_USE_ACCELERATE requested on non-Apple platform; ignored.")
  set(CTL_USE_ACCELERATE OFF CACHE BOOL "" FORCE)
endif()

# sleef (https://sleef.org) is the cross-platform alternative to Accelerate.
# When enabled, routes the same 15 transcendentals through sleef's SIMD
# entry points on x86_64 (SSE2 baseline) and arm64 (NEON).  Works on Linux,
# Windows, and macOS.  On macOS with both options ON, Accelerate takes
# per-function precedence (one backend per function, no runtime branching).
option(CTL_USE_SLEEF
  "Use sleef for vectorized stdlib transcendentals (cross-platform)"
  ${_ctl_use_sleef_default})
set(CTL_SLEEF_ULP "10" CACHE STRING
  "sleef ULP tier: 10 (<=1 ULP, matches Accelerate contract) or 35 (<=3.5 ULP, faster)")
set_property(CACHE CTL_SLEEF_ULP PROPERTY STRINGS "10" "35")
option(CTL_SLEEF_FETCH
  "If sleef is not found via find_package, fetch and build it via FetchContent"
  ON)

if(CTL_USE_SLEEF)
  if(NOT (CTL_SLEEF_ULP STREQUAL "10" OR CTL_SLEEF_ULP STREQUAL "35"))
    message(FATAL_ERROR "CTL_SLEEF_ULP must be \"10\" or \"35\" (got \"${CTL_SLEEF_ULP}\")")
  endif()

  find_package(sleef CONFIG QUIET)
  if(sleef_FOUND)
    message(STATUS "sleef found via find_package (version ${sleef_VERSION})")
  elseif(CTL_SLEEF_FETCH)
    message(STATUS "sleef not found; fetching via FetchContent")
    include(FetchContent)
    set(SLEEF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SLEEF_BUILD_DFT OFF CACHE BOOL "" FORCE)
    set(SLEEF_BUILD_QUAD OFF CACHE BOOL "" FORCE)
    set(SLEEF_BUILD_GNUABI_LIBS OFF CACHE BOOL "" FORCE)
    set(SLEEF_BUILD_SCALAR_LIB ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      sleef
      GIT_REPOSITORY https://github.com/shibatch/sleef.git
      GIT_TAG 3.6.1
    )
    FetchContent_MakeAvailable(sleef)
  else()
    message(FATAL_ERROR
      "CTL_USE_SLEEF=ON but sleef not found and CTL_SLEEF_FETCH=OFF. "
      "Install sleef (apt libsleef-dev / brew sleef / vcpkg sleef) "
      "or enable CTL_SLEEF_FETCH.")
  endif()

  if(APPLE AND CTL_USE_ACCELERATE)
    message(STATUS "Both CTL_USE_ACCELERATE and CTL_USE_SLEEF are ON. "
                   "Accelerate takes per-function precedence; sleef paths "
                   "compile in but are unreachable.")
  endif()
endif()

set(CMAKE_CXX_FLAGS_ASAN
  "-fsanitize=address -fno-optimize-sibling-calls -fsanitize-address-use-after-scope -fno-omit-frame-pointer -g -O1"
  CACHE STRING "Flags used by the C++ compiler during AddressSanitizer builds."
  FORCE)

if( NOT DEFINED CMAKE_BUILD_TYPE )
  MESSAGE( STATUS "To use Release mode, use \"cmake .. -DCMAKE_BUILD_TYPE=Release\"" )
  MESSAGE( STATUS "To use AddressSanitizer, use \"cmake .. -DCMAKE_BUILD_TYPE=asan\"" )
elseif( CMAKE_BUILD_TYPE STREQUAL "Release" )
  MESSAGE( STATUS "Using Release mode - CMAKE_BUILD_TYPE : \"${CMAKE_BUILD_TYPE}\"")
elseif( CMAKE_BUILD_TYPE STREQUAL "asan" )
  MESSAGE( STATUS "Using AddressSanitizer - CMAKE_BUILD_TYPE : \"${CMAKE_BUILD_TYPE}\"")
else()
  MESSAGE( STATUS "CMAKE_BUILD_TYPE : \"${CMAKE_BUILD_TYPE}\"")
  MESSAGE( STATUS "To use Release mode, use \"cmake .. -DCMAKE_BUILD_TYPE=Release\"" )
  MESSAGE( STATUS "To use AddressSanitizer, use \"cmake .. -DCMAKE_BUILD_TYPE=asan\"" )
endif()

# Set a default build type if none was specified
set(default_build_type "Release")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to '${default_build_type}' as none was specified.")
  set(CMAKE_BUILD_TYPE "${default_build_type}" CACHE
      STRING "Choose the type of build." FORCE)
  # Set the possible values of build type for cmake-gui
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# ZLIB is automatically added by OpenEXR 3.x but needs to be added for OpenEXR 2.x
find_package(ZLIB)
if(TARGET ZLIB::ZLIB)
	message( STATUS "found ZLIB" )
	message( STATUS "	ZLIB_VERSION : ${ZLIB_VERSION}" )
	message( STATUS "	ZLIB_INCLUDE_DIRS : ${ZLIB_INCLUDE_DIRS}" )
	message( STATUS "	ZLIB_LIBRARIES : ${ZLIB_LIBRARIES}" )
	message( STATUS "	ZLIB_INCLUDE_DIR : ${ZLIB_INCLUDE_DIR}" )
	message( STATUS "	ZLIB_LIBRARY : ${ZLIB_LIBRARY}" )
else()
  message( STATUS "ZLIB was not found")
  message( STATUS " ZLIB is automatically linked when using OpenEXR 3.x but CTL needs to link to ZLIB if using OpenEXR 2.x" )
endif()

# find OpenEXR
find_package(OpenEXR 3 CONFIG QUIET)
if(OpenEXR_FOUND)
  set(OpenEXR_FOUND_WITH_CONFIG TRUE BOOL "found OpenEXR with Config")
else()
  message(STATUS "OpenEXR v3 not found, looking for IlmBase and OpenEXR v2")
  find_package(IlmBase REQUIRED)
  find_package(OpenEXR 2 REQUIRED)
endif()
 
if(OpenEXR_FOUND)
  message(STATUS "Found OpenEXR ${OpenEXR_VERSION}")
  set(USE_OPENEXR TRUE CACHE BOOL "Add OpenEXR support")
  set(OPENEXR_VERSION_MAJOR ${OpenEXR_VERSION_MAJOR} CACHE STRING "OpenEXR major version")
  set(OPENEXR_VERSION_MINOR ${OpenEXR_VERSION_MINOR} CACHE STRING "OpenEXR minor version")
  set(OPENEXR_VERSION_PATCH ${OpenEXR_VERSION_PATCH} CACHE STRING "OpenEXR patch version")
  if(OpenEXR_FOUND_WITH_CONFIG)
    message(STATUS " found OpenEXR with Config")
  else(OpenEXR_FOUND_WITH_CONFIG)
    message(STATUS " found OpenEXR with FindModule")
  endif(OpenEXR_FOUND_WITH_CONFIG)
else(OpenEXR_FOUND)
  message(ERROR "OpenEXR is a required dependency but it has not been found")
endif(OpenEXR_FOUND)

if(IlmBase_FOUND)
  message(STATUS "IlmBase info:")
  message(STATUS "       IlmBase_VERSION = ${IlmBase_VERSION}")
  message(STATUS "   IlmBase_INCLUDE_DIR = ${IlmBase_INCLUDE_DIR}")
  message(STATUS "  IlmBase_INCLUDE_DIRS = ${IlmBase_INCLUDE_DIRS}")
  message(STATUS "       IlmBase_LIBRARY = ${IlmBase_LIBRARY}")
  message(STATUS "     IlmBase_LIBRARIES = ${IlmBase_LIBRARIES}")
endif(IlmBase_FOUND)

if(OpenEXR_FOUND)
  message(STATUS "OpenEXR info:")
  message(STATUS "       OpenEXR_VERSION = ${OpenEXR_VERSION}")
  if( NOT OpenEXR_FOUND_WITH_CONFIG)
    message(STATUS "   OpenEXR_INCLUDE_DIR = ${OpenEXR_INCLUDE_DIR}")
    message(STATUS "  OpenEXR_INCLUDE_DIRS = ${OpenEXR_INCLUDE_DIRS}")
    message(STATUS "       OpenEXR_LIBRARY = ${OpenEXR_LIBRARY}")
    message(STATUS "     OpenEXR_LIBRARIES = ${OpenEXR_LIBRARIES}")
  endif(NOT OpenEXR_FOUND_WITH_CONFIG)
endif(OpenEXR_FOUND)

add_subdirectory(lib)
add_subdirectory(doc)
if (CTL_BUILD_TOOLS)
  add_subdirectory(ctlrender)
  add_subdirectory(OpenEXR_CTL)
endif()

if (CTL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(unittest)
endif()

