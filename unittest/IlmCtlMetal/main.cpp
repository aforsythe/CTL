///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "testMetalAcesV2Parity.h"
#include "testMetalAcesV2RoundTrip.h"
#include "testMetalArithmetic.h"
#include "testMetalAssertFailure.h"
#include "testMetalBenchmark.h"
#include "testMetalCodegen.h"
#include "testMetalDefaultParams.h"
#include "testMetalDispatch.h"
#include "testMetalHalfExpLog.h"
#include "testMetalHelloWorld.h"
#include "testMetalIlmCtlFixtures.h"
#include "testMetalLanguageRejections.h"
#include "testMetalLContext.h"
#include "testMetalNestedVSArrays.h"
#include "testMetalParity.h"
#include "testMetalReturnValue.h"
#include "testMetalScatterKernel.h"
#include "testMetalShaderCache.h"
#include "testMetalSidecarCache.h"
#include "testMetalStub.h"

#include <iostream>
#include <string.h>

#define TEST(x) if (argc < 2 || !strcmp(argv[1], #x)) x();

int
main(int argc, char *argv[])
{
    std::cout << std::endl;

    TEST(testMetalAcesV2Parity);
    TEST(testMetalAcesV2RoundTrip);
    TEST(testMetalArithmetic);
    TEST(testMetalAssertFailure);
    TEST(testMetalBenchmark);
    TEST(testMetalCodegen);
    TEST(testMetalDefaultParams);
    TEST(testMetalDispatch);
    TEST(testMetalHalfExpLog);
    TEST(testMetalHelloWorld);
    TEST(testMetalIlmCtlFixtures);
    TEST(testMetalLanguageRejections);
    TEST(testMetalLContext);
    TEST(testMetalNestedVSArrays);
    TEST(testMetalParity);
    TEST(testMetalReturnValue);
    TEST(testMetalScatterKernel);
    TEST(testMetalShaderCache);
    TEST(testMetalSidecarCache);
    TEST(testMetalStub);

    return 0;
}
