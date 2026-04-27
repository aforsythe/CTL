# ctldb - single-pixel CLI debugger for CTL modules.
# Built only when CTL_ENABLE_DEBUGGER=ON.

if(NOT CTL_ENABLE_DEBUGGER)
    return()
endif()

set(CTLDB_SOURCES
    ValueFormatter.cc
    main.cc
    Repl.cc
)

add_executable(ctldb ${CTLDB_SOURCES})

target_include_directories(ctldb
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ctldb
    PRIVATE
        IlmCtlDebug
        IlmCtlSimd
        IlmCtl
        # OpenEXR/Imath for half/Imath types
        $<$<TARGET_EXISTS:OpenEXR::OpenEXR>:OpenEXR::OpenEXR>
        $<$<TARGET_EXISTS:Imath::Imath>:Imath::Imath>
        $<$<TARGET_EXISTS:Imath::Half>:Imath::Half>
        $<$<TARGET_EXISTS:OpenEXR::IlmImf>:OpenEXR::IlmImf>
        $<$<TARGET_EXISTS:IlmBase::Imath>:IlmBase::Imath>
        $<$<TARGET_EXISTS:IlmBase::Half>:IlmBase::Half>
        $<$<TARGET_EXISTS:IlmBase::IlmThread>:IlmBase::IlmThread>
        $<$<TARGET_EXISTS:IlmBase::Iex>:IlmBase::Iex>
)

install(TARGETS ctldb
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# End-to-end REPL session test (single stage).
add_test(
    NAME ctldb::session
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctldb_session.sh
)
set_tests_properties(ctldb::session PROPERTIES
    ENVIRONMENT "CTLDB=$<TARGET_FILE:ctldb>"
    LABELS "ctldb")

# End-to-end chain session test (two -ctl stages with rOut→rIn chaining).
add_test(
    NAME ctldb::chain_session
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_ctldb_chain_session.sh
)
set_tests_properties(ctldb::chain_session PROPERTIES
    ENVIRONMENT "CTLDB=$<TARGET_FILE:ctldb>"
    LABELS "ctldb")

# Headless multi-scenario REPL exercise (Python harness).  Self-
# contained: spawns ctldb, scripts REPL input, asserts on stdout.
find_program(PYTHON3_EXE python3 REQUIRED)
add_test(
    NAME ctldb::demo_scenarios
    COMMAND ${PYTHON3_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_demo_scenarios.py
            $<TARGET_FILE:ctldb>
)
set_tests_properties(ctldb::demo_scenarios PROPERTIES
    LABELS "ctldb")
