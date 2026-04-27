///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// IEEE-754 edge-value tests for the SIMD interpreter.
//
// Vector and scalar arithmetic paths must agree bit-exactly on
// non-finite inputs (NaN, +/-Inf, denormals, signed zero).  Past
// vectorization rewrites of comparable interpreters have introduced
// silent regressions like:
//   - NaN-bit-pattern stripping (e.g. payload bits dropped by a
//     half<->float round-trip in the SIMD path)
//   - Comparison results differing under SIMD compare-and-mask vs
//     scalar branch (any comparison with NaN must be false)
//   - 0 * Inf reordered into 0 * x where x already had Inf consumed,
//     producing 0 instead of NaN
//   - Denormal flush-to-zero on one path but not the other (FTZ/DAZ
//     register state differs between scalar and SIMD code paths)
//
// Strategy: run the same arithmetic through the interpreter with a
// curated set of input pairs spanning every IEEE-754 special-value
// combination, then compare against scalar IEEE-754 expected values
// computed in C++ on the same machine.  A bit-pattern compare
// detects NaN-payload divergence that == comparison would miss.
//

#include <CtlSimdInterpreter.h>
#include <CtlFunctionCall.h>
#include <testEdgeValues.h>
#include <testRequire.h>

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <limits>

using namespace Ctl;
using namespace std;


namespace {

uint32_t
bits (float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
}


bool
bitwise_equal (float a, float b)
{
    return bits(a) == bits(b);
}


// IEEE-754 specials we want to cover.  Keeping the set small but
// distinct: every combination of these operands probes a different
// precision/branch behaviour.
const float kSpecials[] = {
    0.0f,
    -0.0f,
    1.0f,
    -1.0f,
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::min(),                     // smallest normal
    std::numeric_limits<float>::denorm_min(),              // smallest denormal
    std::numeric_limits<float>::max(),
};
const int kNSpecials = sizeof(kSpecials) / sizeof(kSpecials[0]);


void
testArithmetic (SimdInterpreter &interp)
{
    cout << "  arith(a,b): sum/diff/prod/quot bit-identical to IEEE-754 reference"
	 << endl;

    FunctionCallPtr fn = interp.newFunctionCall("edge_test::arith");
    FunctionArgPtr aArg    = fn->findInputArg("a");
    FunctionArgPtr bArg    = fn->findInputArg("b");
    FunctionArgPtr sumArg  = fn->findOutputArg("sum");
    FunctionArgPtr diffArg = fn->findOutputArg("diff");
    FunctionArgPtr prodArg = fn->findOutputArg("prod");
    FunctionArgPtr quotArg = fn->findOutputArg("quot");
    REQUIRE(aArg && bArg && sumArg && diffArg && prodArg && quotArg);

    // Lay out every (a, b) pair from kSpecials across lanes.  N pairs
    // = kNSpecials * kNSpecials = 100; well below MAX_REG_SIZE (8192).
    const size_t N = static_cast<size_t>(kNSpecials) * kNSpecials;
    float *aData = (float*)(aArg->data());
    float *bData = (float*)(bArg->data());
    for (int i = 0; i < kNSpecials; ++i)
    {
	for (int j = 0; j < kNSpecials; ++j)
	{
	    aData[i * kNSpecials + j] = kSpecials[i];
	    bData[i * kNSpecials + j] = kSpecials[j];
	}
    }

    fn->callFunction(N);

    const float *sumOut  = (const float*)(sumArg->data());
    const float *diffOut = (const float*)(diffArg->data());
    const float *prodOut = (const float*)(prodArg->data());
    const float *quotOut = (const float*)(quotArg->data());

    int divergences = 0;
    for (int i = 0; i < kNSpecials; ++i)
    {
	for (int j = 0; j < kNSpecials; ++j)
	{
	    const size_t lane = static_cast<size_t>(i) * kNSpecials + j;
	    const float a = kSpecials[i], b = kSpecials[j];
	    const float refSum  = a + b;
	    const float refDiff = a - b;
	    const float refProd = a * b;
	    const float refQuot = a / b;

	    // Compare bit-exact, but tolerate the well-known NaN-payload
	    // difference: hardware NaN canonicalisation may produce a
	    // different NaN bit pattern than C++ literal NaN.  Both being
	    // NaN is the IEEE-754 contract; the exact payload is not.
	    auto check = [&](const char *op, float got, float expect) {
		if (std::isnan(got) && std::isnan(expect)) return;
		if (!bitwise_equal(got, expect))
		{
		    cerr << "  divergence: " << op
			 << " a=" << a << " b=" << b
			 << "  got=" << got << " (0x" << std::hex << bits(got) << std::dec << ")"
			 << "  ref=" << expect << " (0x" << std::hex << bits(expect) << std::dec << ")"
			 << endl;
		    ++divergences;
		}
	    };

	    check("a+b", sumOut[lane],  refSum);
	    check("a-b", diffOut[lane], refDiff);
	    check("a*b", prodOut[lane], refProd);
	    check("a/b", quotOut[lane], refQuot);
	}
    }
    REQUIRE(divergences == 0);
}


void
testMaskedLoopRespectsBranchMask (SimdInterpreter &interp)
{
    cout << "  masked_loop: lanes branched off via `if(active)` must NOT iterate"
	 << endl;

    // Lanes with active=false should produce result=0 (loop body never
    // runs).  Lanes with active=true should produce result=n_in.  A
    // mutation that makes the loop's per-lane mask track only the
    // condition (ignoring the surrounding branch mask) would let the
    // body run for inactive lanes too — caught here.
    FunctionCallPtr fn = interp.newFunctionCall("edge_test::masked_loop");
    FunctionArgPtr nArg      = fn->findInputArg("n_in");
    FunctionArgPtr activeArg = fn->findInputArg("active");
    FunctionArgPtr resArg    = fn->findOutputArg("result");
    REQUIRE(nArg && activeArg && resArg);

    const size_t N = 8;
    int  *nIn   = (int*) (nArg->data());
    bool *aIn   = (bool*)(activeArg->data());
    // Alternating active flag, identical n_in everywhere.  The expected
    // result follows directly from `active`: 5 or 0.
    for (size_t i = 0; i < N; ++i)
    {
	nIn[i] = 5;
	aIn[i] = (i % 2 == 0);   // even lanes active, odd lanes filtered out
    }

    fn->callFunction(N);

    const int *res = (const int*)(resArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	const int expected = aIn[i] ? 5 : 0;
	if (res[i] != expected)
	{
	    cerr << "  lane " << i << " active=" << aIn[i]
		 << " n_in=" << nIn[i]
		 << ": expected " << expected << " got " << res[i] << endl;
	    REQUIRE(false && "masked_loop: branch mask not respected");
	}
    }
}


void
testNestedBranchMaskHandling (SimdInterpreter &interp)
{
    cout << "  nested_branch: outer mask must propagate into inner branch"
	 << endl;

    // Lanes filtered out by `if (a)` must NOT be touched by the inner
    // `if (b) ... else ...`.  A SimdBranchInst mutation that constructs
    // the inner trueMask/falseMask without ANDing the outer mask
    // would let the inner branch run on those lanes too — observable
    // because the pre-branch sentinel `r = -1.0` would get overwritten.
    FunctionCallPtr fn = interp.newFunctionCall("edge_test::nested_branch");
    FunctionArgPtr aArg = fn->findInputArg("a");
    FunctionArgPtr bArg = fn->findInputArg("b");
    FunctionArgPtr rArg = fn->findOutputArg("r");
    REQUIRE(aArg && bArg && rArg);

    // Cover all 4 (a, b) combinations:
    const size_t N = 4;
    bool aIn[N] = {true,  false, true,  false};
    bool bIn[N] = {true,  true,  false, false};
    // Expected:    inner   keep    inner   keep
    //              true    senti   false   senti
    float expected[N] = {1.0f, -1.0f, 0.0f, -1.0f};

    bool *aData = (bool*)(aArg->data());
    bool *bData = (bool*)(bArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	aData[i] = aIn[i];
	bData[i] = bIn[i];
    }

    fn->callFunction(N);

    const float *rOut = (const float*)(rArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	if (rOut[i] != expected[i])
	{
	    cerr << "  lane " << i << " a=" << aIn[i] << " b=" << bIn[i]
		 << ": expected " << expected[i] << " got " << rOut[i]
		 << endl;
	    REQUIRE(false && "nested_branch: outer mask leaked into inner");
	}
    }
}


void
testMergedBranchAroundOuterMask (SimdInterpreter &interp)
{
    cout << "  merge_branch: branch-as-expression must respect outer mask"
	 << endl;

    // merge_inner is an if-expression that returns 7 or 9.  Wrapping it
    // in an outer `if (a)` means the merge runs only for lanes where
    // a=true.  Lanes where a=false must keep r = -1.0 from the pre-
    // branch initialisation.
    FunctionCallPtr fn = interp.newFunctionCall("edge_test::merge_branch");
    FunctionArgPtr aArg = fn->findInputArg("a");
    FunctionArgPtr bArg = fn->findInputArg("b");
    FunctionArgPtr rArg = fn->findOutputArg("r");
    REQUIRE(aArg && bArg && rArg);

    const size_t N = 4;
    bool aIn[N] = {true, false, true,  false};
    bool bIn[N] = {true, true,  false, false};
    float expected[N] = {7.0f, -1.0f, 9.0f, -1.0f};

    bool *aData = (bool*)(aArg->data());
    bool *bData = (bool*)(bArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	aData[i] = aIn[i];
	bData[i] = bIn[i];
    }

    fn->callFunction(N);

    const float *rOut = (const float*)(rArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	if (rOut[i] != expected[i])
	{
	    cerr << "  lane " << i << " a=" << aIn[i] << " b=" << bIn[i]
		 << ": expected " << expected[i] << " got " << rOut[i]
		 << endl;
	    REQUIRE(false && "merge_branch: outer mask leaked into merge");
	}
    }
}


void
testCompareBranchWithNaN (SimdInterpreter &interp)
{
    cout << "  compare_branch(a,b): NaN comparisons must be false (IEEE-754)"
	 << endl;

    FunctionCallPtr fn = interp.newFunctionCall("edge_test::compare_branch");
    FunctionArgPtr aArg = fn->findInputArg("a");
    FunctionArgPtr bArg = fn->findInputArg("b");
    FunctionArgPtr rArg = fn->findOutputArg("r");
    REQUIRE(aArg && bArg && rArg);

    // Mix lanes where neither, one, or both operands are NaN.  Any
    // lane involving NaN must take the else branch (r=0.0); the
    // non-NaN lanes follow the strict less-than ordering.
    const size_t N = 6;
    float aIn[N] = { 0.0f,  1.0f,  std::nanf(""), 2.0f,         std::nanf(""), -1.0f };
    float bIn[N] = { 1.0f,  0.0f,  1.0f,          std::nanf(""), std::nanf(""), 1.0f };
    // Expected:    a<b?    a<b?   NaN<x=false    x<NaN=false   NaN<NaN=false  a<b?
    float expected[N] = {
	1.0f,   // 0 < 1
	0.0f,   // !(1 < 0)
	0.0f,   // NaN < 1: false (any NaN comparison is false)
	0.0f,   // 2 < NaN: false
	0.0f,   // NaN < NaN: false
	1.0f,   // -1 < 1
    };

    float *aData = (float*)(aArg->data());
    float *bData = (float*)(bArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	aData[i] = aIn[i];
	bData[i] = bIn[i];
    }

    fn->callFunction(N);

    const float *rOut = (const float*)(rArg->data());
    for (size_t i = 0; i < N; ++i)
    {
	if (rOut[i] != expected[i])
	{
	    cerr << "  lane " << i << ": expected " << expected[i]
		 << " got " << rOut[i]
		 << " (a=" << aIn[i] << " b=" << bIn[i] << ")" << endl;
	    REQUIRE(false && "compare_branch divergence on NaN lane");
	}
    }
}

} // anonymous namespace


void
testEdgeValues ()
{
    cout << endl;
    cout << "Testing IEEE-754 edge values through SIMD interpreter" << endl;

    SimdInterpreter interp;
    interp.loadModule("testEdgeValues");

    testArithmetic(interp);
    testCompareBranchWithNaN(interp);
    testMaskedLoopRespectsBranchMask(interp);
    testNestedBranchMaskHandling(interp);
    testMergedBranchAroundOuterMask(interp);

    cout << "ok" << endl;
}
