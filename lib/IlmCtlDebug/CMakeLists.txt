# IlmCtlDebug — shared utilities used by the CTL debuggers (ctldb CLI
# and ctldap DAP server).  Both binaries link against this library so
# the expression evaluator, pixel-input binder, and color-evolution
# formatter live in exactly one place.
#
# Built only when CTL_ENABLE_DEBUGGER=ON (matches when ctldb / ctldap
# themselves are built).

if(NOT CTL_ENABLE_DEBUGGER)
    return()
endif()

set(SOURCES
    CtlExprEval.cc
    CtlPixelBinder.cc
    CtlPixelEvolution.cc
    CtlStageChain.cc
    CtlValueFormatter.cc
)

add_library(IlmCtlDebug ${SOURCES})

target_include_directories(IlmCtlDebug
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(IlmCtlDebug
    PUBLIC
        IlmCtlSimd
        IlmCtl
    PRIVATE
        # For OpenEXR/Imath 3.x:
        $<$<TARGET_EXISTS:OpenEXR::OpenEXR>:OpenEXR::OpenEXR>
        $<$<TARGET_EXISTS:Imath::Imath>:Imath::Imath>
        $<$<TARGET_EXISTS:Imath::Half>:Imath::Half>
        # For OpenEXR 2.4/2.5:
        $<$<TARGET_EXISTS:OpenEXR::IlmImf>:OpenEXR::IlmImf>
        $<$<TARGET_EXISTS:IlmBase::Imath>:IlmBase::Imath>
        $<$<TARGET_EXISTS:IlmBase::Half>:IlmBase::Half>
        $<$<TARGET_EXISTS:IlmBase::IlmThread>:IlmBase::IlmThread>
        $<$<TARGET_EXISTS:IlmBase::Iex>:IlmBase::Iex>
)

set_target_properties(IlmCtlDebug
    PROPERTIES
        VERSION   "${CTL_VERSION}"
        SOVERSION "${CTL_VERSION_MAJOR}.${CTL_VERSION_MINOR}"
)

install(FILES
            CtlExprEval.h
            CtlPixelBinder.h
            CtlPixelEvolution.h
            CtlStageChain.h
            CtlValueFormatter.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CTL)

install(TARGETS IlmCtlDebug DESTINATION ${CMAKE_INSTALL_LIBDIR})
