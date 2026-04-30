set(SOURCES
	CtlMetalAddr.cpp
	CtlMetalCodegen.cpp
	CtlMetalDevice.mm
	CtlMetalDispatch.mm
	CtlMetalFunctionCall.cpp
	CtlMetalHalfExpLogTable.cpp
	CtlMetalInterpreter.mm
	CtlMetalLContext.cpp
	CtlMetalModule.cpp
	CtlMetalShaderCache.mm
	CtlMetalSidecarCache.cpp
	CtlMetalStdLibrary.cpp
	CtlMetalSyntaxTree.cpp
	CtlMetalType.cpp
)

add_library(IlmCtlMetal ${SOURCES})

set_source_files_properties(
	CtlMetalDevice.mm
	CtlMetalDispatch.mm
	CtlMetalInterpreter.mm
	CtlMetalShaderCache.mm
	PROPERTIES
		COMPILE_FLAGS "-fobjc-arc"
)

target_include_directories(IlmCtlMetal
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(IlmCtlMetal
	PUBLIC
		IlmCtlMath
		IlmCtl
	PRIVATE
		IlmCtlSimd
		"-framework Foundation"
		"-framework Metal"
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

set_target_properties(IlmCtlMetal
	PROPERTIES
		VERSION "${CTL_VERSION}"
		SOVERSION "${CTL_VERSION_MAJOR}.${CTL_VERSION_MINOR}"
)

install(FILES CtlMetalInterpreter.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/CTL)
install(TARGETS IlmCtlMetal DESTINATION ${CMAKE_INSTALL_LIBDIR})
