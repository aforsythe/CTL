///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Exercise the non-void kernel-entry return-value wiring: call a CTL
// function whose return type is a scalar (float, int) and confirm the
// value shows up in `FunctionCall::returnValue()->data()` after the
// dispatch. Covers both the varying-return case (per-sample result,
// `__ret0[tid] = ret0;`) and the uniform-return case (single-slot
// buffer written only by thread 0).
//

#include "testMetalReturnValue.h"

#include <CtlFunctionCall.h>
#include <CtlMetalDevice.h>
#include <CtlMetalInterpreter.h>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

const char *kReturnVaryingFloat =
    "namespace ret\n"
    "{\n"
    "    float doubleIt(input varying float x)\n"
    "    {\n"
    "        return x * 2.0;\n"
    "    }\n"
    "}\n";

const char *kReturnUniformInt =
    "namespace ret\n"
    "{\n"
    "    int answer()\n"
    "    {\n"
    "        return 42;\n"
    "    }\n"
    "}\n";

const char *kReturnVaryingFloat3 =
    "namespace ret\n"
    "{\n"
    "    float[3] scale(varying float v[3], varying float s)\n"
    "    {\n"
    "        float out[3];\n"
    "        out[0] = v[0] * s;\n"
    "        out[1] = v[1] * s;\n"
    "        out[2] = v[2] * s;\n"
    "        return out;\n"
    "    }\n"
    "}\n";

const char *kReturnUniformStruct =
    "namespace ret\n"
    "{\n"
    "    struct Pair\n"
    "    {\n"
    "        int a;\n"
    "        float b;\n"
    "    };\n"
    "\n"
    "    Pair makePair()\n"
    "    {\n"
    "        Pair p = { 7, 2.5 };\n"
    "        return p;\n"
    "    }\n"
    "}\n";

void
testVaryingFloatReturn()
{
    Ctl::MetalInterpreter interp;
    interp.loadModule("retFloat", "retFloat.ctl", kReturnVaryingFloat);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("ret::doubleIt");
    assert(fn);
    assert(fn->numInputArgs() == 1);
    assert(fn->numOutputArgs() == 0);

    const Ctl::FunctionArgPtr &in = fn->inputArg(0);
    float *inData = reinterpret_cast<float *>(in->data());
    const size_t N = 8;
    for (size_t i = 0; i < N; ++i)
        inData[i] = static_cast<float>(i) + 0.5f;

    fn->callFunction(N);

    const Ctl::FunctionArgPtr &ret = fn->returnValue();
    assert(ret);
    assert(ret->isVarying());

    const float *retData = reinterpret_cast<const float *>(ret->data());
    for (size_t i = 0; i < N; ++i) {
        const float expected = (static_cast<float>(i) + 0.5f) * 2.0f;
        assert(retData[i] == expected);
    }

    std::cout << "  varying float return — ok" << std::endl;
}

void
testUniformIntReturn()
{
    Ctl::MetalInterpreter interp;
    interp.loadModule("retInt", "retInt.ctl", kReturnUniformInt);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("ret::answer");
    assert(fn);
    assert(fn->numInputArgs() == 0);
    assert(fn->numOutputArgs() == 0);

    fn->callFunction(4);

    const Ctl::FunctionArgPtr &ret = fn->returnValue();
    assert(ret);
    assert(!ret->isVarying());

    const int32_t *retData = reinterpret_cast<const int32_t *>(ret->data());
    assert(retData[0] == 42);

    std::cout << "  uniform int return — ok" << std::endl;
}

void
testVaryingFloat3Return()
{
    Ctl::MetalInterpreter interp;
    interp.loadModule("retArr", "retArr.ctl", kReturnVaryingFloat3);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("ret::scale");
    assert(fn);

    const size_t N = 4;
    const Ctl::FunctionArgPtr &vIn = fn->inputArg(0);
    const Ctl::FunctionArgPtr &sIn = fn->inputArg(1);
    float *vData = reinterpret_cast<float *>(vIn->data());
    float *sData = reinterpret_cast<float *>(sIn->data());
    for (size_t i = 0; i < N; ++i) {
        vData[i * 3 + 0] = 1.0f + i;
        vData[i * 3 + 1] = 2.0f + i;
        vData[i * 3 + 2] = 3.0f + i;
        sData[i] = 10.0f;
    }

    fn->callFunction(N);

    const Ctl::FunctionArgPtr &ret = fn->returnValue();
    assert(ret->isVarying());

    const float *retData = reinterpret_cast<const float *>(ret->data());
    for (size_t i = 0; i < N; ++i) {
        assert(retData[i * 3 + 0] == (1.0f + i) * 10.0f);
        assert(retData[i * 3 + 1] == (2.0f + i) * 10.0f);
        assert(retData[i * 3 + 2] == (3.0f + i) * 10.0f);
    }

    std::cout << "  varying float[3] return — ok" << std::endl;
}

void
testUniformStructReturn()
{
    Ctl::MetalInterpreter interp;
    interp.loadModule("retStr", "retStr.ctl", kReturnUniformStruct);

    Ctl::FunctionCallPtr fn = interp.newFunctionCall("ret::makePair");
    assert(fn);

    fn->callFunction(4);

    const Ctl::FunctionArgPtr &ret = fn->returnValue();
    assert(!ret->isVarying());

    const char *retData = ret->data();
    const int32_t a = *reinterpret_cast<const int32_t *>(retData);
    const float b =
        *reinterpret_cast<const float *>(retData + sizeof(int32_t));
    assert(a == 7);
    assert(b == 2.5f);

    std::cout << "  uniform struct return — ok" << std::endl;
}

} // anonymous namespace

void
testMetalReturnValue()
{
    std::cout << "Testing Metal non-void kernel-entry return values"
              << std::endl;

    if (!Ctl::metalBackendAvailable()) {
        std::cout << "  Metal backend unavailable on this host — skipping."
                  << std::endl;
        return;
    }

    testVaryingFloatReturn();
    testUniformIntReturn();
    testVaryingFloat3Return();
    testUniformStructReturn();
}
