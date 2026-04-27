///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Coverage test for SimdReg's array-index reference constructor —
// the variant `SimdReg(SimdReg &original, const SimdReg &indices,
// const SimdBoolMask &mask, size_t arrayElementSize, size_t arraySize,
// size_t regSize, ...)` which testSimdRegAddr.cpp could not exercise
// directly because it requires interpreter machinery (an indices
// register with valid offsets and an active SimdBoolMask).
//
// Strategy: load a CTL fixture with `result = lut[idx]` where `idx` is
// varying.  The SIMD interpreter must use the array-index ref ctor
// per-instruction to gather from the constant array.  Verify per-lane
// outputs match a known squares table; correctness implies the gather
// path computed correct per-lane offsets into the source register's
// buffer.
//

#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <testVaryingArrayIndex.h>

#include <iostream>
#include <vector>
#include <testRequire.h>
#include <cstring>

using namespace Ctl;
using namespace std;


namespace {

// Reference table — must match the `squares` const inside the .ctl.
const float kSquares[8] = {0.0f, 1.0f, 4.0f, 9.0f, 16.0f, 25.0f, 36.0f, 49.0f};


void
testGatherDistinctIndices (SimdInterpreter &interp)
{
    cout << "  varying gather: every lane reads a different index" << endl;

    FunctionCallPtr fn = interp.newFunctionCall("vai_test::gather");
    FunctionArgPtr  idx = fn->findInputArg("idx");
    FunctionArgPtr  out = fn->findOutputArg("result");
    REQUIRE(idx);
    REQUIRE(out);

    const size_t N = 64;
    int *idxData = (int*)(idx->data());
    for (size_t i = 0; i < N; ++i)
	idxData[i] = static_cast<int>(i % 8);

    fn->callFunction(N);

    const float *outData = (const float*)(out->data());
    for (size_t i = 0; i < N; ++i)
    {
	const float expected = kSquares[i % 8];
	if (outData[i] != expected)
	{
	    cerr << "lane " << i << ": expected " << expected
		 << " got " << outData[i] << " (idx=" << idxData[i] << ")"
		 << endl;
	    REQUIRE(false && "varying gather produced wrong per-lane value");
	}
    }
}


void
testGatherUniformIndex (SimdInterpreter &interp)
{
    cout << "  varying gather: every lane reads the same index" << endl;

    // Edge case: all lanes index the same slot.  The interpreter still
    // routes through the varying-array-index ref ctor (because idx is
    // declared varying), but the gather pattern is a broadcast.

    FunctionCallPtr fn = interp.newFunctionCall("vai_test::gather");
    FunctionArgPtr  idx = fn->findInputArg("idx");
    FunctionArgPtr  out = fn->findOutputArg("result");
    REQUIRE(idx);
    REQUIRE(out);

    const size_t N = 32;
    int *idxData = (int*)(idx->data());
    for (size_t i = 0; i < N; ++i)
	idxData[i] = 5;   // all lanes read squares[5] = 25.0

    fn->callFunction(N);

    const float *outData = (const float*)(out->data());
    for (size_t i = 0; i < N; ++i)
    {
	REQUIRE(outData[i] == kSquares[5]);
    }
}


void
testGatherWithUniformOffset (SimdInterpreter &interp)
{
    cout << "  varying gather + uniform offset: composes correctly" << endl;

    // Composes the varying gather with an additional uniform-input
    // operation.  Verifies the gather result is correctly fed into the
    // subsequent add.

    FunctionCallPtr fn = interp.newFunctionCall("vai_test::gather_with_offset");
    FunctionArgPtr  idx    = fn->findInputArg("idx");
    FunctionArgPtr  offset = fn->findInputArg("offset");
    FunctionArgPtr  out    = fn->findOutputArg("result");
    REQUIRE(idx);
    REQUIRE(offset);
    REQUIRE(out);

    const size_t N = 16;
    int *idxData = (int*)(idx->data());
    for (size_t i = 0; i < N; ++i)
	idxData[i] = static_cast<int>(i % 8);

    float *offData = (float*)(offset->data());
    offData[0] = 100.0f;

    fn->callFunction(N);

    const float *outData = (const float*)(out->data());
    for (size_t i = 0; i < N; ++i)
    {
	const float expected = kSquares[i % 8] + 100.0f;
	if (outData[i] != expected)
	{
	    cerr << "lane " << i << ": expected " << expected
		 << " got " << outData[i] << endl;
	    REQUIRE(false && "gather_with_offset produced wrong per-lane value");
	}
    }
}

} // anonymous namespace


void
testVaryingArrayIndex ()
{
    cout << endl;
    cout << "Testing SimdReg array-index ref ctor via varying gather"
	 << endl;

    SimdInterpreter interp;
    interp.loadModule("testVaryingArrayIndex");

    testGatherDistinctIndices(interp);
    testGatherUniformIndex(interp);
    testGatherWithUniformOffset(interp);

    cout << "ok" << endl;
}
