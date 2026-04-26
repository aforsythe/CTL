add_executable(IlmCtlSimdTest
    main.cpp
    testCoverage.cpp
)

target_link_libraries(IlmCtlSimdTest
    PRIVATE
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

add_test(NAME IlmCtlSimdTest COMMAND IlmCtlSimdTest)
