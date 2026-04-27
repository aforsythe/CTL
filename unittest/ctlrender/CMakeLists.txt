if(NOT DEFINED CTLRENDER_PATH)
    set(CTLRENDER_PATH "${CMAKE_BINARY_DIR}/ctlrender/ctlrender")
    message("ctlrender test executable: ${CTLRENDER_PATH}")
endif()

find_package(TIFF)
if ( TARGET TIFF::TIFF AND OpenEXR_FOUND )
    message(STATUS "LibTIFF found, OpenEXR found, including TIF and EXR files in ctlrender unit tests")
elseif( TARGET TIFF::TIFF AND NOT OpenEXR_FOUND )
    message(WARNING "LibTIFF found, OpenEXR not found, including TIF files in ctlrender unit tests")
elseif( NOT TARGET TIFF::TIFF AND OpenEXR_FOUND )
    message(WARNING "LibTIFF not found, OpenEXR found, including EXR files in ctlrender unit tests")
elseif( NOT TARGET TIFF::TIFF AND NOT OpenEXR_FOUND )
    message(WARNING "LibTIFF not found, OpenEXR not found, not including TIF or EXR files in ctlrender unit tests")
endif()

find_package(AcesContainer)
if( AcesContainer_FOUND )
	message( STATUS "DEBUG: find_package(AcesContainer) AcesContainer_FOUND is true")
    message( STATUS "DEBUG: including ACES ctlrender unit tests")
endif()

set(CTLRENDER_OUTPUT_FOLDER "${CMAKE_BINARY_DIR}/unittest/ctlrender/output")
message("ctlrender test output folder will be created at: ${CTLRENDER_OUTPUT_FOLDER}")
add_custom_target(ctlrender-build-time-make-directory ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CTLRENDER_OUTPUT_FOLDER})

set(TEST_FILES "${PROJECT_SOURCE_DIR}/unittest/ctlrender")

foreach(J tiff8 tiff16 tiff32 dpx8 dpx10 dpx12 dpx16)
    foreach(I  bars_cinepaint_10.dpx bars_nuke_10_be.dpx bars_nuke_10_le.dpx bars_nuke_12_be.dpx bars_nuke_12_le.dpx bars_nuke_16_be.dpx bars_nuke_16_le.dpx bars_nuke_8_be.dpx bars_nuke_8_le.dpx bars_photoshop_16_be_interleaved.tif bars_photoshop_16_be_planar.tif bars_photoshop_16_le_interleaved.tif bars_photoshop_16_le_planar.tif bars_photoshop_32_be_interleaved.tif bars_photoshop_32_be_planar.tif bars_photoshop_32_le_interleaved.tif bars_photoshop_32_le_planar.tif bars_photoshop_8_be_interleaved.tif bars_photoshop_8_be_planar.tif bars_photoshop_8_le_interleaved.tif bars_photoshop_8_le_planar.tif)
        # Get extension from ${J}
        string(REGEX REPLACE "[0-9]+" "" EXTENSION "${J}")
        # Get filename from ${I}
        string(REGEX REPLACE "\\..+" "" FILE_NAME "${I}")
        # Get file extension from ${I}
        string(REGEX REPLACE ".+\\." "" FILE_NAME_EXTENSION "${I}")
        if (NOT TARGET TIFF::TIFF AND ${EXTENSION} STREQUAL "tiff")
            message("Skipping ${J} because tiff support was not detected")
        elseif(NOT TARGET TIFF::TIFF AND ${FILE_NAME_EXTENSION} STREQUAL "tif")
            message("Skipping ${J} because tiff support was not detected")
        else()
            add_test(NAME "ctlrender-${I}->${J}_unity" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format ${J} -force "${TEST_FILES}/${I}" "${CTLRENDER_OUTPUT_FOLDER}/${FILE_NAME}_${J}.${EXTENSION}")
        endif()
    endforeach(I)

    # Test tiff32 conversions
    if(TARGET TIFF::TIFF)
        # convert TIFF32 to other format
        add_test(NAME "ctlrender-tiff32->${J}" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format ${J} -force "${TEST_FILES}/bars_photoshop_32_be_planar.tif" "${CTLRENDER_OUTPUT_FOLDER}/bars_tiff32_${J}.${EXTENSION}")
    endif()
endforeach(J)

if(OpenEXR_FOUND)
    # Test EXR->EXR conversions
    add_test(NAME "ctlrender-EXR->EXR" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr -force "${TEST_FILES}/bars_photoshop.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_exr_exr.exr")
    add_test(NAME "ctlrender-EXR->EXR16" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force "${TEST_FILES}/bars_photoshop.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_exr_exr16.exr")
    add_test(NAME "ctlrender-EXR->EXR32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr32 -force "${TEST_FILES}/bars_photoshop.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_exr_exr32.exr")

    # RGB Tests
    add_test(NAME "ctlrender-EXR16->EXR16" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16.exr")
    add_test(NAME "ctlrender-EXR16->EXR32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr32 -force "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr32.exr")
    add_test(NAME "ctlrender-EXR32->EXR16" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr32_to_exr16.exr")
    add_test(NAME "ctlrender-EXR32->EXR32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr32 -force "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr32_to_exr32.exr")

    # RGBA Tests
    add_test(NAME "ctlrender-EXR_RGBA16->EXR_RGBA_16" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity_with_alpha.ctl" -format exr16 -force "${TEST_FILES}/colorbars_nuke_rgba_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgba_exr16_to_exr16.exr")
    add_test(NAME "ctlrender-EXR_RGBA16->EXR_RGBA_32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity_with_alpha.ctl" -format exr32 -force "${TEST_FILES}/colorbars_nuke_rgba_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgba_exr16_to_exr32.exr")
    add_test(NAME "ctlrender-EXR_RGBA32->EXR_RGBA_16" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity_with_alpha.ctl" -format exr16 -force "${TEST_FILES}/colorbars_nuke_rgba_exr32.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgba_exr32_to_exr16.exr")
    add_test(NAME "ctlrender-EXR_RGBA32->EXR_RGBA_32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity_with_alpha.ctl" -format exr32 -force "${TEST_FILES}/colorbars_nuke_rgba_exr32.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgba_exr32_to_exr32.exr")

    # EXR compression tests
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-NONE" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression none "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_none.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-RLE" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression rle "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_rle.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-ZIPS" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression zips "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_zips.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-ZIP" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression zip "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_zip.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-PIZ" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression piz "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_piz.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-PXR24" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression pxr24 "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_pxr24.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-B44" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression b44 "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_b44.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-B44A" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression b44a "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_b44a.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-DWAA" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression dwaa "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_dwaa.exr")
    add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-DWAB" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression dwab "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_dwab.exr")
    if((OPENEXR_VERSION_MAJOR GREATER_EQUAL 3 AND OPENEXR_VERSION_MINOR GREATER_EQUAL 4) OR (OPENEXR_VERSION_MAJOR GREATER_EQUAL 4))
        add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-HTJ2K256" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression htj2k256 "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_htj2k256.exr")
        add_test(NAME "ctlrender-EXR16->EXR16-COMPRESSION-HTJ2K32" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr16 -force -compression htj2k32 "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_exr16_compression_htj2k32.exr")
    endif()

endif()

#ACES tests
if (OpenEXR_FOUND AND AcesContainer_FOUND)
    add_test(NAME "ctlrender-EXR16->ACES" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format aces -force "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_aces.aces")
endif()

# test TIFF32 to EXR and EXR to TIFF32 support
if (OpenEXR_FOUND AND TARGET TIFF::TIFF)
    foreach(J exr exr16 exr32 tiff32)
        foreach(I bars_photoshop_32_be_interleaved.tif bars_photoshop_32_be_planar.tif bars_photoshop_32_le_interleaved.tif bars_photoshop_32_le_planar.tif bars_photoshop.exr)
             # Get extension from ${J}
            string(REGEX REPLACE "[0-9]+" "" EXTENSION "${J}")
            # Get filename from ${I}
            string(REGEX REPLACE "\\..+" "" FILE_NAME "${I}")
            # Get file extension from ${I}
            string(REGEX REPLACE ".+\\." "" FILE_NAME_EXTENSION "${I}")
            if(${J} MATCHES "exr.*" AND FILE_NAME_EXTENSION STREQUAL "exr")
                continue()
            endif()
            if(${J} MATCHES "tif.*" AND FILE_NAME_EXTENSION STREQUAL "tif")
                continue()
            endif()
            add_test(NAME "ctlrender-EXR<->TIF_${I}->${J}" COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format ${J} -force "${TEST_FILES}/${I}" "${CTLRENDER_OUTPUT_FOLDER}/${FILE_NAME}_${J}_exr_tif_conversion_test.${EXTENSION}")
        endforeach()
    endforeach()
endif()

add_test(NAME "ctlrender-ctl-function-name-is-filename" COMMAND ctlrender -force -format exr32 -ctl "${TEST_FILES}/functionname_is_filename.ctl" "${TEST_FILES}/bars_photoshop.exr" "${CTLRENDER_OUTPUT_FOLDER}/functionname_is_filename.exr")

add_test(NAME "ctlrender-ctl-function-name-is-incorrect" COMMAND ctlrender -force -format exr32 -ctl "${TEST_FILES}/incorrect_function_name.ctl" "${TEST_FILES}/bars_photoshop.exr" "${CTLRENDER_OUTPUT_FOLDER}/incorrect_function_name.exr")
set_tests_properties("ctlrender-ctl-function-name-is-incorrect" PROPERTIES WILL_FAIL TRUE)

# Test for invalid input file names
add_test(NAME "ctlrender-invalid-input-filename-dpx" COMMAND ctlrender -force -format dpx -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/invalid_filename.dpx" "${CTLRENDER_OUTPUT_FOLDER}/invalid_filename_out.dpx")
set_tests_properties("ctlrender-invalid-input-filename-dpx" PROPERTIES WILL_FAIL TRUE)
if(TARGET TIFF::TIFF)
    add_test(NAME "ctlrender-invalid-input-filename-tiff" COMMAND ctlrender -force -format tiff -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/invalid_filename.tif" "${CTLRENDER_OUTPUT_FOLDER}/invalid_filename_out.tif")
    set_tests_properties("ctlrender-invalid-input-filename-tiff" PROPERTIES WILL_FAIL TRUE)
endif()
if(OpenEXR_FOUND)
    add_test(NAME "ctlrender-invalid-input-filename-exr" COMMAND ctlrender -force -format exr -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/invalid_filename.exr" "${CTLRENDER_OUTPUT_FOLDER}/invalid_filename_out.exr")
    set_tests_properties("ctlrender-invalid-input-filename-exr" PROPERTIES WILL_FAIL TRUE)
endif()

# Test for invalid output file names
add_test(NAME "ctlrender-invalid-output-filename-dpx" COMMAND ctlrender -force -format dpx -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/bars_nuke_8_be.dpx" "${CTLRENDER_OUTPUT_FOLDER}/missing_folder/invalid_filename_out.dpx")
set_tests_properties("ctlrender-invalid-output-filename-dpx" PROPERTIES WILL_FAIL TRUE)
if(TARGET TIFF::TIFF)
    add_test(NAME "ctlrender-invalid-output-filename-tiff" COMMAND ctlrender -force -format tiff -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/bars_photoshop_8_be_interleaved.tif" "${CTLRENDER_OUTPUT_FOLDER}/missing_folder/invalid_filename_out.tif")
    set_tests_properties("ctlrender-invalid-output-filename-tiff" PROPERTIES WILL_FAIL TRUE)
endif()
if(OpenEXR_FOUND)
    add_test(NAME "ctlrender-invalid-output-filename-exr" COMMAND ctlrender -force -format exr -ctl "${TEST_FILES}/unity.ctl" "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/missing_folder/invalid_filename_out.exr")
    set_tests_properties("ctlrender-invalid-output-filename-exr" PROPERTIES WILL_FAIL TRUE)
endif()

# test for maxInstCount exception
add_test(NAME "ctlrender-maxInstCount-exception" COMMAND ctlrender -force -ctl "${PROJECT_SOURCE_DIR}/resources/test/ctl/throw_maxInstCount_exception.ctl" "${PROJECT_SOURCE_DIR}/resources/test/exr/colorbars_nuke_rgb_exr16.exr" "${CTLRENDER_OUTPUT_FOLDER}/maxInstCount_exception.exr")
set_tests_properties("ctlrender-maxInstCount-exception" PROPERTIES WILL_FAIL TRUE)

# -pixel mode: one pixel on CLI, result to stdout, no file I/O.
#
# Input RGBA = (0.5, 0.5, 0.5, 1.0); pixel_chain_a.ctl adds 0.25 to R;
# pixel_chain_b.ctl multiplies G by 2 and halves A.  Expected:
#   single a:    R=0.75 G=0.5  B=0.5 A=1.0
#   chain a→b:   R=0.75 G=1.0  B=0.5 A=0.5
# Tests assert via PASS_REGULAR_EXPRESSION on stdout.
add_test(NAME "ctlrender-pixel-rgba-single"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_chain_a.ctl"
        -pixel 0.5,0.5,0.5,1.0)
set_tests_properties("ctlrender-pixel-rgba-single" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=0\\.7500000[ \\t]+G=0\\.5000000[ \\t]+B=0\\.5000000[ \\t]+A=1\\.0000000")

add_test(NAME "ctlrender-pixel-rgba-chain"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_chain_a.ctl"
        -ctl "${TEST_FILES}/pixel_chain_b.ctl"
        -pixel 0.5,0.5,0.5,1.0)
set_tests_properties("ctlrender-pixel-rgba-chain" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=0\\.7500000[ \\t]+G=1\\.0000000[ \\t]+B=0\\.5000000[ \\t]+A=0\\.5000000")

# RGB (3 values) depth-3 input; expected stdout has no 'A=' component.
add_test(NAME "ctlrender-pixel-rgb-only"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_rgb_only.ctl"
        -pixel 0.1,0.2,0.3)
set_tests_properties("ctlrender-pixel-rgb-only" PROPERTIES
    PASS_REGULAR_EXPRESSION "^R=0\\.2000000[ \\t]+G=0\\.4000000[ \\t]+B=0\\.6000000[ \\t]*\n?$")

# Error: -pixel without -ctl must fail.
add_test(NAME "ctlrender-pixel-no-ctl-errors"
    COMMAND ctlrender -pixel 0.1,0.2,0.3,1.0)
set_tests_properties("ctlrender-pixel-no-ctl-errors" PROPERTIES WILL_FAIL TRUE)

# Error: -pixel mixed with a positional input file must fail.
add_test(NAME "ctlrender-pixel-positional-conflict-errors"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_chain_a.ctl"
        -pixel 0.1,0.2,0.3,1.0
        "${TEST_FILES}/colorbars_nuke_rgba_exr32.exr")
set_tests_properties("ctlrender-pixel-positional-conflict-errors" PROPERTIES WILL_FAIL TRUE)

# Error: non-numeric value in -pixel must fail.
add_test(NAME "ctlrender-pixel-malformed-errors"
    COMMAND ctlrender -ctl "${TEST_FILES}/pixel_chain_a.ctl" -pixel 0.1,abc,0.3)
set_tests_properties("ctlrender-pixel-malformed-errors" PROPERTIES WILL_FAIL TRUE)

# Error: too few values (needs 3 or 4).
add_test(NAME "ctlrender-pixel-too-few-errors"
    COMMAND ctlrender -ctl "${TEST_FILES}/pixel_chain_a.ctl" -pixel 0.1,0.2)
set_tests_properties("ctlrender-pixel-too-few-errors" PROPERTIES WILL_FAIL TRUE)

# -pixel + -param1: uniform parameter override on the single-pixel path.
# pixel_with_param.ctl scales R by `scale` (default 1.0).  Default test
# verifies the default propagates; override test verifies -param1 reaches
# the pixel path the same way it reaches the image path.
add_test(NAME "ctlrender-pixel-param-default"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_with_param.ctl"
        -pixel 0.5,0.6,0.7,1.0)
set_tests_properties("ctlrender-pixel-param-default" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=0\\.5000000[ \\t]+G=0\\.6000000[ \\t]+B=0\\.7000000[ \\t]+A=1\\.0000000")

add_test(NAME "ctlrender-pixel-param-override"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_with_param.ctl"
        -param1 scale 2.0
        -pixel 0.5,0.6,0.7,1.0)
set_tests_properties("ctlrender-pixel-param-override" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=1\\.0000000[ \\t]+G=0\\.6000000[ \\t]+B=0\\.7000000[ \\t]+A=1\\.0000000")

# -pixel HDR-range inputs: negative and large positive values must pass
# through the SIMD interpreter unclipped.
add_test(NAME "ctlrender-pixel-hdr-negative"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_chain_a.ctl"
        -pixel -0.5,0.6,0.7,1.0)
set_tests_properties("ctlrender-pixel-hdr-negative" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=-0\\.2500000[ \\t]+G=0\\.6000000[ \\t]+B=0\\.7000000[ \\t]+A=1\\.0000000")

add_test(NAME "ctlrender-pixel-hdr-large"
    COMMAND ctlrender
        -ctl "${TEST_FILES}/pixel_chain_a.ctl"
        -pixel 10.0,0.6,0.7,1.0)
set_tests_properties("ctlrender-pixel-hdr-large" PROPERTIES
    PASS_REGULAR_EXPRESSION "R=10\\.2500000[ \\t]+G=0\\.6000000[ \\t]+B=0\\.7000000[ \\t]+A=1\\.0000000")

# Error: -param1 before any -ctl must fail (parser invariant).
add_test(NAME "ctlrender-pixel-param-before-ctl-errors"
    COMMAND ctlrender
        -param1 scale 2.0
        -ctl "${TEST_FILES}/pixel_with_param.ctl"
        -pixel 0.5,0.6,0.7,1.0)
set_tests_properties("ctlrender-pixel-param-before-ctl-errors" PROPERTIES WILL_FAIL TRUE)

# Phase B: tile-parallel dispatch must produce byte-identical output for
# any -threads N on a fixture that spans multiple tiles (maxSamples=4096,
# so >4096 pixels). Runs the same transform at several thread counts and
# compares the resulting EXRs byte-for-byte against the N=1 reference.
if(OpenEXR_FOUND)
    set(THREADED_PARITY_REF "${CTLRENDER_OUTPUT_FOLDER}/threaded_parity_ref.exr")
    add_test(NAME "ctlrender-threaded-parity-ref"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force -threads 1
                "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                "${THREADED_PARITY_REF}")
    foreach(N 2 4 8 12)
        set(OUT "${CTLRENDER_OUTPUT_FOLDER}/threaded_parity_t${N}.exr")
        add_test(NAME "ctlrender-threaded-parity-t${N}-run"
            COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                    -format exr32 -force -threads ${N}
                    "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                    "${OUT}")
        add_test(NAME "ctlrender-threaded-parity-t${N}-cmp"
            COMMAND ${CMAKE_COMMAND} -E compare_files
                    "${THREADED_PARITY_REF}" "${OUT}")
        set_tests_properties("ctlrender-threaded-parity-t${N}-run"
            PROPERTIES DEPENDS "ctlrender-threaded-parity-ref")
        set_tests_properties("ctlrender-threaded-parity-t${N}-cmp"
            PROPERTIES DEPENDS "ctlrender-threaded-parity-t${N}-run")
    endforeach()
endif()

# exr16 (half) output threaded parity: the float->half conversion in
# exr_file.cc has its own work-stealing parallelization.  The fixture is
# below the kMinToThread threshold so the parallel path itself only fires
# on larger inputs, but this matrix still pins serial-vs-threaded byte
# parity through the whole exr16 write pipeline (interpreter threading,
# format conversion, OpenEXR compression).
if(OpenEXR_FOUND)
    set(EXR16_PARITY_REF "${CTLRENDER_OUTPUT_FOLDER}/exr16_parity_ref.exr")
    add_test(NAME "ctlrender-exr16-threaded-parity-ref"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                -format exr16 -force -threads 1
                "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                "${EXR16_PARITY_REF}")
    foreach(N 2 4 8)
        set(OUT "${CTLRENDER_OUTPUT_FOLDER}/exr16_parity_t${N}.exr")
        add_test(NAME "ctlrender-exr16-threaded-parity-t${N}-run"
            COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                    -format exr16 -force -threads ${N}
                    "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                    "${OUT}")
        add_test(NAME "ctlrender-exr16-threaded-parity-t${N}-cmp"
            COMMAND ${CMAKE_COMMAND} -E compare_files
                    "${EXR16_PARITY_REF}" "${OUT}")
        set_tests_properties("ctlrender-exr16-threaded-parity-t${N}-run"
            PROPERTIES DEPENDS "ctlrender-exr16-threaded-parity-ref")
        set_tests_properties("ctlrender-exr16-threaded-parity-t${N}-cmp"
            PROPERTIES DEPENDS "ctlrender-exr16-threaded-parity-t${N}-run")
    endforeach()
endif()

# -threads × -compression interaction: each compression codec sees a
# different chunk-size / encoder, and the per-tile pixel batches arrive
# from worker threads.  Verify byte-identical output across N=1 vs N=4
# for a representative set of codecs (lossless block, lossless wavelet,
# lossy DCT).
if(OpenEXR_FOUND)
    foreach(C zip zips piz dwaa)
        set(REF "${CTLRENDER_OUTPUT_FOLDER}/threads_compression_${C}_ref.exr")
        set(OUT "${CTLRENDER_OUTPUT_FOLDER}/threads_compression_${C}_t4.exr")
        add_test(NAME "ctlrender-threads-compression-${C}-ref"
            COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                    -format exr32 -force -threads 1 -compression ${C}
                    "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                    "${REF}")
        add_test(NAME "ctlrender-threads-compression-${C}-t4-run"
            COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                    -format exr32 -force -threads 4 -compression ${C}
                    "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                    "${OUT}")
        add_test(NAME "ctlrender-threads-compression-${C}-t4-cmp"
            COMMAND ${CMAKE_COMMAND} -E compare_files "${REF}" "${OUT}")
        set_tests_properties("ctlrender-threads-compression-${C}-t4-run"
            PROPERTIES DEPENDS "ctlrender-threads-compression-${C}-ref")
        set_tests_properties("ctlrender-threads-compression-${C}-t4-cmp"
            PROPERTIES DEPENDS "ctlrender-threads-compression-${C}-t4-run")
    endforeach()
endif()

# -help / -help <topic>: ctlrender prints usage and exits 1 (the existing
# behaviour — `-help` is treated as a parse-error path, not exit-0).  Each
# topic dispatches through a different branch in usage.cc, which was
# entirely untested (0% line coverage) before these cases.
foreach(TOPIC noarg format compression ctl pixel scale param unknown)
    if(TOPIC STREQUAL "noarg")
        add_test(NAME "ctlrender-help-${TOPIC}" COMMAND ctlrender -help)
    else()
        add_test(NAME "ctlrender-help-${TOPIC}"
            COMMAND ctlrender -help ${TOPIC})
    endif()
    set_tests_properties("ctlrender-help-${TOPIC}" PROPERTIES WILL_FAIL TRUE)
endforeach()

# CLI flag coverage for ctlrender/main.cc — every option we support but
# don't otherwise exercise in another test.  Each flag gets at minimum one
# happy-path case (with unity.ctl + an existing fixture), and the obvious
# error branches (missing arg / malformed value) where they exist.
if(OpenEXR_FOUND)
    set(_FX  "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr")
    set(_FX_RGBA "${TEST_FILES}/colorbars_nuke_rgba_exr32.exr")
    set(_OUT "${CTLRENDER_OUTPUT_FOLDER}")

    # -input_scale / -output_scale: both take a float, both have malformed
    # error branches.
    add_test(NAME "ctlrender-input-scale-happy"
        COMMAND ${CTLRENDER_PATH} -input_scale 0.5 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/input_scale.exr")
    add_test(NAME "ctlrender-input-scale-no-arg-errors"
        COMMAND ${CTLRENDER_PATH} -input_scale)
    set_tests_properties("ctlrender-input-scale-no-arg-errors" PROPERTIES WILL_FAIL TRUE)
    add_test(NAME "ctlrender-input-scale-malformed-errors"
        COMMAND ${CTLRENDER_PATH} -input_scale abc -ctl "${TEST_FILES}/unity.ctl" "${_FX}" "${_OUT}/x.exr")
    set_tests_properties("ctlrender-input-scale-malformed-errors" PROPERTIES WILL_FAIL TRUE)

    add_test(NAME "ctlrender-output-scale-happy"
        COMMAND ${CTLRENDER_PATH} -output_scale 2.0 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/output_scale.exr")
    add_test(NAME "ctlrender-output-scale-malformed-errors"
        COMMAND ${CTLRENDER_PATH} -output_scale abc -ctl "${TEST_FILES}/unity.ctl" "${_FX}" "${_OUT}/x.exr")
    set_tests_properties("ctlrender-output-scale-malformed-errors" PROPERTIES WILL_FAIL TRUE)

    # -noalpha: drops the alpha channel from RGBA input.  Output should
    # write successfully (we don't assert on channel count, just that
    # the path runs).
    add_test(NAME "ctlrender-noalpha-happy"
        COMMAND ${CTLRENDER_PATH} -noalpha -ctl "${TEST_FILES}/unity_with_alpha.ctl"
                -format exr32 -force "${_FX_RGBA}" "${_OUT}/noalpha.exr")

    # -quiet / -verbose: just verify they don't change exit status.
    add_test(NAME "ctlrender-quiet-happy"
        COMMAND ${CTLRENDER_PATH} -quiet -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/quiet.exr")
    add_test(NAME "ctlrender-verbose-happy"
        COMMAND ${CTLRENDER_PATH} -verbose -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/verbose.exr")

    # -jobs: file-level parallelism.  Test happy paths at N=0 (autoselect)
    # and N=1 (serial), plus malformed/missing-arg errors.
    add_test(NAME "ctlrender-jobs-1-happy"
        COMMAND ${CTLRENDER_PATH} -jobs 1 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/jobs_1.exr")
    add_test(NAME "ctlrender-jobs-0-autoselect-happy"
        COMMAND ${CTLRENDER_PATH} -jobs 0 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/jobs_0.exr")
    add_test(NAME "ctlrender-jobs-no-arg-errors"
        COMMAND ${CTLRENDER_PATH} -jobs)
    set_tests_properties("ctlrender-jobs-no-arg-errors" PROPERTIES WILL_FAIL TRUE)
    add_test(NAME "ctlrender-jobs-malformed-errors"
        COMMAND ${CTLRENDER_PATH} -jobs abc -ctl "${TEST_FILES}/unity.ctl" "${_FX}" "${_OUT}/x.exr")
    set_tests_properties("ctlrender-jobs-malformed-errors" PROPERTIES WILL_FAIL TRUE)

    # -format unknown-value error branch (-compression with an unknown
    # codec name silently uses the default rather than erroring on
    # cpu-perf, so we don't test that here).
    add_test(NAME "ctlrender-format-unknown-errors"
        COMMAND ${CTLRENDER_PATH} -format mystery -ctl "${TEST_FILES}/unity.ctl" "${_FX}" "${_OUT}/x.mystery")
    set_tests_properties("ctlrender-format-unknown-errors" PROPERTIES WILL_FAIL TRUE)

    # -global_param1: applies a uniform parameter override across all
    # -ctl operations.  image_with_param.ctl declares `input uniform
    # float gain = 1.0` against the rIn/gIn/bIn ctlrender image input
    # convention, so -global_param1 gain 2.0 reaches it.
    add_test(NAME "ctlrender-global-param1-happy"
        COMMAND ${CTLRENDER_PATH} -global_param1 gain 2.0 -ctl "${TEST_FILES}/image_with_param.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/global_param1.exr")
    add_test(NAME "ctlrender-global-param1-no-arg-errors"
        COMMAND ${CTLRENDER_PATH} -global_param1)
    set_tests_properties("ctlrender-global-param1-no-arg-errors" PROPERTIES WILL_FAIL TRUE)

    # -param1 short alias (-p1) — exercises the same code path as -param1
    # but verifies the alias dispatch.
    add_test(NAME "ctlrender-p1-alias-happy"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/image_with_param.ctl" -p1 gain 0.75
                -format exr32 -force "${_FX}" "${_OUT}/p1_alias.exr")

    # Image-chain mode: more than one -ctl applied serially to an image
    # input (the existing chain test only used -pixel).
    add_test(NAME "ctlrender-image-multi-ctl-chain"
        COMMAND ${CTLRENDER_PATH}
                -ctl "${TEST_FILES}/unity.ctl"
                -ctl "${TEST_FILES}/image_with_param.ctl" -p1 gain 1.5
                -format exr32 -force "${_FX}" "${_OUT}/multi_chain.exr")

    # Unknown option must fail.
    add_test(NAME "ctlrender-unknown-option-errors"
        COMMAND ${CTLRENDER_PATH} -nonsense -ctl "${TEST_FILES}/unity.ctl" "${_FX}" "${_OUT}/x.exr")
    set_tests_properties("ctlrender-unknown-option-errors" PROPERTIES WILL_FAIL TRUE)

    # transform.cc edge cases:
    # -threads 0 = autoselect; ensure the autoselect path runs cleanly.
    add_test(NAME "ctlrender-threads-autoselect-happy"
        COMMAND ${CTLRENDER_PATH} -threads 0 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/threads_auto.exr")
    # -threads N where N >> number of tiles in the fixture: hits the
    # `worker_count > tiles.size()` cap path inside transform.cc that
    # only kicks in when explicit thread count exceeds tile count.
    add_test(NAME "ctlrender-threads-more-than-tiles-happy"
        COMMAND ${CTLRENDER_PATH} -threads 64 -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force "${_FX}" "${_OUT}/threads_64.exr")

    # Multi-input batch: multiple positional inputs + a destination
    # directory + an explicit -format.  Exercises the "outdir" + per-file
    # transform code path (vs. single-file in→out).
    set(_MULTI_OUT "${_OUT}/multi_in")
    add_test(NAME "ctlrender-multi-input-mkdir"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_MULTI_OUT}")
    add_test(NAME "ctlrender-multi-input-batch-happy"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr32 -force
                "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr"
                "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                "${TEST_FILES}/bars_photoshop.exr"
                "${_MULTI_OUT}")
    set_tests_properties("ctlrender-multi-input-batch-happy"
        PROPERTIES DEPENDS "ctlrender-multi-input-mkdir")

    # -jobs > 1 with a multi-input batch: file-level parallelism running
    # whole transforms concurrently.  Catches regressions in the per-job
    # fork/join logic and the shared global_ctl_parameters access path.
    add_test(NAME "ctlrender-jobs-multi-input-happy"
        COMMAND ${CTLRENDER_PATH} -jobs 2 -ctl "${TEST_FILES}/unity.ctl" -format exr32 -force
                "${TEST_FILES}/colorbars_nuke_rgb_exr16.exr"
                "${TEST_FILES}/colorbars_nuke_rgb_exr32.exr"
                "${TEST_FILES}/bars_photoshop.exr"
                "${_MULTI_OUT}")
    set_tests_properties("ctlrender-jobs-multi-input-happy"
        PROPERTIES DEPENDS "ctlrender-multi-input-batch-happy")

    # Force-overwrite negative test: without -force, attempting to
    # overwrite an existing file must error out.
    add_test(NAME "ctlrender-write-existing-no-force-errors"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl" -format exr32
                "${_FX}" "${_OUT}/threads_auto.exr")
    set_tests_properties("ctlrender-write-existing-no-force-errors"
        PROPERTIES DEPENDS "ctlrender-threads-autoselect-happy" WILL_FAIL TRUE)
endif()

# ACES round-trip: ACES → EXR via unity.  The existing test produces an
# .aces file from an EXR input; this reads that .aces back through the
# pipeline as a cross-check that aces_file.cc's read path works on its
# own writer's output.
if (OpenEXR_FOUND AND AcesContainer_FOUND)
    add_test(NAME "ctlrender-ACES->EXR-roundtrip"
        COMMAND ${CTLRENDER_PATH} -ctl "${TEST_FILES}/unity.ctl"
                -format exr32 -force
                "${CTLRENDER_OUTPUT_FOLDER}/bars_rgb_exr16_to_aces.aces"
                "${CTLRENDER_OUTPUT_FOLDER}/aces_roundtrip.exr")
    set_tests_properties("ctlrender-ACES->EXR-roundtrip"
        PROPERTIES DEPENDS "ctlrender-EXR16->ACES")
endif()
