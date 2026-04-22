add_subdirectory(IlmCtl)
add_subdirectory(IlmCtlMath)
add_subdirectory(IlmCtlSimd)
# IlmCtlDebug self-gates on CTL_ENABLE_DEBUGGER (early-returns if OFF), so
# unconditional add_subdirectory is safe.
add_subdirectory(IlmCtlDebug)
add_subdirectory(IlmImfCtl)
add_subdirectory(dpx)
if (CTL_BUILD_METAL_BACKEND)
	add_subdirectory(IlmCtlMetal)
endif()
