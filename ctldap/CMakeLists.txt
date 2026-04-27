# ctldap - DAP (Debug Adapter Protocol) server for CTL modules.
# Built only when CTL_ENABLE_DEBUGGER=ON.

if(NOT CTL_ENABLE_DEBUGGER)
    return()
endif()

set(CTLDAP_SOURCES
    DapDebugger.cc
    DapHandlers.cc
    DapServer.cc
    DapTransport.cc
    main.cc
)

add_executable(ctldap ${CTLDAP_SOURCES})

target_include_directories(ctldap
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ctldap
    PRIVATE
        IlmCtlDebug
        IlmCtlSimd
        IlmCtl
        $<$<TARGET_EXISTS:OpenEXR::OpenEXR>:OpenEXR::OpenEXR>
        $<$<TARGET_EXISTS:Imath::Imath>:Imath::Imath>
        $<$<TARGET_EXISTS:Imath::Half>:Imath::Half>
        $<$<TARGET_EXISTS:OpenEXR::IlmImf>:OpenEXR::IlmImf>
        $<$<TARGET_EXISTS:IlmBase::Imath>:IlmBase::Imath>
        $<$<TARGET_EXISTS:IlmBase::Half>:IlmBase::Half>
        $<$<TARGET_EXISTS:IlmBase::IlmThread>:IlmBase::IlmThread>
        $<$<TARGET_EXISTS:IlmBase::Iex>:IlmBase::Iex>
)

# Reuse ctldb's ValueFormatter (build it as an OBJECT library or duplicate
# the source compile here).  Simplest: include the cc directly.
target_sources(ctldap PRIVATE
    ${CMAKE_SOURCE_DIR}/ctldb/ValueFormatter.cc
)
target_include_directories(ctldap PRIVATE ${CMAKE_SOURCE_DIR}/ctldb)

install(TARGETS ctldap
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

find_program(BASH_EXE bash REQUIRED)
add_test(
    NAME ctldap::session
    COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dap_session.sh
)
set_tests_properties(ctldap::session PROPERTIES
    ENVIRONMENT "CTLDAP=$<TARGET_FILE:ctldap>"
    LABELS "ctldap")

add_test(
    NAME ctldap::chain_session
    COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dap_chain_session.sh
)
set_tests_properties(ctldap::chain_session PROPERTIES
    ENVIRONMENT "CTLDAP=$<TARGET_FILE:ctldap>"
    LABELS "ctldap")

add_test(
    NAME ctldap::transport_robustness
    COMMAND ${BASH_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dap_transport_robustness.sh
)
set_tests_properties(ctldap::transport_robustness PROPERTIES
    ENVIRONMENT "CTLDAP=$<TARGET_FILE:ctldap>"
    LABELS "ctldap")

# Headless DAP exercise of the VS Code demo flow (F5/F10/F11 sequences).
# Self-contained Python harness — fixtures, scripted DAP messages, and
# response assertions all live in run_demo_scenarios.py.
find_program(PYTHON3_EXE python3 REQUIRED)
add_test(
    NAME ctldap::demo_scenarios
    COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_demo_scenarios.py
            $<TARGET_FILE:ctldap>
)
set_tests_properties(ctldap::demo_scenarios PROPERTIES
    LABELS "ctldap")
