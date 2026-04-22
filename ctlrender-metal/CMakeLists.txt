# ctlrender-metal — Metal-backed sibling of ctlrender.
#
# Builds the same sources as ctlrender/, but with CTL_GPU_BACKEND=1 so that
# transform.cc instantiates Ctl::MetalInterpreter instead of
# Ctl::SimdInterpreter. All format I/O, argument binding, CLI parsing, and
# the batch loop come from ctlrender/ verbatim — no source duplication, no
# refactor of ctlrender itself.

set(CTLRENDER_DIR ${CMAKE_SOURCE_DIR}/ctlrender)

set(SOURCES
	${CTLRENDER_DIR}/main.cc
	${CTLRENDER_DIR}/transform.cc
	${CTLRENDER_DIR}/usage.cc
	${CTLRENDER_DIR}/aces_file.cc
	${CTLRENDER_DIR}/dpx_file.cc
	${CTLRENDER_DIR}/exr_file.cc
	${CTLRENDER_DIR}/tiff_file.cc
	${CTLRENDER_DIR}/format.cc
	${CTLRENDER_DIR}/compression.cc
	${CMAKE_CURRENT_SOURCE_DIR}/parity.cc
	${CMAKE_CURRENT_SOURCE_DIR}/benchmark.cc
)

add_executable(ctlrender-metal ${SOURCES})

target_include_directories(ctlrender-metal
	PRIVATE
		${CTLRENDER_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(ctlrender-metal
	PRIVATE
		CTL_GPU_BACKEND=1
)

target_link_libraries(ctlrender-metal
	PRIVATE
		IlmCtlMetal
		IlmCtlSimd
		IlmCtlMath
		IlmCtl
		ctldpx
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
		# ZLIB for OpenEXR 2.x:
		$<$<TARGET_EXISTS:ZLIB::ZLIB>:ZLIB::ZLIB>
)

if(OpenEXR_FOUND)
	target_compile_definitions(ctlrender-metal
		PRIVATE
			-DHAVE_OPENEXR=1
			OPENEXR_VERSION_MAJOR=${OpenEXR_VERSION_MAJOR}
			OPENEXR_VERSION_MINOR=${OpenEXR_VERSION_MINOR}
	)
endif()

find_package(TIFF)
if(TARGET TIFF::TIFF)
	target_compile_definitions(ctlrender-metal
		PRIVATE
			-DHAVE_LIBTIFF=1
	)
	target_link_libraries(ctlrender-metal PRIVATE TIFF::TIFF)
endif()

find_package(AcesContainer)
if(AcesContainer_FOUND OR TARGET AcesContainer::AcesContainer)
	target_compile_definitions(ctlrender-metal
		PRIVATE
			-DHAVE_ACESFILE=1
	)
	target_link_libraries(ctlrender-metal
		PRIVATE
			AcesContainer::AcesContainer
	)
endif()

install(TARGETS ctlrender-metal DESTINATION ${CMAKE_INSTALL_BINDIR})
