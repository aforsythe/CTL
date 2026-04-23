# ctltest_core - shared library between ctltest_run_one (ctest driver)
# and ctltest (standalone CLI, v0.4+).
#
# All framework logic lives here; the driver and CLI are thin wrappers.
# C++17 is scoped to this target (the rest of the project is C++11).

set(CTLTEST_CORE_SOURCES
  CaseModel.cc
  Result.cc
  YamlLoader.cc
  ValueIO.cc
  CsvTable.cc
  Marshal.cc
  InterpRunner.cc
  Oracle.cc
  InlineOracle.cc
  CsvOracle.cc
  SnapshotOracle.cc
  ImageIO.cc
  ImageCompare.cc
  Reporter.cc
  ConsoleReporter.cc
  TapReporter.cc
  JUnitReporter.cc
  TestKit.cc
  Runner.cc
)

add_library(ctltest_core STATIC ${CTLTEST_CORE_SOURCES})

target_compile_features(ctltest_core PUBLIC cxx_std_17)

target_include_directories(ctltest_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ctltest_core
  PUBLIC
    IlmCtl
    IlmCtlSimd
    IlmCtlMath
    # CtlExc.h (public, via CTL install path) pulls in <IexBaseExc.h>; IlmCtl
    # exposes Imath/Iex as PRIVATE, so consumers must propagate Iex themselves.
    $<$<TARGET_EXISTS:Imath::Imath>:Imath::Imath>
    $<$<TARGET_EXISTS:Imath::Half>:Imath::Half>
    $<$<TARGET_EXISTS:OpenEXR::OpenEXR>:OpenEXR::OpenEXR>
    $<$<TARGET_EXISTS:OpenEXR::Iex>:OpenEXR::Iex>
    $<$<TARGET_EXISTS:OpenEXR::IlmImf>:OpenEXR::IlmImf>
    $<$<TARGET_EXISTS:IlmBase::Imath>:IlmBase::Imath>
    $<$<TARGET_EXISTS:IlmBase::Half>:IlmBase::Half>
    $<$<TARGET_EXISTS:IlmBase::Iex>:IlmBase::Iex>
  PRIVATE
    ${CTLTEST_YAML_CPP_TARGET}
)
