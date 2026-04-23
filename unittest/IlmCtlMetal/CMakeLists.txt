# Metal backend unit tests: device acquisition, hello-world dispatch,
# language-level parity, stdlib precision, and CPU-vs-GPU benchmarks.

set(SOURCES
	main.cpp
	testMetalAcesV2Parity.cpp
	testMetalAcesV2RoundTrip.cpp
	testMetalArithmetic.cpp
	testMetalAssertFailure.cpp
	testMetalBenchmark.mm
	testMetalCodegen.cpp
	testMetalDefaultParams.cpp
	testMetalDispatch.cpp
	testMetalHalfExpLog.cpp
	testMetalHelloWorld.cpp
	testMetalIlmCtlFixtures.cpp
	testMetalLanguageRejections.cpp
	testMetalLContext.cpp
	testMetalNestedVSArrays.cpp
	testMetalParity.cpp
	testMetalReturnValue.cpp
	testMetalScatterKernel.cpp
	testMetalShaderCache.cpp
	testMetalSidecarCache.cpp
	testMetalStub.cpp
)

add_executable(IlmCtlMetalTest ${SOURCES})

target_include_directories(IlmCtlMetalTest
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(IlmCtlMetalTest
	PRIVATE
		IlmCtlMetal
		IlmCtlSimd
		IlmCtl
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

add_test(IlmCtlMetal IlmCtlMetalTest)

# IlmCtl fixtures used by testMetalIlmCtlFixtures. The bar for inclusion
# is that the fixture's CTL stdlib use lines up with symbols the Metal
# backend has registered — i.e. no `scatteredDataToGrid3D` (deferred) and
# no `pow_h` (deferred). Fixtures that call other stdlib helpers (assert,
# math, lookup/interp, colorspace) are in-scope because those land in
# the registered stdlib set. The test harness loads each fixture and probes a named
# function; that's enough to catch front-end ↔ Metal codegen regressions
# without requiring CPU-vs-GPU parity (covered by testMetalArithmetic /
# testMetalParity / testMetalAcesV2*).
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/common.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/example.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/test.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testArray.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testCast.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testComments.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testCppCall.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testCtlVersion.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testDefaults.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testEmpty.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testExamples.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testExamplesNamespace.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testExpr.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testFunc.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testHugeInit.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testInterpolator.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testLiterals.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testLookupTables.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testLoops.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testName.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testName2.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testNameSpace.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testNameSpace2.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testNoName.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testParse.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testScope.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testScope2.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testStruct.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testTypes.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testVarying.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testVaryingLookup.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testVaryingReturn.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../IlmCtl/testVSArrays.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

# ctlrender fixtures used by testMetalParity. These exercise the varying
# half-precision I/O path that real ctlrender transforms use.
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/unity.ctl
	${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/unity_with_alpha.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

# ACES v2 OutputTransform fixture used by testMetalAcesV2Parity. This is
# the flattened, VSArray-stripped subset that concatenates Utilities +
# Tonescale + OutputTransform with a Rec.709 wrapper — kept in-tree so
# the CI regression guard runs against a frozen snapshot.
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/aces_combined.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

# Nested-VSArray fixture used by testMetalNestedVSArrays. Exercises the
# `float[][3]` and `float[][][]` shapes (read + write + multi-dim) that
# the Metal backend emits as flat scalar pointer + length uniforms.
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/nestedVSArrays.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

# halfExpLog fixture used by testMetalHalfExpLog. Exercises the three-
# level call graph (top -> mid -> leaf) that forwards the three hoisted
# halfExpLog MTLBuffer pointers through two user-to-user call sites
# before the stdlib helper consumes them — the plumbing shape the ACES
# v2 OutputTransform relies on.
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/halfExpLog.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

# Kernel-reachable scatteredDataToGrid3D parity fixture. Small (n=5)
# identity-data RBF, evaluated inside a per-pixel varying function so
# the MSL port of the RBF solve + grid eval runs on each lane rather
# than being collapsed into a module-init value on the sidecar.
file(COPY
	${CMAKE_CURRENT_SOURCE_DIR}/scatterKernel.ctl
DESTINATION
	${CMAKE_CURRENT_BINARY_DIR}
)

#
# End-to-end real-image parity test for the full ACES v2 OutputTransform
# pipeline (Rec.709). Drives `ctlrender-metal --parity-check-ulp` with
# aces_combined.ctl on a real EXR fixture, runs both backends
# side-by-side, asserts the GPU output stays within the documented ULP
# bound. Complements testMetalAcesV2Parity (1024 synthetic samples) —
# natural imagery hits far fewer CAM16 opponent-axis cancellation
# edge cases (colorbars_nuke_rgb_exr32.exr: 11 ULP / 26% diverged on
# 2026-04-19, M4 Max; the synthetic worst case is 55 ULP / 46% diverged).
# The 64-ULP threshold is ~6× headroom above the real-image measurement,
# tight enough to catch a regression that reverts any `ctl_stdlib_*`
# port back to its `metal::precise::*` baseline.
#
if(TARGET ctlrender-metal AND TARGET OpenEXR::OpenEXR OR TARGET OpenEXR::IlmImf)
	set(_aces_ctl "${CMAKE_CURRENT_SOURCE_DIR}/aces_combined.ctl")
	set(_aces_in  "${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/colorbars_nuke_rgb_exr32.exr")
	set(_aces_out "${CMAKE_CURRENT_BINARY_DIR}/aces_v2_parity.exr")
	add_test(NAME "ctlrender-metal-aces-v2-parity"
		COMMAND $<TARGET_FILE:ctlrender-metal>
			--parity-check-ulp 64
			-ctl "${_aces_ctl}"
			-format exr32
			-force
			"${_aces_in}"
			"${_aces_out}")
endif()

#
# Regression test for the ctlrender-metal -jobs clamp. The Metal backend
# shares one MetalInterpreter + MTLCommandQueue across the batch, so
# concurrent transform() calls from multiple file workers race and
# SIGSEGV. main.cc clamps file_jobs to 1 on the Metal binary and prints
# a warning to stderr; if someone removes that clamp without also making
# the shared state concurrency-safe, this test catches it.
#
# Exercises >= 2 input files with -jobs 4 and asserts:
#   - the process exits cleanly
#   - all inputs produced an output
#   - stderr contains "clamping to -jobs=1"
#   - stderr echoes the requested -jobs=N
#
if(TARGET ctlrender-metal AND (TARGET OpenEXR::OpenEXR OR TARGET OpenEXR::IlmImf))
	file(COPY
		${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/colorbars_nuke_rgb_exr32.exr
		${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/colorbars_nuke_rgba_exr32.exr
		${CMAKE_CURRENT_SOURCE_DIR}/../ctlrender/colorbars_nuke_rgb_exr16.exr
	DESTINATION
		${CMAKE_CURRENT_BINARY_DIR}/jobs_clamp_inputs
	)
	add_test(NAME "ctlrender-metal-jobs-clamp"
		COMMAND ${CMAKE_COMMAND}
			-DCTLRENDER_METAL=$<TARGET_FILE:ctlrender-metal>
			-DUNITY_CTL=${CMAKE_CURRENT_BINARY_DIR}/unity.ctl
			-DINPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/jobs_clamp_inputs
			-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/jobs_clamp_outputs
			-DJOBS_REQUESTED=4
			-P ${CMAKE_CURRENT_SOURCE_DIR}/run_jobs_clamp_test.cmake)
endif()
