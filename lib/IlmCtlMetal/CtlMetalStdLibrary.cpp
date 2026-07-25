///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Register Metal-backend stdlib symbols.
//
// Each entry binds a CTL stdlib name to a `MetalStdLibFuncAddr` whose
// `mslName()` points at a helper defined in the MSL preamble emitted
// by `MetalCodegen::emitStdLibPreamble`. Keeping the registration
// table small and explicit (no metaprogramming) makes it easy to read
// off which functions have landed and which still throw "undefined
// symbol" at CTL parse time.
//
// Naming convention: the MSL helper for CTL stdlib function `foo` is
// `ctl_stdlib_foo`. Generated CTL call sites emit a direct invocation
// of that name (see `MetalCallNode::generateCode`). The stdlib names
// registered here must match the CTL stdlib spec exactly (these are
// the names user CTL source types in); the MSL helper name is a free
// choice but kept mnemonic so the preamble and the registration file
// are easy to eyeball together.
//

#include <CtlMetalStdLibrary.h>

#include <CtlMetalAddr.h>

#include <CtlLContext.h>
#include <CtlSymbolTable.h>
#include <CtlType.h>

#include <vector>

namespace Ctl {

namespace {

//
// Build the CTL FunctionType `float (float)`. Mirrors the shape
// `SimdStdTypes::funcType_f_f` builds on the SIMD backend — one
// read-only scalar-float parameter, scalar-float return, non-varying.
//
FunctionTypePtr
funcTypeFloatFloat(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// Build `float (float, float)` -- two scalar-float read-only params,
// scalar-float return. Used by `fmod`, `pow`, `atan2`, `hypot`, etc.
//
FunctionTypePtr
funcTypeFloatFloatFloat(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    params.push_back(
        Param("a2", lcontext.newFloatType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// Build `bool (float)` -- one scalar-float read-only param, bool
// return. Used by `isfinite_f`, `isnan_f`, `isinf_f`, `isnormal_f`.
//
FunctionTypePtr
funcTypeBoolFloat(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newBoolType(), false, params);
}

//
// Convenience aliases for the fixed-size array element types used by
// the matrix/vector stdlib signatures. `f3` is `float[3]`, `f33` is
// `float[3][3]`. The SIMD backend names them identically
// (`SimdStdTypes::type_f3` / `type_f33`).
//
DataTypePtr typeF3(LContext &lcontext)
{
    return lcontext.newArrayType(lcontext.newFloatType(), 3);
}

DataTypePtr typeF33(LContext &lcontext)
{
    return lcontext.newArrayType(typeF3(lcontext), 3);
}

DataTypePtr typeF4(LContext &lcontext)
{
    return lcontext.newArrayType(lcontext.newFloatType(), 4);
}

DataTypePtr typeF44(LContext &lcontext)
{
    return lcontext.newArrayType(typeF4(lcontext), 4);
}

//
// `f3 (f3, f33)` -- vector * matrix. Used by `mult_f3_f33`.
//
FunctionTypePtr
funcTypeF3F3F33(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF3(lcontext),  0, RWA_READ, false));
    params.push_back(Param("a2", typeF33(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF3(lcontext), false, params);
}

//
// `f3 (f3, f3)` -- pair-of-vec3 binary ops: add_f3_f3, sub_f3_f3,
// cross_f3_f3.
//
FunctionTypePtr
funcTypeF3F3F3(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF3(lcontext), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF3(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF3(lcontext), false, params);
}

//
// `f3 (float, f3)` -- scalar-vector multiply.
//
FunctionTypePtr
funcTypeF3FF3(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF3(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF3(lcontext), false, params);
}

//
// `float (f3, f3)` -- pair-of-vec3 reduction: dot_f3_f3.
//
FunctionTypePtr
funcTypeFF3F3(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF3(lcontext), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF3(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// `f33 (float, f33)` -- scalar-matrix multiply: mult_f_f33.
//
FunctionTypePtr
funcTypeF33FF33(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF33(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF33(lcontext), false, params);
}

//
// `f33 (f33, f33)` -- pair-of-mat33 binary ops: add_f33_f33, mult_f33_f33.
//
FunctionTypePtr
funcTypeF33F33F33(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF33(lcontext), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF33(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF33(lcontext), false, params);
}

//
// `f33 (f33)` -- single-mat33 op: transpose_f33, invert_f33.
//
FunctionTypePtr
funcTypeF33F33(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF33(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF33(lcontext), false, params);
}

//
// `f44 (float, f44)` -- scalar-4x4 multiply.
//
FunctionTypePtr
funcTypeF44FF44(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF44(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF44(lcontext), false, params);
}

//
// `f44 (f44, f44)` -- pair-of-mat44 binary ops: add_f44_f44, mult_f44_f44.
//
FunctionTypePtr
funcTypeF44F44F44(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF44(lcontext), 0, RWA_READ, false));
    params.push_back(Param("a2", typeF44(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF44(lcontext), false, params);
}

//
// `f44 (f44)` -- single-mat44 op: transpose_f44, invert_f44.
//
FunctionTypePtr
funcTypeF44F44(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF44(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF44(lcontext), false, params);
}

//
// `f3 (f3, f44)` -- affine-transform vector multiply: mult_f3_f44.
//
FunctionTypePtr
funcTypeF3F3F44(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF3(lcontext),  0, RWA_READ, false));
    params.push_back(Param("a2", typeF44(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(typeF3(lcontext), false, params);
}

//
// `float (f3)` -- single-vec3 reduction: length_f3.
//
FunctionTypePtr
funcTypeFF3(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("a1", typeF3(lcontext), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// `float []` -- variable-size 1D float array type. Passed by value at
// the CTL level; lowered to a `thread const float*` plus a length
// uniform at the MSL level (see MetalFunctionNode / MetalCallNode).
//
DataTypePtr
typeF0(LContext &lcontext)
{
    return lcontext.newArrayType(lcontext.newFloatType(),
                                 0, LContext::PARAMETER);
}

//
// `float (float[], float, float, float)` -- lookup1D's signature.
// Matches `CtlSimdStdTypes::funcType_f_f0_f_f_f()` on the CPU side.
//
FunctionTypePtr
funcTypeF_F0_F_F_F(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("table", typeF0(lcontext),          0, RWA_READ, false));
    params.push_back(Param("pMin",  lcontext.newFloatType(),   0, RWA_READ, false));
    params.push_back(Param("pMax",  lcontext.newFloatType(),   0, RWA_READ, false));
    params.push_back(Param("p",     lcontext.newFloatType(),   0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// `float [][2]` -- variable-size array of fixed-size pair; used by
// interpolate1D/interpolateCubic1D for their (x,y) knot tables. The
// outer dim is the VSArray (passes as `thread const T* + uint len`);
// the inner dim is an MSL `metal::array<float, 2>`.
//
DataTypePtr
typeF02(LContext &lcontext)
{
    SizeVector sizes;
    sizes.push_back(0);
    sizes.push_back(2);
    return lcontext.newArrayType(lcontext.newFloatType(),
                                 sizes, LContext::PARAMETER);
}

//
// `float (float[][2], float)` -- signature of interpolate1D and
// interpolateCubic1D. Matches `CtlSimdStdTypes::funcType_f_f02_f()`.
//
FunctionTypePtr
funcTypeF_F02_F(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("table", typeF02(lcontext),        0, RWA_READ, false));
    params.push_back(Param("p",     lcontext.newFloatType(),  0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// `float[][][][3]` -- the variable-size 4D table shape used by the
// 3D-lookup stdlib family (`lookup3D_f3` / `_f` / `_h`). The outer
// three dimensions are each size()==0 (a multi-dim VSArray); the
// fourth dim is fixed at 3 (the channel axis). The Metal backend
// lowers this to a `thread const metal::array<float, 3>*` pointer
// plus three `uint` length uniforms (one per variable dim) via the
// multi-dim VSArray path in CtlMetalSyntaxTree — the pointee type
// is the leaf `float[3]`, and the length uniforms carry `size0`,
// `size1`, `size2` in declaration order.
//
DataTypePtr
typeF0003(LContext &lcontext)
{
    SizeVector sizes;
    sizes.push_back(0);
    sizes.push_back(0);
    sizes.push_back(0);
    sizes.push_back(3);
    return lcontext.newArrayType(lcontext.newFloatType(),
                                 sizes, LContext::PARAMETER);
}

//
// `float[3] (float[][][][3], float[3], float[3], float[3])` --
// signature of `lookup3D_f3`. Matches
// `CtlSimdStdTypes::funcType_f3_f0003_f3_f3_f3()`.
//
FunctionTypePtr
funcTypeF3_F0003_F3_F3_F3(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("table", typeF0003(lcontext), 0, RWA_READ, false));
    params.push_back(Param("pMin",  typeF3(lcontext),    0, RWA_READ, false));
    params.push_back(Param("pMax",  typeF3(lcontext),    0, RWA_READ, false));
    params.push_back(Param("p",     typeF3(lcontext),    0, RWA_READ, false));
    return lcontext.newFunctionType(typeF3(lcontext), false, params);
}

//
// `void (float[][][][3], float[3], float[3],
//        float, float, float,
//        output float, output float, output float)` -- signature of
// `lookup3D_f`. Matches
// `CtlSimdStdTypes::funcType_v_f0003_f3_f3_fff_offf()`. Unlike the
// f3-returning variant this form decomposes the coord triple and the
// return triple into scalar args — the CTL spec predates any aggregate
// return convention and keeps the ABI flat.
//
FunctionTypePtr
funcTypeV_F0003_F3_F3_FFF_OFFF(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("table", typeF0003(lcontext),      0, RWA_READ,  false));
    params.push_back(Param("pMin",  typeF3(lcontext),         0, RWA_READ,  false));
    params.push_back(Param("pMax",  typeF3(lcontext),         0, RWA_READ,  false));
    params.push_back(Param("p0",    lcontext.newFloatType(),  0, RWA_READ,  false));
    params.push_back(Param("p1",    lcontext.newFloatType(),  0, RWA_READ,  false));
    params.push_back(Param("p2",    lcontext.newFloatType(),  0, RWA_READ,  false));
    params.push_back(Param("q0",    lcontext.newFloatType(),  0, RWA_WRITE, false));
    params.push_back(Param("q1",    lcontext.newFloatType(),  0, RWA_WRITE, false));
    params.push_back(Param("q2",    lcontext.newFloatType(),  0, RWA_WRITE, false));
    return lcontext.newFunctionType(
        lcontext.newVoidType(), false, params);
}

//
// `void (float[][][][3], float[3], float[3],
//        half, half, half,
//        output half, output half, output half)` -- signature of
// `lookup3D_h`. Matches
// `CtlSimdStdTypes::funcType_v_f0003_f3_f3_hhh_ohhh()`. Same shape as
// the f variant but scalar coord/return args are `half`; the table
// stays `float[][][][3]` so the trilinear math runs at float precision
// (matching the CPU V3f code path exactly).
//
FunctionTypePtr
funcTypeV_F0003_F3_F3_HHH_OHHH(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("table", typeF0003(lcontext),      0, RWA_READ,  false));
    params.push_back(Param("pMin",  typeF3(lcontext),         0, RWA_READ,  false));
    params.push_back(Param("pMax",  typeF3(lcontext),         0, RWA_READ,  false));
    params.push_back(Param("p0",    lcontext.newHalfType(),   0, RWA_READ,  false));
    params.push_back(Param("p1",    lcontext.newHalfType(),   0, RWA_READ,  false));
    params.push_back(Param("p2",    lcontext.newHalfType(),   0, RWA_READ,  false));
    params.push_back(Param("q0",    lcontext.newHalfType(),   0, RWA_WRITE, false));
    params.push_back(Param("q1",    lcontext.newHalfType(),   0, RWA_WRITE, false));
    params.push_back(Param("q2",    lcontext.newHalfType(),   0, RWA_WRITE, false));
    return lcontext.newFunctionType(
        lcontext.newVoidType(), false, params);
}

//
// `float[2]` -- a fixed-size 2-element float array. Used as the member
// type for each of the four chromaticity coordinates in the
// `Chromaticities` struct. Matches `SimdStdTypes::type_f2`.
//
DataTypePtr
typeF2(LContext &lcontext)
{
    return lcontext.newArrayType(lcontext.newFloatType(), 2);
}

//
// `float[][2][3]` -- the scattered-data input shape used by
// `scatteredDataToGrid3D`. Outer dim is variable (one entry per RBF
// data pair); inner `[2][3]` is the `(V3f position, V3f value)` pair.
// Matches the `inSizes = {0, 2, 3}` shape built in
// `SimdStdTypes::funcType_v_f023_f3_f3_of0003`.
//
DataTypePtr
typeF023(LContext &lcontext)
{
    SizeVector sizes;
    sizes.push_back(0);
    sizes.push_back(2);
    sizes.push_back(3);
    return lcontext.newArrayType(lcontext.newFloatType(),
                                 sizes, LContext::PARAMETER);
}

//
// `void (float[][2][3], float[3], float[3], output float[][][][3])` --
// signature of `scatteredDataToGrid3D`. Matches
// `SimdStdTypes::funcType_v_f023_f3_f3_of0003`. The input data is an
// outer-variable-size array of `(V3f,V3f)` pairs; the output grid is
// a fully-variable 3D table of `V3f` samples.
//
FunctionTypePtr
funcTypeScatteredDataToGrid3D(LContext &lcontext)
{
    ParamVector params;
    params.push_back(Param("data", typeF023(lcontext),  0, RWA_READ,  false));
    params.push_back(Param("pMin", typeF3(lcontext),    0, RWA_READ,  false));
    params.push_back(Param("pMax", typeF3(lcontext),    0, RWA_READ,  false));
    params.push_back(Param("grid", typeF0003(lcontext), 0, RWA_WRITE, false));
    return lcontext.newFunctionType(
        lcontext.newVoidType(), false, params);
}

//
// Register the CTL `Chromaticities` struct in the Metal backend's
// symbol table. Mirrors `SimdStdTypes::type_chr` exactly — same member
// names, same order, same nested `float[2]` types — so CTL source that
// writes `Chromaticities c = {{rx,ry},{gx,gy},{bx,by},{wx,wy}};`
// parses identically on both backends. The MSL side emits a struct
// with mangled name `__Chromaticities`; that declaration is hardcoded
// at the top of `CtlMetalCodegen.cpp`'s preamble and pre-registered in
// `_declaredStructs` so `ensureStructDeclared` does not double-emit
// it when a user program references the type.
//
DataTypePtr
declareChromaticitiesType(LContext &lcontext)
{
    std::string structName =
        lcontext.symtab().getAbsoluteName("Chromaticities");

    MemberVector m;
    m.push_back(Member("red",   typeF2(lcontext)));
    m.push_back(Member("green", typeF2(lcontext)));
    m.push_back(Member("blue",  typeF2(lcontext)));
    m.push_back(Member("white", typeF2(lcontext)));

    DataTypePtr chr = lcontext.newStructType(structName, m);
    SymbolInfoPtr info = new SymbolInfo(/*module*/ 0, RWA_NONE,
                                        /*isTypeName*/ true, chr);
    lcontext.symtab().defineSymbol(structName, info);
    return chr;
}

//
// `f44 (Chromaticities, float)` -- signature of RGBtoXYZ and
// XYZtoRGB. Matches `CtlSimdStdTypes::funcType_f44_chr_f()`.
//
FunctionTypePtr
funcTypeF44ChrF(LContext &lcontext, const DataTypePtr &chr)
{
    ParamVector params;
    params.push_back(Param("a1", chr,                       0, RWA_READ, false));
    params.push_back(Param("a2", lcontext.newFloatType(),   0, RWA_READ, false));
    return lcontext.newFunctionType(typeF44(lcontext), false, params);
}

//
// `half (float)` — signature of `exp_h` and `pow10_h`. Matches
// `CtlSimdStdTypes::funcType_h_f()`.
//
FunctionTypePtr
funcTypeHalfFloat(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newFloatType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newHalfType(), false, params);
}

//
// `float (half)` — signature of `log_h` and `log10_h`. Matches
// `CtlSimdStdTypes::funcType_f_h()`.
//
FunctionTypePtr
funcTypeFloatHalf(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newHalfType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newFloatType(), false, params);
}

//
// `half (half, float)` — signature of `pow_h`. Matches
// `CtlSimdStdTypes::funcType_h_h_f()`.
//
FunctionTypePtr
funcTypeHalfHalfFloat(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newHalfType(),  0, RWA_READ, false));
    params.push_back(
        Param("a2", lcontext.newFloatType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newHalfType(), false, params);
}

//
// `void (T)` — signatures for each scalar print variant plus
// `print_string`. All are void-returning, single read-only scalar
// parameter. Mirrors `SimdStdTypes::funcType_v_{b,i,ui,h,f,s}`. The
// Metal backend lowers a `print_*` call to a no-op and emits a
// one-time stderr warning at codegen time — see comments at the call
// site in `MetalCallNode::generateCode` for why this is a no-op rather
// than a device-side I/O path.
//
FunctionTypePtr
funcTypeVoidScalar(LContext &lcontext, const DataTypePtr &t)
{
    ParamVector params;
    params.push_back(Param("a1", t, 0, RWA_READ, false));
    return lcontext.newFunctionType(lcontext.newVoidType(), false, params);
}

//
// `void (bool)` — `assert`'s signature. Matches
// `SimdStdTypes::funcType_v_b()`. The Metal assert MSL helper has a
// trailing `device atomic_uint* flag` parameter that the codegen
// appends at the call site from the `__ctl_err_flag` plumbing in the
// enclosing kernel/helper; the CTL-visible signature stays
// single-argument for source-level parity with the CPU backend.
//
FunctionTypePtr
funcTypeVoidBool(LContext &lcontext)
{
    ParamVector params;
    params.push_back(
        Param("a1", lcontext.newBoolType(), 0, RWA_READ, false));
    return lcontext.newFunctionType(
        lcontext.newVoidType(), false, params);
}

void
declareStdLibFunction(LContext &lcontext,
                      const std::string &ctlName,
                      const std::string &mslName,
                      const FunctionTypePtr &type)
{
    AddrPtr addr = new MetalStdLibFuncAddr(mslName);
    lcontext.symtab().defineSymbol(
        ctlName,
        new SymbolInfo(/*module*/ 0, RWA_NONE, /*isTypeName*/ false,
                       type, addr));
}

//
// Register a named CTL stdlib constant. The MSL identifier must match
// a `constant` global pre-declared in the codegen preamble (see the
// `ctl_const_*` block at the top of `kPreamble` in
// `CtlMetalCodegen.cpp`). Name lookup through `MetalNameNode` resolves
// to `MetalStaticAddr::mslName()` which pushes the MSL identifier
// verbatim on the expression stack — exactly the same path the CPU
// SIMD backend takes with `SimdDataAddr` and a persistent `SimdReg`.
//
void
declareStdLibConstant(LContext &lcontext,
                      const std::string &ctlName,
                      const std::string &mslName,
                      const DataTypePtr &type)
{
    AddrPtr addr = new MetalStaticAddr(mslName);
    lcontext.symtab().defineSymbol(
        ctlName,
        new SymbolInfo(/*module*/ 0, RWA_READ, /*isTypeName*/ false,
                       type, addr));
}

} // anonymous namespace

void
declareMetalStdLibrary(LContext &lcontext)
{
    FunctionTypePtr f_f   = funcTypeFloatFloat(lcontext);
    FunctionTypePtr f_f_f = funcTypeFloatFloatFloat(lcontext);
    FunctionTypePtr b_f   = funcTypeBoolFloat(lcontext);

    // IEEE-754-exact math functions -- MSL built-ins under
    // MTLMathModeSafe match CPU libm bit-for-bit because each is
    // specified as a correctly-rounded IEEE-754 operation with no
    // transcendental approximation. Keep this list aligned with the
    // CTL stdlib spec (see CtlSimdStdLibMath.cpp for the reference set
    // the CPU backend registers).
    declareStdLibFunction(lcontext, "fabs",  "ctl_stdlib_fabs",  f_f);
    declareStdLibFunction(lcontext, "floor", "ctl_stdlib_floor", f_f);
    declareStdLibFunction(lcontext, "sqrt",  "ctl_stdlib_sqrt",  f_f);
    declareStdLibFunction(lcontext, "fmod",  "ctl_stdlib_fmod",  f_f_f);

    // Limits / classification -- each is a pure bit-pattern test with
    // no rounding, so MSL built-ins are trivially bit-exact with libm.
    declareStdLibFunction(lcontext, "isfinite_f",
                          "ctl_stdlib_isfinite_f", b_f);
    declareStdLibFunction(lcontext, "isnormal_f",
                          "ctl_stdlib_isnormal_f", b_f);
    declareStdLibFunction(lcontext, "isnan_f",
                          "ctl_stdlib_isnan_f",    b_f);
    declareStdLibFunction(lcontext, "isinf_f",
                          "ctl_stdlib_isinf_f",    b_f);

    // Transcendentals -- BASELINE entries using metal::precise:: variants.
    // Each is expected to diverge from CPU libm by a small number of ULP
    // (see `lib/IlmCtlMetal/PRECISION.md` for current thresholds). The
    // threshold-bounded test fixtures in testMetalArithmetic serve as
    // regression guards; the 0-ULP closer is a hand-rolled FreeBSD-libm
    // port per function.
    declareStdLibFunction(lcontext, "exp",   "ctl_stdlib_exp",   f_f);
    declareStdLibFunction(lcontext, "log",   "ctl_stdlib_log",   f_f);
    declareStdLibFunction(lcontext, "log10", "ctl_stdlib_log10", f_f);
    declareStdLibFunction(lcontext, "pow",   "ctl_stdlib_pow",   f_f_f);
    declareStdLibFunction(lcontext, "sin",   "ctl_stdlib_sin",   f_f);
    declareStdLibFunction(lcontext, "cos",   "ctl_stdlib_cos",   f_f);
    declareStdLibFunction(lcontext, "tan",   "ctl_stdlib_tan",   f_f);
    declareStdLibFunction(lcontext, "asin",  "ctl_stdlib_asin",  f_f);
    declareStdLibFunction(lcontext, "acos",  "ctl_stdlib_acos",  f_f);
    declareStdLibFunction(lcontext, "atan",  "ctl_stdlib_atan",  f_f);
    declareStdLibFunction(lcontext, "atan2", "ctl_stdlib_atan2", f_f_f);
    declareStdLibFunction(lcontext, "sinh",  "ctl_stdlib_sinh",  f_f);
    declareStdLibFunction(lcontext, "cosh",  "ctl_stdlib_cosh",  f_f);
    declareStdLibFunction(lcontext, "tanh",  "ctl_stdlib_tanh",  f_f);
    declareStdLibFunction(lcontext, "hypot", "ctl_stdlib_hypot", f_f_f);
    declareStdLibFunction(lcontext, "pow10", "ctl_stdlib_pow10", f_f);

    //
    // Half-precision exp/log — 0-ULP table-lookup ports of the CPU
    // SIMD backend's `exp_h`/`log_h`/`log10_h`/`pow10_h` helpers. Each
    // MSL call site triggers `MetalCodegen::markHalfExpLogUsed()`,
    // which in turn causes `MetalCodegen::source()` to emit the three
    // precomputed tables (log10Table / logTable / expTable) plus the
    // four helper bodies into the MSL preamble. Modules that never call
    // any of the four pay zero compile-time cost for the ~12 MB of
    // tabulated data.
    //
    declareStdLibFunction(lcontext, "exp_h",   "ctl_stdlib_exp_h",
                          funcTypeHalfFloat(lcontext));
    declareStdLibFunction(lcontext, "log_h",   "ctl_stdlib_log_h",
                          funcTypeFloatHalf(lcontext));
    declareStdLibFunction(lcontext, "log10_h", "ctl_stdlib_log10_h",
                          funcTypeFloatHalf(lcontext));
    declareStdLibFunction(lcontext, "pow10_h", "ctl_stdlib_pow10_h",
                          funcTypeHalfFloat(lcontext));
    declareStdLibFunction(lcontext, "pow_h",   "ctl_stdlib_pow_h",
                          funcTypeHalfHalfFloat(lcontext));

    // Vector / matrix primitives. Each MSL helper hand-codes the exact
    // sequence of scalar mul/add operations that Imath (the CPU backend's
    // V3f / M33f implementation) uses, so 0-ULP parity is the target.
    FunctionTypePtr f3_f3_f33   = funcTypeF3F3F33(lcontext);
    FunctionTypePtr f3_f3_f3    = funcTypeF3F3F3(lcontext);
    FunctionTypePtr f3_f_f3     = funcTypeF3FF3(lcontext);
    FunctionTypePtr f_f3_f3     = funcTypeFF3F3(lcontext);
    FunctionTypePtr f_f3        = funcTypeFF3(lcontext);
    FunctionTypePtr f33_f_f33   = funcTypeF33FF33(lcontext);
    FunctionTypePtr f33_f33_f33 = funcTypeF33F33F33(lcontext);
    FunctionTypePtr f33_f33     = funcTypeF33F33(lcontext);

    declareStdLibFunction(lcontext, "mult_f3_f33",
                          "ctl_stdlib_mult_f3_f33", f3_f3_f33);
    declareStdLibFunction(lcontext, "mult_f_f3",
                          "ctl_stdlib_mult_f_f3",   f3_f_f3);
    declareStdLibFunction(lcontext, "add_f3_f3",
                          "ctl_stdlib_add_f3_f3",   f3_f3_f3);
    declareStdLibFunction(lcontext, "sub_f3_f3",
                          "ctl_stdlib_sub_f3_f3",   f3_f3_f3);
    declareStdLibFunction(lcontext, "cross_f3_f3",
                          "ctl_stdlib_cross_f3_f3", f3_f3_f3);
    declareStdLibFunction(lcontext, "dot_f3_f3",
                          "ctl_stdlib_dot_f3_f3",   f_f3_f3);
    declareStdLibFunction(lcontext, "length_f3",
                          "ctl_stdlib_length_f3",   f_f3);

    declareStdLibFunction(lcontext, "mult_f_f33",
                          "ctl_stdlib_mult_f_f33",   f33_f_f33);
    declareStdLibFunction(lcontext, "add_f33_f33",
                          "ctl_stdlib_add_f33_f33",  f33_f33_f33);
    declareStdLibFunction(lcontext, "mult_f33_f33",
                          "ctl_stdlib_mult_f33_f33", f33_f33_f33);
    declareStdLibFunction(lcontext, "transpose_f33",
                          "ctl_stdlib_transpose_f33", f33_f33);

    FunctionTypePtr f44_f_f44   = funcTypeF44FF44(lcontext);
    FunctionTypePtr f44_f44_f44 = funcTypeF44F44F44(lcontext);
    FunctionTypePtr f44_f44     = funcTypeF44F44(lcontext);
    FunctionTypePtr f3_f3_f44   = funcTypeF3F3F44(lcontext);

    declareStdLibFunction(lcontext, "mult_f_f44",
                          "ctl_stdlib_mult_f_f44",    f44_f_f44);
    declareStdLibFunction(lcontext, "add_f44_f44",
                          "ctl_stdlib_add_f44_f44",   f44_f44_f44);
    declareStdLibFunction(lcontext, "mult_f44_f44",
                          "ctl_stdlib_mult_f44_f44",  f44_f44_f44);
    declareStdLibFunction(lcontext, "transpose_f44",
                          "ctl_stdlib_transpose_f44", f44_f44);
    declareStdLibFunction(lcontext, "mult_f3_f44",
                          "ctl_stdlib_mult_f3_f44",   f3_f3_f44);

    //
    // Matrix inversion. Imath branches on matrix shape: affine
    // matrices go through the 3x3 adjugate/determinant fast path;
    // non-affine 4x4 inputs fall back to Gauss-Jordan elimination
    // with partial pivoting (`gjInverse`). The MSL helpers ported in
    // `CtlMetalCodegen.cpp` preserve both branches.
    //
    declareStdLibFunction(lcontext, "invert_f33",
                          "ctl_stdlib_invert_f33", f33_f33);
    declareStdLibFunction(lcontext, "invert_f44",
                          "ctl_stdlib_invert_f44", f44_f44);

    //
    // Lookup / interpolation. `lookup1D` takes a uniform `float[]` table
    // (VSArray) plus scalar pMin/pMax/p. The MSL helper mirrors
    // Imath's `indicesAndWeights` + the final
    // `table[i]*u1 + table[i1]*u` blend bit-for-bit, using explicit
    // `metal::fma` where Clang fuses two multiplies into one fmadd on
    // the CPU reference path.
    //
    declareStdLibFunction(lcontext, "lookup1D",
                          "ctl_stdlib_lookup1D",
                          funcTypeF_F0_F_F_F(lcontext));
    declareStdLibFunction(lcontext, "lookupCubic1D",
                          "ctl_stdlib_lookupCubic1D",
                          funcTypeF_F0_F_F_F(lcontext));
    declareStdLibFunction(lcontext, "interpolate1D",
                          "ctl_stdlib_interpolate1D",
                          funcTypeF_F02_F(lcontext));
    declareStdLibFunction(lcontext, "interpolateCubic1D",
                          "ctl_stdlib_interpolateCubic1D",
                          funcTypeF_F02_F(lcontext));

    //
    // `lookup3D_f3` — trilinear 3D lookup with packed f3 return. The
    // multi-dim VSArray `float[][][][3]` table type decays at the MSL
    // call site to `thread const metal::array<float, 3>*` plus three
    // `uint` length uniforms. The MSL helper in the codegen preamble
    // uses explicit `metal::fma` for every `a*b + c*d` pair so the
    // per-channel trilinear blend matches Clang's -O2 fmul/fmadd
    // sequence on CPU bit-for-bit.
    //
    // `lookup3D_f` / `lookup3D_h` share that trilinear body but expose
    // a scalar-in, scalar-out ABI with three `output float`/`half`
    // parameters. The stdlib call path in `MetalCallNode` already
    // forwards unmodified lvalues for write-only scalar args; the MSL
    // helpers in the codegen preamble take them as `thread T&` refs
    // and assign directly after delegating to the same trilinear core
    // as `lookup3D_f3`.
    //
    declareStdLibFunction(lcontext, "lookup3D_f3",
                          "ctl_stdlib_lookup3D_f3",
                          funcTypeF3_F0003_F3_F3_F3(lcontext));
    declareStdLibFunction(lcontext, "lookup3D_f",
                          "ctl_stdlib_lookup3D_f",
                          funcTypeV_F0003_F3_F3_FFF_OFFF(lcontext));
    declareStdLibFunction(lcontext, "lookup3D_h",
                          "ctl_stdlib_lookup3D_h",
                          funcTypeV_F0003_F3_F3_HHH_OHHH(lcontext));

    //
    // `lookup3DTetra_f3` / `_f` / `_h` — tetrahedral variants of the
    // trio above. Same signatures (the interpolation method is the
    // only difference), so the three function types are reused. The
    // MSL helpers in the codegen preamble share the trilinear
    // helper's clamp/index prologue and blend only the four corners
    // of the tetrahedron containing the fractional coordinates.
    //
    declareStdLibFunction(lcontext, "lookup3DTetra_f3",
                          "ctl_stdlib_lookup3DTetra_f3",
                          funcTypeF3_F0003_F3_F3_F3(lcontext));
    declareStdLibFunction(lcontext, "lookup3DTetra_f",
                          "ctl_stdlib_lookup3DTetra_f",
                          funcTypeV_F0003_F3_F3_FFF_OFFF(lcontext));
    declareStdLibFunction(lcontext, "lookup3DTetra_h",
                          "ctl_stdlib_lookup3DTetra_h",
                          funcTypeV_F0003_F3_F3_HHH_OHHH(lcontext));

    //
    // `scatteredDataToGrid3D` -- RBF interpolation over scattered
    // `(V3f position, V3f value)` pairs, sampled onto a regular 3D
    // grid. The CPU SIMD backend constructs a `Ctl::RbfInterpolator`
    // and samples it per grid cell (see
    // `CtlSimdStdLibInterpolator.cpp`). The Metal side intentionally
    // ships only an MSL stub that never writes to `grid`: no ACES
    // path calls this at GPU dispatch time, and the only in-tree
    // fixture that exercises the symbol (`unittest/IlmCtl/
    // testInterpolator.ctl`) does so entirely in module-init, where
    // `MetalInterpreter`'s SIMD sidecar already runs the genuine
    // `RbfInterpolator` and its assertions on the host.
    //
    // A real GPU implementation would detect this call at codegen
    // time, run the solve on the host at Module init, and bind the
    // resulting grid as an MTLBuffer the kernel reads; no runtime
    // caller has triggered that yet. If one does, rewrite the call
    // site in `MetalCallNode` rather than extending this stub.
    //
    declareStdLibFunction(lcontext, "scatteredDataToGrid3D",
                          "ctl_stdlib_scatteredDataToGrid3D",
                          funcTypeScatteredDataToGrid3D(lcontext));

    //
    // CIE colorspace conversions. The `f3 (f3, f3)` signature matches
    // `funcType_f3_f3_f3` on the CPU side. LuvtoXYZ / LabtoXYZ use
    // only rational math + t*t*t, so they target 0-ULP parity;
    // XYZtoLuv / XYZtoLab call `pow(x, 1/3)` internally and inherit
    // that function's ULP drift.
    //
    declareStdLibFunction(lcontext, "LuvtoXYZ",
                          "ctl_stdlib_LuvtoXYZ", f3_f3_f3);
    declareStdLibFunction(lcontext, "LabtoXYZ",
                          "ctl_stdlib_LabtoXYZ", f3_f3_f3);
    declareStdLibFunction(lcontext, "XYZtoLuv",
                          "ctl_stdlib_XYZtoLuv", f3_f3_f3);
    declareStdLibFunction(lcontext, "XYZtoLab",
                          "ctl_stdlib_XYZtoLab", f3_f3_f3);

    //
    // RGB <-> XYZ matrix builders. Unlike the `*toLab` / `*toLuv`
    // family these return a `float[4][4]` constructed from a
    // `Chromaticities` struct rather than operating on a per-pixel
    // triple — typical CTL programs call them once at top of scope to
    // build a constant colorspace matrix, then mult through with
    // `mult_f3_f44`. Registering the Chromaticities type symbol must
    // happen before building the function type so the parser can
    // resolve `Chromaticities` in source.
    //
    DataTypePtr chr = declareChromaticitiesType(lcontext);
    FunctionTypePtr f44_chr_f = funcTypeF44ChrF(lcontext, chr);
    declareStdLibFunction(lcontext, "RGBtoXYZ",
                          "ctl_stdlib_RGBtoXYZ", f44_chr_f);
    declareStdLibFunction(lcontext, "XYZtoRGB",
                          "ctl_stdlib_XYZtoRGB", f44_chr_f);

    //
    // `print_*` — the CTL stdlib spec exposes one print overload per
    // scalar type (bool, int, unsigned int, half, float, string). All
    // map to the same lowering strategy on Metal: the call site short-
    // circuits to a no-op and emits a one-time stderr warning.
    // Registering the symbols here means CTL programs that call
    // `print_float(x)` etc. resolve cleanly during parsing instead of
    // erroring out with "undefined symbol."
    //
    declareStdLibFunction(lcontext, "print_bool",
                          "ctl_stdlib_print_bool",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newBoolType()));
    declareStdLibFunction(lcontext, "print_int",
                          "ctl_stdlib_print_int",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newIntType()));
    declareStdLibFunction(lcontext, "print_unsigned_int",
                          "ctl_stdlib_print_unsigned_int",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newUIntType()));
    declareStdLibFunction(lcontext, "print_half",
                          "ctl_stdlib_print_half",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newHalfType()));
    declareStdLibFunction(lcontext, "print_float",
                          "ctl_stdlib_print_float",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newFloatType()));
    declareStdLibFunction(lcontext, "print_string",
                          "ctl_stdlib_print_string",
                          funcTypeVoidScalar(lcontext,
                                             lcontext.newStringType()));

    //
    // `assert(bool)` — full device-side implementation via an atomic
    // error flag buffer. The kernel wrapper binds the flag as an extra
    // buffer and threads it through every user helper as the
    // `__ctl_err_flag` trailing parameter. MetalCallNode rewrites calls
    // to `ctl_stdlib_assert` to include `__ctl_err_flag` as the second
    // argument. After dispatch, MetalFunctionCall inspects the flag
    // and, if nonzero, throws `Iex::LogicExc("CTL assertion failed.")`
    // to mirror the CPU backend's user-visible behavior exactly.
    //
    declareStdLibFunction(lcontext, "assert",
                          "ctl_stdlib_assert",
                          funcTypeVoidBool(lcontext));

    //
    // Named numeric constants. Each CTL name maps to an MSL `constant`
    // global defined in `CtlMetalCodegen.cpp`'s `kPreamble` — see the
    // `ctl_const_*` block there. Keep this table aligned with the CPU
    // SIMD backend's `defineConstants` in `CtlSimdStdLibLimits.cpp`.
    //
    DataTypePtr t_f  = lcontext.newFloatType();
    DataTypePtr t_h  = lcontext.newHalfType();
    DataTypePtr t_i  = lcontext.newIntType();
    DataTypePtr t_ui = lcontext.newUIntType();

    declareStdLibConstant(lcontext, "M_E",          "ctl_const_M_E",          t_f);
    declareStdLibConstant(lcontext, "M_PI",         "ctl_const_M_PI",         t_f);
    declareStdLibConstant(lcontext, "FLT_MAX",      "ctl_const_FLT_MAX",      t_f);
    declareStdLibConstant(lcontext, "FLT_MIN",      "ctl_const_FLT_MIN",      t_f);
    declareStdLibConstant(lcontext, "FLT_EPSILON",  "ctl_const_FLT_EPSILON",  t_f);
    declareStdLibConstant(lcontext, "FLT_POS_INF",  "ctl_const_FLT_POS_INF",  t_f);
    declareStdLibConstant(lcontext, "FLT_NEG_INF",  "ctl_const_FLT_NEG_INF",  t_f);
    declareStdLibConstant(lcontext, "FLT_NAN",      "ctl_const_FLT_NAN",      t_f);
    declareStdLibConstant(lcontext, "HALF_MAX",     "ctl_const_HALF_MAX",     t_h);
    declareStdLibConstant(lcontext, "HALF_MIN",     "ctl_const_HALF_MIN",     t_h);
    declareStdLibConstant(lcontext, "HALF_EPSILON", "ctl_const_HALF_EPSILON", t_h);
    declareStdLibConstant(lcontext, "HALF_POS_INF", "ctl_const_HALF_POS_INF", t_h);
    declareStdLibConstant(lcontext, "HALF_NEG_INF", "ctl_const_HALF_NEG_INF", t_h);
    declareStdLibConstant(lcontext, "HALF_NAN",     "ctl_const_HALF_NAN",     t_h);
    declareStdLibConstant(lcontext, "INT_MAX",      "ctl_const_INT_MAX",      t_i);
    declareStdLibConstant(lcontext, "INT_MIN",      "ctl_const_INT_MIN",      t_i);
    declareStdLibConstant(lcontext, "UINT_MAX",     "ctl_const_UINT_MAX",     t_ui);
}

} // namespace Ctl
