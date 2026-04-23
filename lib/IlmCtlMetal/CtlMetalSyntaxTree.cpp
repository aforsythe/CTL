///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Metal backend syntax-tree-node subclasses.  Each generateCode() emits
// MSL into the enclosing MetalModule's codegen buffer.
//

#include <CtlMetalSyntaxTree.h>
#include <CtlMetalAddr.h>
#include <CtlMetalCodegen.h>
#include <CtlMetalInterpreter.h>
#include <CtlMetalLContext.h>
#include <CtlMetalModule.h>
#include <CtlMetalType.h>
#include <CtlSimdAddr.h>
#include <CtlSimdInterpreter.h>
#include <CtlSimdReg.h>
#include <CtlSymbolTable.h>
#include <CtlTokens.h>
#include <CtlType.h>
#include <Iex.h>
#include <half.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>

namespace Ctl {

namespace {

[[noreturn]] void
notImplemented(const char *what)
{
    throw IEX_NAMESPACE::NoImplExc(
        std::string("CTL Metal backend: ") + what +
        "::generateCode is not supported for this node type.");
}

MetalCodegen &
codegenOf(LContext &lcontext)
{
    return static_cast<MetalLContext &>(lcontext).metalModule()->codegen();
}

//
// Fold a CTL module name into a valid MSL identifier fragment by mapping
// every character that would otherwise be illegal in a C identifier
// (`.`, `-`, etc.) to an underscore. The result is prepended to user
// function helper/kernel names so imports from different modules do not
// collide in the shared codegen.
//
std::string
sanitizeForIdentifier(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const bool ok =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '_';
        out += ok ? c : '_';
    }
    return out;
}

//
// Build the globally-unique base name for a user-defined function's
// MetalFunctionAddr. The name follows `<sanitized-module>__<function>`
// so the emitted helper is `__ctl_<sanitized-module>__<function>` and
// the kernel wrapper is `<sanitized-module>__<function>_kernel`.
// Modules with no name (internal stdlib scaffold) fall back to the
// bare function name.
//
std::string
qualifiedFunctionBaseName(const SymbolInfoPtr &info,
                          const std::string &functionName)
{
    if (!info) return functionName;
    const Module *mod = info->module();
    if (!mod || mod->name().empty()) return functionName;
    return sanitizeForIdentifier(mod->name()) + "__" + functionName;
}

std::string
paramMslName(int i)
{
    std::ostringstream s;
    s << "param" << i;
    return s.str();
}

std::string
argMslName(int i)
{
    std::ostringstream s;
    s << "__arg" << i;
    return s.str();
}

std::string
formatFloatLiteral(float v)
{
    //
    // MSL, like C++, rejects a trailing `f` suffix on a literal that has
    // no decimal point or exponent (`1f` is an integer with an invalid
    // suffix, not a float). `%.9g` is a nine-significant-digit round-trip
    // for IEEE-754 binary32, so the only case that needs a tweak is when
    // the formatted form has neither '.' nor 'e'/'E' (e.g. "1" or "-42").
    //
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
    std::string s(buf);
    bool hasDotOrExp = false;
    for (char c : s) {
        if (c == '.' || c == 'e' || c == 'E') {
            hasDotOrExp = true;
            break;
        }
    }
    if (!hasDotOrExp)
        s += ".0";
    s += 'f';
    return s;
}

std::string
formatHalfLiteral(half v)
{
    //
    // MSL accepts `h` as the half-float suffix (`1.5h`). Route through the
    // float round-trip formatter for the mantissa, strip the `f` suffix,
    // and append `h` so the constant is emitted with half precision.
    //
    std::string s = formatFloatLiteral(static_cast<float>(v));
    if (!s.empty() && s.back() == 'f')
        s.pop_back();
    s += 'h';
    return s;
}

const char *
binaryOpAsMsl(Token op)
{
    switch (op) {
        case TK_PLUS:         return "+";
        case TK_MINUS:        return "-";
        case TK_TIMES:        return "*";
        case TK_DIV:          return "/";
        case TK_MOD:          return "%";
        case TK_EQUAL:        return "==";
        case TK_NOTEQUAL:     return "!=";
        case TK_LESS:         return "<";
        case TK_LESSEQUAL:    return "<=";
        case TK_GREATER:      return ">";
        case TK_GREATEREQUAL: return ">=";
        case TK_AND:          return "&&";
        case TK_OR:           return "||";
        case TK_BITAND:       return "&";
        case TK_BITOR:        return "|";
        case TK_BITXOR:       return "^";
        case TK_LEFTSHIFT:    return "<<";
        case TK_RIGHTSHIFT:   return ">>";
        default:              return nullptr;
    }
}

const char *
unaryOpAsMsl(Token op)
{
    switch (op) {
        case TK_MINUS:  return "-";
        case TK_PLUS:   return "+";
        case TK_NOT:    return "!";
        case TK_BITNOT: return "~";
        default:        return nullptr;
    }
}

std::string
mslNameOf(const SymbolInfoPtr &info, const char *context)
{
    if (!info)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: ") + context +
            " references a null SymbolInfo.");

    MetalDataAddrPtr addr = info->addr().cast<MetalDataAddr>();
    if (!addr)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: ") + context +
            " references a symbol whose address is not a MetalDataAddr.");
    return addr->mslName();
}

void
generateStatementList(const StatementNodePtr &head, LContext &lcontext)
{
    for (StatementNodePtr s = head; s; s = s->next)
        s->generateCode(lcontext);
}

//
// Resolve a DataType's MSL spelling and, as a side effect, ensure that
// any struct types it references have been declared in the codegen
// header section. Array element types are unwrapped recursively so that
// a `metal::array<MyStruct, N>` triggers a `MyStruct` declaration.
//
std::string
registerAndNameType(MetalCodegen &cg, const DataType *type)
{
    const DataType *inner = type;
    bool sawVSArrayDim = false;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(inner)) {
        if (at->size() <= 0)
            sawVSArrayDim = true;
        inner = at->elementType().pointer();
    }
    if (const StructType *st = dynamic_cast<const StructType *>(inner))
        cg.ensureStructDeclared(st);
    if (sawVSArrayDim)
        cg.ensureVSArrayPlaceholderDeclared();
    return metalTypeName(type);
}

//
// MSL identifier for the runtime-length uniform that accompanies a
// VSArray parameter. The pair (`paramN`, `paramN_len`) flows through
// the helper signature, the call site, and the `.size` operator.
//
std::string
vsArrayLenName(const std::string &paramName)
{
    return paramName + "_len";
}

//
// Count the number of leading variable-size (size()==0) array
// dimensions on a parameter type, stopping at the first fixed-size
// (or non-array) level. For `float[]` this returns 1; for
// `float[][][][3]` (the `lookup3D_*` table shape) it returns 3; for
// any non-VSArray type it returns 0. The value drives the number of
// `_len` uniforms emitted alongside the pointer parameter and the
// number of `[0]` decay levels at the call site.
//
int
vsArrayDepth(const Type *type)
{
    int depth = 0;
    const Type *cur = type;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(cur)) {
        if (at->size() != 0)
            break;
        ++depth;
        cur = at->elementType().pointer();
    }
    return depth;
}

//
// Detect a variable-size array (VSArray) parameter. CTL represents it
// as an ArrayType with `size()==0`. Multi-dim VSArrays — the only
// shape CTL actually uses is `float[][][][3]` for the `lookup3D_*`
// table — are supported by treating each leading size()==0 level as
// an independent length uniform (`paramN_len_0`, `paramN_len_1`, ...).
// The leaf element type (the first fixed-size or scalar level) becomes
// the MSL pointee. Scalar-VSArray `float[]` continues to lower to a
// single `_len` uniform (no numeric suffix) for backward compatibility
// with the lookup1D / interpolate1D / interpolateCubic1D helpers.
//
bool
isVSArrayParam(const Type *type)
{
    return vsArrayDepth(type) >= 1;
}

//
// True if any dim along `type`'s array chain is variable-size
// (ArrayType with `size()==0`). Unlike `isVSArrayParam`, this catches
// trailing and interleaved VSArrays — `int[1][2][]` and similar
// signatures the CTL tests use — where the outer dim is fixed but an
// inner dim is variable. Any such parameter still lowers to a flat
// `thread T*` pointer plus one length uniform per variable dim.
//
bool
hasAnyVSArrayDim(const Type *type)
{
    const Type *cur = type;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(cur)) {
        if (at->size() == 0) return true;
        cur = at->elementType().pointer();
    }
    return false;
}

//
// Per-dim mask over the full array chain of `type`: `true` at index D
// means the D-th dim (from outer to inner) is variable-size.
// `float[][3]` → [true, false]; `float[][][][3]` → [true,true,true,false];
// `int[1][2][]` → [false,false,true]. Drives both length-uniform naming
// and the signature emission order.
//
std::vector<bool>
vsArrayVarMask(const Type *type)
{
    std::vector<bool> mask;
    const Type *cur = type;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(cur)) {
        mask.push_back(at->size() == 0);
        cur = at->elementType().pointer();
    }
    return mask;
}

//
// MSL identifier for the length uniform at original dim `origDim` of a
// VSArray parameter with the given var-dim mask. Backward-compatibility
// rule: when the parameter has exactly one variable dim AND it is at
// position 0 (the classic `float[]` / `float[][3]` shapes), the uniform
// keeps its unsuffixed name `paramN_len` — the same name the single-dim
// stdlib helpers (lookup1D, interpolate1D, ...) already expect. Every
// other shape — `float[][][][3]`, `int[1][2][]`, `int[3][]` — suffixes
// with the original dim index so the positional order of length
// arguments at call sites matches the positional order of length
// parameters in the helper signature.
//
std::string
vsDimLenName(const std::string &base,
             const std::vector<bool> &mask,
             int origDim)
{
    int numVar = 0;
    int firstVarPos = -1;
    for (size_t d = 0; d < mask.size(); ++d) {
        if (mask[d]) {
            ++numVar;
            if (firstVarPos < 0) firstVarPos = static_cast<int>(d);
        }
    }
    if (numVar == 1 && firstVarPos == 0)
        return base + "_len";
    return base + "_len_" + std::to_string(origDim);
}

//
// Total array depth for a VSArray parameter, counting every leading
// array level — variable OR fixed — until the first non-array leaf is
// reached. Examples: `float[]` → 1; `float[][2]` → 2;
// `float[][][][3]` → 4. This is the number of `[0]` decay levels the
// call site must emit to produce a `thread T*` pointing at the
// contiguous scalar backing of the caller's local array.
//
int
vsArrayTotalDepth(const Type *type)
{
    int depth = 0;
    const Type *cur = type;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(cur)) {
        ++depth;
        cur = at->elementType().pointer();
    }
    return depth;
}

//
// MSL spelling for the *scalar leaf* element type of a VSArray
// parameter. For every VSArray shape CTL actually uses — `float[]`,
// `float[][2]`, `float[][][][3]` — the leaf is the primitive scalar
// (`float`) reached after descending through every array level. The
// helper then indexes the flat scalar backing with explicit `stride*k`
// offsets. This avoids MSL's `metal::array<T, N>` aliasing surprises
// (pointer arithmetic through nested `metal::array` unexpectedly lost
// the association with the caller's initialized storage during
// `reinterpret_cast`-free accesses on M4 Max; the scalar-leaf form is
// immune because the caller decays `&var[0]...[0]` into a plain
// `thread const T*` at the call site).
//
std::string
vsArrayElementTypeName(MetalCodegen &cg, const DataType *arrType)
{
    const ArrayType *at = dynamic_cast<const ArrayType *>(arrType);
    if (!at)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: vsArrayElementTypeName called on a "
            "non-array type.");
    const DataType *cur = at->elementType().pointer();
    while (const ArrayType *sub = dynamic_cast<const ArrayType *>(cur)) {
        cur = sub->elementType().pointer();
    }
    return registerAndNameType(cg, cur);
}

//
// Walk a (possibly chained) ArrayIndex expression back to its base
// NameNode. For `arr[i][j][k]` this unwraps three layers of
// `ArrayIndexNode` and returns the `NameNode` for `arr`. Returns a
// null pointer if the chain bottoms out at a non-name expression
// (e.g. a call result), which rules out the chain being rooted in a
// VSArray parameter since CTL forbids local VSArrays and expression-
// yielded VSArrays.
//
NameNodePtr
arrayIndexChainBase(const ExprNodePtr &start)
{
    ExprNodePtr cur = start;
    while (cur) {
        ArrayIndexNodePtr ai = cur.cast<ArrayIndexNode>();
        if (!ai) break;
        cur = ai->array;
    }
    return cur.cast<NameNode>();
}

//
// Materialize a VSArray non-leaf index expression into a fresh
// fixed-size `metal::array<...>` tmp. `expr` is the CTL expression
// whose type is a fixed-size ArrayType but whose MSL codegen produced
// a raw `thread const T*` (via stride arithmetic into a VSArray
// backing). Both call-site argument passing and direct assignment to
// a fixed-size local need this materialization — MSL has no implicit
// conversion from `thread const T*` to `metal::array<T, N>`. Returns
// the tmp's MSL name if promotion applies; returns `frag` untouched
// otherwise. Emits the tmp declaration + element-copy loop nest into
// the current code block.
//
std::string
maybePromoteVSArraySliceToFixed(MetalCodegen &cg,
                                const ExprNodePtr &expr,
                                const std::string &frag)
{
    ArrayTypePtr argArr = expr->type.cast<ArrayType>();
    if (!argArr || argArr->size() <= 0)
        return frag;
    NameNodePtr chainRoot = arrayIndexChainBase(expr);
    const bool fromVSArrayChain =
        chainRoot && chainRoot->info &&
        hasAnyVSArrayDim(chainRoot->info->type().pointer()) &&
        expr.cast<ArrayIndexNode>();
    if (!fromVSArrayChain)
        return frag;

    std::vector<int> dims;
    ArrayTypePtr dimWalk = argArr;
    while (dimWalk) {
        if (dimWalk->size() <= 0)
            throw IEX_NAMESPACE::LogicExc(
                "CTL Metal backend: VSArray-to-fixed promotion hit a "
                "nested VSArray level.");
        dims.push_back(dimWalk->size());
        dimWalk = dimWalk->elementType().cast<ArrayType>();
    }
    const std::string tmp = cg.nextTempName();
    const std::string tname = registerAndNameType(cg, argArr.pointer());
    cg.writeln(tname + " " + tmp + ";");
    std::string loopIdx;
    std::string flatIdx;
    for (size_t k = 0; k < dims.size(); ++k) {
        const std::string iv = tmp + "_i" + std::to_string(k);
        cg.writeln("for (uint " + iv + " = 0; " + iv + " < " +
                   std::to_string(dims[k]) + "u; ++" + iv + ")");
        cg.writeln("{");
        cg.indent();
        loopIdx += "[" + iv + "]";
        int tailProduct = 1;
        for (size_t m = k + 1; m < dims.size(); ++m)
            tailProduct *= dims[m];
        if (!flatIdx.empty()) flatIdx += " + ";
        if (tailProduct == 1)
            flatIdx += iv;
        else
            flatIdx += iv + " * " + std::to_string(tailProduct) + "u";
    }
    cg.writeln(tmp + loopIdx + " = (" + frag + ")[" + flatIdx + "];");
    for (size_t k = 0; k < dims.size(); ++k) {
        cg.outdent();
        cg.writeln("}");
    }
    return tmp;
}

//
// Build the MSL expression for the flat-scalar stride of one element
// of a VSArray-backed type. The caller supplies the element type
// (`array->type->elementType()` at some ArrayIndex level), the root
// parameter's MSL name + full var-dim mask, and the 0-based index
// `firstElemOrigDim` of the first dimension of the element within
// the original root type. Each level of the element's array chain
// contributes one factor: a length uniform (`paramN_len[_D]`) for
// variable levels or a literal integer for fixed levels. Returns the
// empty string when the element is already scalar (stride == 1);
// callers treat that as `1`.
//
std::string
vsArrayChainStride(const DataType *elemType,
                   const std::string &rootMslName,
                   const std::vector<bool> &rootMask,
                   int firstElemOrigDim)
{
    std::string stride;
    const DataType *cur = elemType;
    int d = 0;
    while (const ArrayType *at = dynamic_cast<const ArrayType *>(cur)) {
        const int origDim = firstElemOrigDim + d;
        std::string dimExpr;
        if (at->size() == 0) {
            dimExpr = vsDimLenName(rootMslName, rootMask, origDim);
        } else {
            dimExpr = std::to_string(at->size()) + "u";
        }
        if (stride.empty()) stride = dimExpr;
        else stride += " * " + dimExpr;
        cur = at->elementType().pointer();
        ++d;
    }
    return stride;
}

//
// Depth-first search for any function call inside an initializer
// expression subtree — user-defined OR stdlib. Apple's current MSL
// compiler rejects *any* non-constexpr function call as a `constant`
// global initializer (the backend emits a global constructor, which
// the Metal runtime refuses with "cannot have global constructors"),
// so both user helpers and stdlib helpers like `sqrt` / `mult_f33_f33`
// must be routed through the host-side SIMD sidecar and substituted
// with a bit-exact literal aggregate at codegen time.
//
bool
containsUserFunctionCall(const ExprNodePtr &expr)
{
    if (!expr)
        return false;

    if (CallNodePtr call = expr.cast<CallNode>()) {
        return true;
    }
    if (BinaryOpNodePtr b = expr.cast<BinaryOpNode>()) {
        return containsUserFunctionCall(b->leftOperand) ||
               containsUserFunctionCall(b->rightOperand);
    }
    if (UnaryOpNodePtr u = expr.cast<UnaryOpNode>()) {
        return containsUserFunctionCall(u->operand);
    }
    if (ArrayIndexNodePtr ai = expr.cast<ArrayIndexNode>()) {
        return containsUserFunctionCall(ai->array) ||
               containsUserFunctionCall(ai->index);
    }
    if (MemberNodePtr m = expr.cast<MemberNode>()) {
        return containsUserFunctionCall(m->obj);
    }
    if (SizeNodePtr s = expr.cast<SizeNode>()) {
        return containsUserFunctionCall(s->obj);
    }
    if (ValueNodePtr v = expr.cast<ValueNode>()) {
        for (size_t i = 0; i < v->elements.size(); ++i)
            if (containsUserFunctionCall(v->elements[i]))
                return true;
        return false;
    }
    return false;
}

//
// Format the raw bytes of a sidecar-evaluated module-scope const as an
// MSL aggregate initializer matching the CTL data type's shape. The
// bytes come from a non-varying SimdReg that holds the value as a
// contiguous C-ABI blob (scalars at natural alignment, array elements
// at `alignedObjectSize()` stride, struct members at
// `Member::offset`). MSL's `metal::array<T,N>` wrapper needs doubled
// braces — same as `emitAggregateInit` — so the emitted spelling is
// directly substitutable for the initializer expression.
//
std::string
formatSidecarLiteral(const DataType *type, const char *bytes)
{
    if (!type || !bytes)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: formatSidecarLiteral called with a null "
            "type or byte pointer.");

    if (const ArrayType *at = dynamic_cast<const ArrayType *>(type)) {
        const DataType *et = at->elementType().pointer();
        const size_t stride = et->alignedObjectSize();
        std::string s = "{{";
        for (int i = 0; i < at->size(); ++i) {
            if (i) s += ", ";
            s += formatSidecarLiteral(et, bytes + i * stride);
        }
        s += "}}";
        return s;
    }
    if (const StructType *st = dynamic_cast<const StructType *>(type)) {
        std::string s = "{";
        const MemberVector &members = st->members();
        for (size_t i = 0; i < members.size(); ++i) {
            if (i) s += ", ";
            s += formatSidecarLiteral(members[i].type.pointer(),
                                      bytes + members[i].offset);
        }
        s += "}";
        return s;
    }
    if (dynamic_cast<const FloatType *>(type)) {
        float v;
        std::memcpy(&v, bytes, sizeof(v));
        return formatFloatLiteral(v);
    }
    if (dynamic_cast<const HalfType *>(type)) {
        half v;
        std::memcpy(&v, bytes, sizeof(v));
        return formatHalfLiteral(v);
    }
    if (dynamic_cast<const IntType *>(type)) {
        int v;
        std::memcpy(&v, bytes, sizeof(v));
        std::ostringstream s;
        s << v;
        return s.str();
    }
    if (dynamic_cast<const UIntType *>(type)) {
        unsigned v;
        std::memcpy(&v, bytes, sizeof(v));
        std::ostringstream s;
        s << v << "u";
        return s.str();
    }
    if (dynamic_cast<const BoolType *>(type)) {
        //
        // SimdBoolType stores a byte-sized bool (objectSize() == 1).
        // Read a single byte rather than sizeof(bool) to avoid reading
        // past the end of a packed boolean at the tail of a struct.
        //
        unsigned char v = static_cast<unsigned char>(bytes[0]);
        return v ? "true" : "false";
    }
    throw IEX_NAMESPACE::NoImplExc(
        std::string("CTL Metal backend: host-side const eval does not "
                    "support values of type '") + type->asString() +
        "'. Expected a numeric, bool, array, or struct leaf.");
}

} // anonymous namespace

//--- ModuleNode -------------------------------------------------------------

MetalModuleNode::MetalModuleNode(int lineNumber,
                                 const StatementNodePtr &constants,
                                 const FunctionNodePtr &functions)
    : ModuleNode(lineNumber, constants, functions)
{
}

void
MetalModuleNode::generateCode(LContext &lcontext)
{
    //
    // Module-level constants become MSL `constant` declarations emitted
    // into the header; function definitions become kernel entries in the
    // body.  Walk constants first so any forward references compile.
    //
    generateStatementList(constants, lcontext);

    for (FunctionNodePtr f = functions; f; f = f->next)
        f->generateCode(lcontext);
}

//--- FunctionNode -----------------------------------------------------------

MetalFunctionNode::MetalFunctionNode(int lineNumber,
                                     const std::string &name,
                                     const SymbolInfoPtr &info,
                                     const StatementNodePtr &body)
    : FunctionNode(lineNumber, name, info, body)
{
}

void
MetalFunctionNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    FunctionTypePtr ftype = info->functionType();
    if (!ftype)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalFunctionNode::generateCode called "
            "on a symbol whose type is not a FunctionType.");

    const ParamVector &params = ftype->parameters();
    const DataTypePtr &retType = ftype->returnType();
    const bool voidReturn = retType && retType.cast<VoidType>();

    MetalFunctionAddrPtr addr =
        new MetalFunctionAddr(qualifiedFunctionBaseName(info, name));
    const std::string fnName    = addr->helperName();
    const std::string kernelName = addr->kernelName();

    //
    // Emit the user-visible CTL function as an MSL `static inline` helper
    // and the callable entry-point as a `kernel void` wrapper that loads
    // inputs, invokes the helper, and writes outputs back. Splitting these
    // two means `return` inside the body becomes a native MSL `return;`
    // (MSL does not support labeled statements so we can't keep the body
    // flattened into the kernel), and the kernel's output-writeback block
    // always runs exactly once per thread regardless of how many `return`
    // statements the CTL source contains.
    //
    // The helper emits `ret0` as a `thread RET &` reference parameter for
    // non-void functions so MetalReturnNode's existing `ret0 = expr;`
    // assignment pattern continues to work unchanged.
    //
    //
    // A function is "kernel-callable" only when none of its parameters
    // are VSArrays. CTL forbids calling such a function from C++
    // (`Interpreter::newFunctionCall` throws at the host boundary), and
    // MSL has no natural way to express a runtime-length buffer at a
    // compute-kernel entry point without a separate length uniform per
    // parameter. We still emit the helper so other CTL functions can
    // invoke it — we just skip the kernel wrapper for this one.
    //
    bool hasVSArrayParam = false;
    for (size_t i = 0; i < params.size(); ++i) {
        if (hasAnyVSArrayDim(params[i].type.pointer()))
            hasVSArrayParam = true;
    }

    cg.write("static inline void ");
    cg.write(fnName);
    cg.write("(");
    bool firstParam = true;
    if (!voidReturn) {
        cg.write("thread ");
        cg.write(registerAndNameType(cg, retType.pointer()));
        cg.write(" &ret0");
        firstParam = false;
    }
    for (size_t i = 0; i < params.size(); ++i) {
        const Param &p = params[i];
        const std::string pname = paramMslName(static_cast<int>(i));
        if (!firstParam)
            cg.write(", ");
        firstParam = false;
        if (hasAnyVSArrayDim(p.type.pointer())) {
            //
            // VSArray parameters decay to a `thread` pointer + one
            // length uniform per variable dimension. The helper is
            // always `static inline`, so every caller's storage lives
            // in the thread address space and `thread` is the correct
            // qualifier for the pointee. Naming follows `vsDimLenName`:
            // a single variable dim at position 0 keeps the classic
            // `paramN_len` spelling (matches single-dim stdlib helpers
            // `lookup1D`/`interpolate1D`/etc.); every other shape —
            // `float[][][][3]`, `int[1][2][]`, `int[3][]` — suffixes
            // with the original dim index.
            //
            const std::string etype = vsArrayElementTypeName(cg, p.type.pointer());
            if (p.isWritable()) {
                cg.write("thread ");
                cg.write(etype);
                cg.write(" *");
                cg.write(pname);
            } else {
                cg.write("thread const ");
                cg.write(etype);
                cg.write(" *");
                cg.write(pname);
            }
            const std::vector<bool> mask = vsArrayVarMask(p.type.pointer());
            for (size_t d = 0; d < mask.size(); ++d) {
                if (!mask[d]) continue;
                cg.write(", uint ");
                cg.write(vsDimLenName(pname, mask, static_cast<int>(d)));
            }
            continue;
        }
        const std::string tname = registerAndNameType(cg, p.type.pointer());
        if (p.isWritable()) {
            cg.write("thread ");
            cg.write(tname);
            cg.write(" &");
            cg.write(pname);
        } else {
            cg.write(tname);
            cg.write(" ");
            cg.write(pname);
        }
    }
    //
    // Trailing `device atomic_uint* __ctl_err_flag` parameter. Every
    // user-defined helper (including VSArray helpers) takes the flag
    // so that any inner `assert()` call — or any onward call to
    // another helper — can forward it. Stdlib helpers other than
    // `assert` never reference the flag, so they do not take it.
    // MetalCallNode::generateCode appends the matching `__ctl_err_flag`
    // argument at every user-function and `assert` call site.
    //
    if (!firstParam)
        cg.write(", ");
    cg.write("device atomic_uint* __ctl_err_flag");
    //
    // halfExpLog table buffers. Unconditionally appended to every user
    // helper signature so they can be forwarded on any call — including
    // calls that happen before the enclosing function's body has been
    // walked (and thus before `_halfExpLogUsed` would be set). The three
    // device-const pointers land on consecutive kernel buffer slots
    // appended after `__ctl_err_flag`; MetalFunctionCall populates
    // them from the host-side halfExpLog data via persistent bindings
    // that allocate + upload once per pipeline.
    //
    cg.write(", device const uint* __ctl_half_log10_tbl");
    cg.write(", device const uint* __ctl_half_log_tbl");
    cg.write(", device const ushort* __ctl_half_exp_tbl");
    cg.writeln(")");
    cg.writeln("{");
    cg.indent();
    cg.pushEmittingFunction(name);
    try {
        generateStatementList(body, lcontext);
    } catch (...) {
        cg.popEmittingFunction();
        throw;
    }
    cg.popEmittingFunction();
    cg.outdent();
    cg.writeln("}");
    cg.writeln("");

    //
    // Functions with VSArray parameters cannot be invoked from the C++
    // API (`Interpreter::newFunctionCall` rejects them), so no compute
    // kernel entry point is ever needed for them. Register the helper
    // address and bail out before the kernel-wrapper emission — that
    // block's fixed-size-array assumptions would otherwise fail loudly
    // on an ArrayType whose `size()==0`.
    //
    if (hasVSArrayParam) {
        info->setAddr(addr);
        return;
    }

    //
    // Kernel wrapper: one thread per sample. Declare a local for each
    // CTL parameter (pre-populated from the input buffer for readable
    // params), optionally a local for the return value, call the helper,
    // then copy outputs back to their buffers.
    //
    cg.beginKernel(kernelName);
    for (size_t i = 0; i < params.size(); ++i) {
        cg.declareKernelBuffer(
            registerAndNameType(cg, params[i].type.pointer()), argMslName(i));
    }
    //
    // Non-void return value. Declared after the user params so the
    // buffer slot is deterministic: params 0..N-1, then `__ret0` at N,
    // then `__ctl_err_flag` at N+1 (or N for void). MetalFunctionCall
    // binds buffers in the same order. Varying returns land one slot
    // per sample; uniform returns are stored in a single-slot buffer
    // written only by thread 0 to avoid a cross-thread race on the
    // same address.
    //
    if (!voidReturn) {
        cg.declareKernelBuffer(
            registerAndNameType(cg, retType.pointer()), "__ret0");
    }
    //
    // Assertion error flag. Always declared as the last kernel buffer
    // so that `MetalFunctionCall::callFunction` can unconditionally
    // append a single 4-byte `uint` binding after the user-param
    // buffers. The `atomic_uint` pointer is threaded into the helper
    // invocation below as `__ctl_err_flag`; any `assert(cond)` that
    // sees `!cond` performs an atomic fetch-or to set bit 0 and the
    // host inspects the flag post-dispatch.
    //
    cg.declareKernelBuffer("atomic_uint", "__ctl_err_flag");
    //
    // halfExpLog table buffers. Always appended — see signature comment
    // above. Host side binds them as `persistent` MetalKernelBindings so
    // the ~739 KB of exp/log tables are allocated + uploaded once per
    // pipeline and reused for every dispatch.
    //
    cg.declareKernelBuffer("const uint", "__ctl_half_log10_tbl");
    cg.declareKernelBuffer("const uint", "__ctl_half_log_tbl");
    cg.declareKernelBuffer("const ushort", "__ctl_half_exp_tbl");
    cg.endKernel();

    cg.indent();

    if (!voidReturn) {
        cg.writeln(registerAndNameType(cg, retType.pointer()) + " ret0;");
    }

    for (size_t i = 0; i < params.size(); ++i) {
        const Param &p = params[i];
        const std::string pname = paramMslName(static_cast<int>(i));
        const std::string aname = argMslName(static_cast<int>(i));
        const std::string tname = registerAndNameType(cg, p.type.pointer());

        if (p.isReadable()) {
            if (p.varying)
                cg.writeln(tname + " " + pname +
                           " = " + aname + "[tid];");
            else
                cg.writeln(tname + " " + pname +
                           " = *" + aname + ";");
        } else {
            cg.writeln(tname + " " + pname + ";");
        }
    }

    {
        std::string call = fnName + "(";
        bool firstArg = true;
        if (!voidReturn) {
            call += "ret0";
            firstArg = false;
        }
        for (size_t i = 0; i < params.size(); ++i) {
            if (!firstArg)
                call += ", ";
            firstArg = false;
            call += paramMslName(static_cast<int>(i));
        }
        if (!firstArg)
            call += ", ";
        call += "__ctl_err_flag";
        call +=
            ", __ctl_half_log10_tbl, __ctl_half_log_tbl, __ctl_half_exp_tbl";
        call += ");";
        cg.writeln(call);
    }

    for (size_t i = 0; i < params.size(); ++i) {
        const Param &p = params[i];
        if (!p.isWritable())
            continue;
        const std::string pname = paramMslName(static_cast<int>(i));
        const std::string aname = argMslName(static_cast<int>(i));
        if (p.varying)
            cg.writeln(aname + "[tid] = " + pname + ";");
        else
            cg.writeln("*" + aname + " = " + pname + ";");
    }

    if (!voidReturn) {
        if (ftype->returnVarying())
            cg.writeln("__ret0[tid] = ret0;");
        else
            cg.writeln("if (tid == 0) *__ret0 = ret0;");
    }

    cg.outdent();
    cg.writeln("}");
    cg.writeln("");
    //
    // Close the per-kernel write redirection opened by `beginKernel`
    // so subsequent helper emissions land in the shared body again.
    //
    cg.finishKernel();

    info->setAddr(addr);
}

//--- VariableNode -----------------------------------------------------------

MetalVariableNode::MetalVariableNode(int lineNumber,
                                     const std::string &name,
                                     const std::string &absoluteName_,
                                     const SymbolInfoPtr &info,
                                     const ExprNodePtr &initialValue,
                                     bool assignInitialValue)
    : VariableNode(lineNumber, name, info, initialValue, assignInitialValue),
      absoluteName(absoluteName_)
{
}

void
MetalVariableNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    DataTypePtr dtype = info->dataType();
    if (!dtype)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalVariableNode::generateCode called on "
            "a symbol whose type is not a DataType.");

    //
    // Phase-2 guardrails. Variable-size arrays are only legal as
    // function parameters on the Metal backend (they lower to a
    // `device T* ptr, constant uint& len` pair). As a local variable
    // a VSArray has no statically-known size and cannot be emitted.
    // CTL's `string` type is only supported as the literal tag of a
    // future `print` / `assert` call; it has no GPU-side storage.
    //
    if (const ArrayType *at =
            dynamic_cast<const ArrayType *>(dtype.pointer()))
    {
        if (at->size() == 0)
            throw IEX_NAMESPACE::NoImplExc(
                "CTL Metal backend: variable-size arrays (VSArrays) are "
                "not supported as local variables. Pass them as function "
                "parameters instead.");
    }
    if (dynamic_cast<const StringType *>(dtype.pointer()))
        throw IEX_NAMESPACE::NoImplExc(
            "CTL Metal backend: `string` values are only valid as the "
            "literal argument to `print` / `assert`, not as variables.");

    const std::string localName = mslNameOf(info, "local variable");
    const std::string tname = registerAndNameType(cg, dtype.pointer());

    //
    // Module-scope static variables (the `const X = ...` declarations
    // inside a namespace, plus any future writable module globals) are
    // routed through MetalStaticAddr by the backend's newStaticVariable
    // implementations. Emit them as MSL `constant T name = init;`
    // globals in the codegen header. v1 requires an initializer that
    // MSL accepts as constexpr — most CTL module-level constants fit
    // naturally (literals, aggregate literals, arithmetic of literals).
    // Writable (non-const) module statics stay unsupported; they would
    // need runtime CPU-side init code and a GPU-visible buffer.
    //
    if (dynamic_cast<const MetalStaticAddr *>(info->addr().pointer())) {
        if (info->access() != RWA_READ)
            throw IEX_NAMESPACE::NoImplExc(
                "CTL Metal backend: writable module-scope statics are "
                "not yet supported. Mark module-level variables `const`.");
        if (!initialValue)
            throw IEX_NAMESPACE::NoImplExc(
                "CTL Metal backend: module-scope static `" + name +
                "` must have a constant initializer.");

        //
        // If the initializer calls a user-defined CTL function (directly
        // or transitively), MSL's `constant` storage qualifier will
        // reject it — only constexpr initializers are accepted there.
        // Substitute the value the host-side SIMD sidecar has already
        // evaluated for this symbol and emit an MSL aggregate literal
        // with bit-exact bytes. Parses of CTL that import helper
        // modules (ACES v2's `Lib.Academy.*.ctl`) rely on this path.
        //
        if (containsUserFunctionCall(initialValue)) {
            MetalLContext &mlcontext = static_cast<MetalLContext &>(lcontext);
            MetalInterpreter &interp = mlcontext.metalModule()->interpreter();

            size_t nbytes = 0;
            const char *bytes =
                interp.lookupSidecarBytes(absoluteName, nbytes);
            if (!bytes)
                throw IEX_NAMESPACE::LogicExc(
                    std::string("CTL Metal backend: host-side sidecar "
                                "has no evaluated value for '") +
                    absoluteName + "' (neither the persistent cache "
                    "nor the live sidecar's symbol table holds it). "
                    "Module-scope const initializers that call user "
                    "functions require the sidecar to evaluate them.");

            //
            // The Metal-side DataType (built by the current parse) is
            // structurally isomorphic to the one the sidecar built for
            // the same source, so formatSidecarLiteral drives off
            // `dtype` directly — the cache only needs to preserve
            // bytes, not the parsed type.
            //
            const std::string rhs =
                formatSidecarLiteral(dtype.pointer(), bytes);
            (void)nbytes;

            MetalCodegen::Section previous = cg.section();
            cg.setSection(MetalCodegen::Header);
            cg.writeln("constant " + tname + " " + localName +
                       " = " + rhs + ";");
            cg.setSection(previous);
            return;
        }

        initialValue->generateCode(lcontext);
        dtype->generateCastFrom(initialValue, lcontext);
        const std::string rhs = cg.popExpr();

        MetalCodegen::Section previous = cg.section();
        cg.setSection(MetalCodegen::Header);
        cg.writeln("constant " + tname + " " + localName + " = " + rhs + ";");
        cg.setSection(previous);
        return;
    }

    if (!initialValue) {
        cg.writeln(tname + " " + localName + ";");
        return;
    }

    if (assignInitialValue) {
        initialValue->generateCode(lcontext);
        dtype->generateCastFrom(initialValue, lcontext);
        const std::string rhs = cg.popExpr();
        cg.writeln(tname + " " + localName + " = " + rhs + ";");
    } else {
        //
        // Initializer writes to the variable by side effect (e.g. a call
        // that binds the variable as an output argument). Declare first,
        // then let the initializer expression's generateCode run for its
        // side-effect; any value pushed on the expression stack is
        // discarded the same way SimdVariableNode handles this case.
        //
        cg.writeln(tname + " " + localName + ";");
        const size_t depthBefore = cg.exprStackSize();
        initialValue->generateCode(lcontext);
        while (cg.exprStackSize() > depthBefore)
            (void)cg.popExpr();
    }
}

//--- AssignmentNode ---------------------------------------------------------

MetalAssignmentNode::MetalAssignmentNode(int lineNumber,
                                         const ExprNodePtr &lhs,
                                         const ExprNodePtr &rhs)
    : AssignmentNode(lineNumber, lhs, rhs)
{
}

void
MetalAssignmentNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    lhs->generateCode(lcontext);
    rhs->generateCode(lcontext);
    lhs->type->generateCastFrom(rhs, lcontext);

    std::string rhsStr = cg.popExpr();
    const std::string lhsStr = cg.popExpr();

    //
    // When the RHS is a non-leaf VSArray index (e.g. `table[i]` where
    // `table` is `float[][3]`), it yields a raw `thread const float*`
    // from stride arithmetic. If the LHS is a fixed-size array local
    // (`float lo[3]` → `metal::array<float, 3>`), MSL has no implicit
    // conversion. Materialize the slice into a fresh metal::array tmp
    // first, then assign from that.
    //
    rhsStr = maybePromoteVSArraySliceToFixed(cg, rhs, rhsStr);

    cg.writeln(lhsStr + " = " + rhsStr + ";");
}

//--- ExprStatementNode ------------------------------------------------------

MetalExprStatementNode::MetalExprStatementNode(int lineNumber,
                                               const ExprNodePtr &expr)
    : ExprStatementNode(lineNumber, expr)
{
}

void
MetalExprStatementNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    const size_t depthBefore = cg.exprStackSize();
    expr->generateCode(lcontext);

    if (cg.exprStackSize() > depthBefore) {
        const std::string s = cg.popExpr();
        cg.writeln(s + ";");
    }
}

//--- IfNode -----------------------------------------------------------------

MetalIfNode::MetalIfNode(int lineNumber,
                         const ExprNodePtr &condition,
                         const StatementNodePtr &truePath,
                         const StatementNodePtr &falsePath)
    : IfNode(lineNumber, condition, truePath, falsePath)
{
}

void
MetalIfNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    BoolTypePtr boolType = lcontext.newBoolType();
    condition->generateCode(lcontext);
    boolType->generateCastFrom(condition, lcontext);
    const std::string cond = cg.popExpr();

    cg.writeln("if (" + cond + ")");
    cg.writeln("{");
    cg.indent();
    generateStatementList(truePath, lcontext);
    cg.outdent();
    cg.writeln("}");

    if (falsePath) {
        cg.writeln("else");
        cg.writeln("{");
        cg.indent();
        generateStatementList(falsePath, lcontext);
        cg.outdent();
        cg.writeln("}");
    }
}

//--- ReturnNode -------------------------------------------------------------

MetalReturnNode::MetalReturnNode(int lineNumber,
                                 const SymbolInfoPtr &info,
                                 const ExprNodePtr &returnedValue)
    : ReturnNode(lineNumber, info, returnedValue)
{
}

void
MetalReturnNode::generateCode(LContext &lcontext)
{
    //
    // A bare `return;` in a void kernel just stops further work on this
    // thread. A `return expr;` stores the value into the return slot set
    // up by the enclosing function node and then short-circuits. Both
    // map straight to MSL `return;`.
    //
    MetalCodegen &cg = codegenOf(lcontext);

    if (returnedValue) {
        if (!info || !info->type())
            throw IEX_NAMESPACE::LogicExc(
                "CTL Metal backend: MetalReturnNode lacks a resolved "
                "return-value symbol.");
        returnedValue->generateCode(lcontext);
        info->type()->generateCastFrom(returnedValue, lcontext);
        const std::string value = cg.popExpr();
        const std::string retName = mslNameOf(info, "return-value slot");
        cg.writeln(retName + " = " + value + ";");
    }

    //
    // Inside the emitted `static inline` helper, MSL `return;` exits the
    // helper cleanly. MetalFunctionNode's kernel wrapper runs its output
    // writeback block after the helper returns regardless of how many
    // `return` statements appear in source.
    //
    cg.writeln("return;");
}

//--- WhileNode --------------------------------------------------------------

MetalWhileNode::MetalWhileNode(int lineNumber,
                               const ExprNodePtr &condition,
                               const StatementNodePtr &loopBody)
    : WhileNode(lineNumber, condition, loopBody)
{
}

void
MetalWhileNode::generateCode(LContext &lcontext)
{
    //
    // CTL semantics: the while-loop condition is re-evaluated each
    // iteration. MSL doesn't let us re-run an arbitrary MSL expression
    // from within the loop header without recomputing it, so we emit a
    // canonical `while (true)` with an early `break` gated on the
    // negated condition — that keeps the expression stack well-scoped
    // and re-evaluates the condition on every iteration.
    //
    MetalCodegen &cg = codegenOf(lcontext);

    BoolTypePtr boolType = lcontext.newBoolType();

    cg.writeln("while (true)");
    cg.writeln("{");
    cg.indent();

    condition->generateCode(lcontext);
    boolType->generateCastFrom(condition, lcontext);
    const std::string cond = cg.popExpr();
    cg.writeln("if (!(" + cond + ")) break;");

    generateStatementList(loopBody, lcontext);

    cg.outdent();
    cg.writeln("}");
}

//--- BinaryOpNode -----------------------------------------------------------

MetalBinaryOpNode::MetalBinaryOpNode(int lineNumber,
                                     Token op,
                                     const ExprNodePtr &leftOperand,
                                     const ExprNodePtr &rightOperand)
    : BinaryOpNode(lineNumber, op, leftOperand, rightOperand)
{
}

void
MetalBinaryOpNode::generateCode(LContext &lcontext)
{
    if (!operandType || !leftOperand || !rightOperand)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalBinaryOpNode::generateCode called "
            "on a node with unresolved operand type.");

    const char *opStr = binaryOpAsMsl(op);
    if (!opStr)
        throw IEX_NAMESPACE::NoImplExc(
            std::string("CTL Metal backend: unsupported binary operator ") +
            tokenAsString(op));

    MetalCodegen &cg = codegenOf(lcontext);

    //
    // Mirror the CPU SIMD pattern: evaluate each operand, cast to the
    // common operandType, then combine. MSL's && / || are short-circuit
    // at expression level (no explicit branch needed here).
    //
    // Note: CPU SimdInterpreter lowers each CTL binary op to a separate
    // SimdInst that stores its result to an arena buffer before the next
    // op reads it, which keeps `a*b + c` double-rounded on the CPU
    // reference. A peephole that folded `(a*b)+c` into `metal::fma` was
    // prototyped on 2026-04-22 but had to be reverted because it made
    // Metal single-rounded on that pattern and broke testMetalArithmetic
    // sample 18 (`x = a * 2.0 + b`) at 1-ULP strict parity. ACES v2
    // gain from the fusion was marginal (ch1 max ULP 712→622, ch2
    // 431→364 on marci-512); not worth the parity regression. If the
    // CPU SimdInterpreter's arithmetic lowering ever becomes
    // contract-on, revisit.
    //
    leftOperand->generateCode(lcontext);
    operandType->generateCastFrom(leftOperand, lcontext);
    rightOperand->generateCode(lcontext);
    operandType->generateCastFrom(rightOperand, lcontext);

    const std::string rhs = cg.popExpr();
    const std::string lhs = cg.popExpr();
    cg.pushExpr("(" + lhs + " " + opStr + " " + rhs + ")");
}

//--- UnaryOpNode ------------------------------------------------------------

MetalUnaryOpNode::MetalUnaryOpNode(int lineNumber,
                                   Token op,
                                   const ExprNodePtr &operand)
    : UnaryOpNode(lineNumber, op, operand)
{
}

void
MetalUnaryOpNode::generateCode(LContext &lcontext)
{
    if (!type || !operand)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalUnaryOpNode::generateCode called on "
            "a node with unresolved result type.");

    const char *opStr = unaryOpAsMsl(op);
    if (!opStr)
        throw IEX_NAMESPACE::NoImplExc(
            std::string("CTL Metal backend: unsupported unary operator ") +
            tokenAsString(op));

    MetalCodegen &cg = codegenOf(lcontext);

    operand->generateCode(lcontext);
    type->generateCastFrom(operand, lcontext);

    const std::string inner = cg.popExpr();

    //
    // Unary plus is a no-op on arithmetic types in CTL/MSL alike: emit
    // the operand without additional spelling so the stack still matches
    // what a parent node expects (a single expression fragment).
    //
    if (op == TK_PLUS) {
        cg.pushExpr(inner);
        return;
    }

    cg.pushExpr(std::string("(") + opStr + inner + ")");
}

//--- ArrayIndexNode ---------------------------------------------------------

MetalArrayIndexNode::MetalArrayIndexNode(int lineNumber,
                                         const ExprNodePtr &array,
                                         const ExprNodePtr &index)
    : ArrayIndexNode(lineNumber, array, index)
{
}

void
MetalArrayIndexNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    array->generateCode(lcontext);
    index->generateCode(lcontext);

    //
    // CTL allows indexing with any integral or boolean type; cast the
    // index to `int` so the emitted MSL matches what the CPU SIMD
    // backend does. `metal::array<T, N>::operator[]` takes a `size_t`
    // but accepts any integer argument.
    //
    IntTypePtr intType = lcontext.newIntType();
    intType->generateCastFrom(index, lcontext);

    const std::string idx = cg.popExpr();
    const std::string arr = cg.popExpr();

    //
    // VSArray-rooted chains back onto a flat `thread const T*` scalar
    // pointer (see `vsArrayElementTypeName` — the pointee type is the
    // primitive scalar reached after every array level). MSL's `[]`
    // operator on that pointer performs a scalar load, so the naive
    // `arr[i][j]` spelling doesn't parse for a nested VSArray — the
    // inner `arr[i]` yields a `float`, and the outer `[j]` has no
    // array to index. Detect the VSArray-rooted case and emit
    // explicit flat-scalar pointer arithmetic instead: at each index
    // level, multiply by the stride (= flat-scalar count of one
    // element at this level) and either continue producing a
    // `thread const T*` for non-leaf levels or perform the final
    // scalar load at the leaf. This mirrors the CPU SIMD backend's
    // own `(i * sy + j) * sz + k` indexing pattern for VSArrays.
    //
    NameNodePtr baseName = arrayIndexChainBase(array);
    const bool inVSArrayChain =
        baseName && baseName->info &&
        hasAnyVSArrayDim(baseName->info->type().pointer());

    if (inVSArrayChain) {
        MetalDataAddrPtr mda = baseName->info->addr().cast<MetalDataAddr>();
        if (!mda)
            throw IEX_NAMESPACE::LogicExc(
                "CTL Metal backend: VSArray chain base has no "
                "MetalDataAddr.");
        const std::string rootName = mda->mslName();
        const std::vector<bool> rootMask =
            vsArrayVarMask(baseName->info->type().pointer());
        const int rootTotal =
            vsArrayTotalDepth(baseName->info->type().pointer());

        ArrayTypePtr arrType = array->type.cast<ArrayType>();
        if (!arrType)
            throw IEX_NAMESPACE::LogicExc(
                "CTL Metal backend: ArrayIndex rooted in a VSArray "
                "with a non-array array-operand type.");

        //
        // 0-based position of this index within the chain: count how
        // many levels we've already consumed from the root. The
        // current index consumes original dim `P`; its element
        // type's first dim is original dim `P + 1`.
        //
        const int remainingTotal =
            vsArrayTotalDepth(array->type.pointer());
        const int P = rootTotal - remainingTotal;

        const std::string stride =
            vsArrayChainStride(arrType->elementType().pointer(),
                               rootName, rootMask, P + 1);

        ArrayTypePtr resultArrType = type.cast<ArrayType>();
        if (resultArrType) {
            //
            // Non-leaf index: produce a `thread (const) T*` pointer
            // into the flat scalar backing, offset by `idx * stride`
            // scalars. Parens around `arr` protect any compound
            // expression the previous level may have produced
            // (itself a nested pointer-arithmetic fragment).
            //
            if (stride.empty())
                cg.pushExpr("((" + arr + ") + (" + idx + "))");
            else
                cg.pushExpr("((" + arr + ") + (" + idx + ") * (" +
                            stride + "))");
        } else {
            //
            // Leaf index: perform the actual scalar load/store. When
            // stride is empty the element is already a scalar and
            // natural `[idx]` works; otherwise fold `idx * stride`
            // into the subscript so fixed-size tails (e.g. the `[3]`
            // channel axis of `float[][3]`) still index correctly.
            //
            if (stride.empty())
                cg.pushExpr("(" + arr + ")[" + idx + "]");
            else
                cg.pushExpr("(" + arr + ")[(" + idx + ") * (" +
                            stride + ")]");
        }
        return;
    }

    cg.pushExpr(arr + "[" + idx + "]");
}

//--- MemberNode -------------------------------------------------------------

MetalMemberNode::MetalMemberNode(int lineNumber,
                                 const ExprNodePtr &obj,
                                 const std::string &member)
    : MemberNode(lineNumber, obj, member)
{
}

void
MetalMemberNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    obj->generateCode(lcontext);
    const std::string o = cg.popExpr();
    cg.pushExpr(o + "." + member);
}

//--- SizeNode ---------------------------------------------------------------

MetalSizeNode::MetalSizeNode(int lineNumber, const ExprNodePtr &obj)
    : SizeNode(lineNumber, obj)
{
}

void
MetalSizeNode::generateCode(LContext &lcontext)
{
    //
    // `.size` lowers in three ways depending on what's being measured:
    //
    //   1. Fixed-size array → compile-time integer constant.
    //   2. VSArray param referenced directly by name → the outer
    //      length uniform (`paramN_len` for single-dim,
    //      `paramN_len_0` for multi-dim).
    //   3. Inner slice of a multi-dim VSArray (`arr[i].size`,
    //      `arr[i][j].size`) → the uniform for the dim at the
    //      slice's depth (`paramN_len_K` where K = number of
    //      index levels already consumed from the root).
    //
    // CTL forbids local VSArrays and the C++ API rejects VSArray
    // parameters at the host boundary, so the only VSArray value that
    // can reach `.size` is either a direct parameter name or an
    // index chain bottomed out at one. `arrayIndexChainBase` walks
    // that chain back to the root `NameNode`.
    //
    MetalCodegen &cg = codegenOf(lcontext);

    ArrayTypePtr arrType = obj->type.cast<ArrayType>();
    if (!arrType)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalSizeNode applied to a non-array type.");

    if (arrType->size() > 0) {
        cg.pushExpr(std::to_string(arrType->size()));
        return;
    }

    NameNodePtr nameNode = obj.cast<NameNode>();
    int levelsConsumed = 0;
    if (!nameNode) {
        nameNode = arrayIndexChainBase(obj);
        if (!nameNode)
            throw IEX_NAMESPACE::NoImplExc(
                "CTL Metal backend: `.size` on a variable-size array is only "
                "supported when the array is referenced by a name or a "
                "VSArray-rooted index chain.");
        const int rootTotal =
            vsArrayTotalDepth(nameNode->info->type().pointer());
        const int remainingTotal =
            vsArrayTotalDepth(obj->type.pointer());
        levelsConsumed = rootTotal - remainingTotal;
    }
    if (!nameNode->info)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: VSArray `.size` base has no SymbolInfo.");

    MetalDataAddrPtr mda = nameNode->info->addr().cast<MetalDataAddr>();
    if (!mda)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: VSArray `.size` name has no MetalDataAddr.");

    const std::vector<bool> mask =
        vsArrayVarMask(nameNode->info->type().pointer());
    if (levelsConsumed >= static_cast<int>(mask.size()) ||
        !mask[levelsConsumed])
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: VSArray `.size` targets a dim that is "
            "not variable-size.");

    cg.pushExpr(vsDimLenName(mda->mslName(), mask, levelsConsumed));
}

//--- NameNode ---------------------------------------------------------------

MetalNameNode::MetalNameNode(int lineNumber,
                             const std::string &name,
                             const SymbolInfoPtr &info)
    : NameNode(lineNumber, name, info)
{
}

void
MetalNameNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    //
    // Parser error-recovery path: when a CTL source references an
    // undefined name, the parser reports the error but still builds a
    // NameNode with a null SymbolInfo so parsing can continue. Modules
    // that pair every actual error with an `@error` marker reach
    // codegen with numErrors() == 0. Emit a placeholder identifier so
    // subsequent expression assembly has something syntactically legal;
    // the surrounding expression's result is never observed at runtime
    // because a module with unresolved names has no live entry points.
    //
    if (!info) {
        cg.pushExpr("__ctl_err_placeholder_name");
        return;
    }

    cg.pushExpr(mslNameOf(info, "name reference"));
}

//--- Literal nodes ----------------------------------------------------------

MetalBoolLiteralNode::MetalBoolLiteralNode(int lineNumber,
                                           const LContext &lcontext,
                                           bool value_)
    : BoolLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalBoolLiteralNode::generateCode(LContext &lcontext)
{
    codegenOf(lcontext).pushExpr(value ? "true" : "false");
}
char *MetalBoolLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

MetalIntLiteralNode::MetalIntLiteralNode(int lineNumber,
                                         const LContext &lcontext,
                                         int value_)
    : IntLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalIntLiteralNode::generateCode(LContext &lcontext)
{
    //
    // MSL accepts decimal integer literals the same way C++ does. INT_MIN
    // is emitted as `(-2147483647 - 1)` so the parser never sees a bare
    // literal that would exceed the signed-int range.
    //
    if (value == (std::numeric_limits<int>::min)()) {
        codegenOf(lcontext).pushExpr("(-2147483647 - 1)");
        return;
    }
    codegenOf(lcontext).pushExpr(std::to_string(value));
}
char *MetalIntLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

MetalUIntLiteralNode::MetalUIntLiteralNode(int lineNumber,
                                           const LContext &lcontext,
                                           unsigned value_)
    : UIntLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalUIntLiteralNode::generateCode(LContext &lcontext)
{
    codegenOf(lcontext).pushExpr(std::to_string(value) + "u");
}
char *MetalUIntLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

MetalHalfLiteralNode::MetalHalfLiteralNode(int lineNumber,
                                           const LContext &lcontext,
                                           half value_)
    : HalfLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalHalfLiteralNode::generateCode(LContext &lcontext)
{
    codegenOf(lcontext).pushExpr(formatHalfLiteral(value));
}
char *MetalHalfLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

MetalFloatLiteralNode::MetalFloatLiteralNode(int lineNumber,
                                             const LContext &lcontext,
                                             float value_)
    : FloatLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalFloatLiteralNode::generateCode(LContext &lcontext)
{
    codegenOf(lcontext).pushExpr(formatFloatLiteral(value));
}
char *MetalFloatLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

MetalStringLiteralNode::MetalStringLiteralNode(int lineNumber,
                                               const LContext &lcontext,
                                               const std::string &value_)
    : StringLiteralNode(lineNumber, lcontext, value_)
{
}

void MetalStringLiteralNode::generateCode(LContext &) { notImplemented("MetalStringLiteralNode"); }
char *MetalStringLiteralNode::valuePtr() { return reinterpret_cast<char *>(&value); }

//--- CallNode ---------------------------------------------------------------

MetalCallNode::MetalCallNode(int lineNumber,
                             const NameNodePtr &function,
                             const ExprNodeVector &arguments)
    : CallNode(lineNumber, function, arguments)
{
}

void
MetalCallNode::generateCode(LContext &lcontext)
{
    MetalCodegen &cg = codegenOf(lcontext);

    if (!function)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: MetalCallNode has no function name node.");

    //
    // Parser error-recovery path: the CTL parser reports an undefined
    // name but still builds a NameNode with null SymbolInfo so that
    // parsing can continue to collect further diagnostics. Modules
    // whose actual errors are all paired with `@error` markers reach
    // codegen with numErrors() == 0. Emit a no-op so the walk survives;
    // the declared-error harness has already done its job and there is
    // no real function body to call.
    //
    if (!function->info) {
        cg.pushExpr("((void)0)");
        return;
    }

    if (cg.isEmittingFunction(function->name))
        throw IEX_NAMESPACE::NoImplExc(
            std::string("CTL Metal backend: recursive call to '") +
            function->name + "' is not supported. MSL does not allow "
            "recursion.");

    MetalFunctionAddrPtr addr =
        function->info->addr().cast<MetalFunctionAddr>();
    MetalStdLibFuncAddrPtr stdlibAddr =
        function->info->addr().cast<MetalStdLibFuncAddr>();
    if (!addr && !stdlibAddr)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: function '") +
            function->name + "' has no MetalFunctionAddr or "
            "MetalStdLibFuncAddr. Did codegen run on its definition "
            "before the call site?");

    //
    // `print_*` stdlib calls short-circuit to a no-op at the call site.
    // Two reasons to bypass the normal argument-codegen path:
    //
    //   1. MSL has no device-side print facility equivalent to `fprintf`,
    //      so the MSL helpers are no-ops that discard their operand. The
    //      CPU backend's per-call stderr output is best-effort in the
    //      Metal backend; we emit a one-time warning at codegen time.
    //
    //   2. `print_string` takes a CTL `string`, which has no GPU-side
    //      representation (MetalStringLiteralNode::generateCode throws
    //      NoImplExc). Skipping argument codegen for the entire print_*
    //      family lets string-argument prints compile without a
    //      dedicated string-handling path, at the cost of the runtime
    //      output — acceptable for stdlib registration.
    //
    // The emitted MSL is the literal `((void)0)` expression so the
    // enclosing ExprStatementNode still gets something legal to
    // terminate with a semicolon.
    //
    if (stdlibAddr) {
        const std::string &mn = stdlibAddr->mslName();
        if (mn.compare(0, 17, "ctl_stdlib_print_") == 0) {
            if (cg.markPrintUsed()) {
                std::cerr << "ctl: the Metal backend treats `print_*` "
                             "as a no-op; runtime output will be "
                             "suppressed." << std::endl;
            }
            cg.pushExpr("((void)0)");
            return;
        }
    }

    FunctionTypePtr ftype = function->info->functionType();
    if (!ftype)
        throw IEX_NAMESPACE::LogicExc(
            std::string("CTL Metal backend: function symbol '") +
            function->name + "' has a non-FunctionType.");

    const ParamVector &params = ftype->parameters();
    const DataTypePtr &retType = ftype->returnType();
    const bool voidReturn = retType && retType.cast<VoidType>();

    //
    // Walk each parameter in declaration order. Input args are cast to
    // the parameter type (CTL's type checker has already verified the
    // cast is legal); output args must be lvalues and are emitted as-is
    // because MSL requires `thread T &` parameters to bind to actual
    // storage, not a temporary. CTL's parser enforces both the lvalue
    // and same-type requirements, so no additional checking is needed
    // here.
    //
    // VSArray parameters take (1 + depth) MSL arguments each: a pointer
    // to the first element and one length uniform per variable
    // dimension. `argFragments[i]` holds the pointer fragment;
    // `argLenFragments[i]` is empty for non-VSArray params, has one
    // entry for single-dim VSArrays (`float[]`), and has N entries for
    // a depth-N multi-dim VSArray (only `float[][][][3]` is currently
    // exercised, by `lookup3D_f3`).
    //
    std::vector<std::string> argFragments;
    std::vector<std::vector<std::string>> argLenFragments;
    argFragments.reserve(params.size());
    argLenFragments.reserve(params.size());

    //
    // For each writable VSArray param whose caller-side is a fixed-
    // size local (not a forwarded VSArray), we route the pointer
    // through a fresh C-array tmp and must copy the helper's writes
    // back into the caller's array after the call completes. Each
    // entry records what's needed to emit that reverse-copy loop
    // nest: the caller lvalue, the tmp name, and the per-level
    // extent. Forwarding cases (caller is itself a VSArray param)
    // skip this because the helper writes directly into the shared
    // backing store.
    //
    struct VSArrayWriteback {
        std::string lvalue;     // caller-side lvalue expression
        std::string tmp;        // fresh C-array tmp name
        std::vector<int> dims;  // full fixed-size shape (outer→inner)
    };
    std::vector<VSArrayWriteback> writebacks;

    for (size_t i = 0; i < params.size(); ++i) {
        ExprNodePtr argExpr;
        if (i >= arguments.size()) {
            argExpr = params[i].defaultValue;
            if (!argExpr)
                throw IEX_NAMESPACE::LogicExc(
                    std::string("CTL Metal backend: call to '") +
                    function->name + "' is missing argument " +
                    std::to_string(i) + " and the parameter has no "
                    "default value.");
        } else {
            argExpr = arguments[i];
        }

        const bool formalIsVSArray = hasAnyVSArrayDim(params[i].type.pointer());

        argExpr->generateCode(lcontext);
        //
        // CTL's cast machinery considers a fixed-size array assignable
        // to a VSArray of the same element type (and vice versa for
        // forwarding). Skip the cast for VSArray params — we'll emit
        // the pointer decay and the length uniform by hand below, and
        // `generateCastFrom` for ArrayType is a no-op on Metal anyway.
        //
        if (params[i].isReadable() && !formalIsVSArray)
            params[i].type->generateCastFrom(argExpr, lcontext);

        std::string frag = cg.popExpr();

        //
        // Promotion: VSArray-chain non-leaf index expression → fixed-size
        // array formal. `testJMh[testIndex]` where `testJMh` is
        // `float[][3]` emits a raw `thread const float*` via
        // `MetalArrayIndexNode`'s VSArray path, but a callee expecting
        // `float[3]` has signature `metal::array<float, 3>` by value.
        // MSL has no implicit conversion, so materialize a fresh
        // `metal::array<...>` tmp, copy element-by-element from the
        // flat pointer, and pass the tmp as the argument. The copy
        // respects the fixed-size shape encoded in argExpr->type
        // (single-level here, but the same loop nest generalizes to
        // nested fixed-size tails if a future transform needs it).
        //
        if (!formalIsVSArray)
            frag = maybePromoteVSArraySliceToFixed(cg, argExpr, frag);

        if (formalIsVSArray) {
            //
            // Pointer decay and length-uniform emission. The formal
            // signature carries one `uint paramN_len[_D]` per variable
            // dim (at its original position in the formal type); we
            // walk the formal's var-dim positions and emit one length
            // expression per position, either a compile-time literal
            // (when the caller's dim at that position is fixed) or a
            // forwarded uniform (when the caller's dim is itself
            // variable). The pointer side produces a `thread (const)
            // T*` pointing at the contiguous scalar backing — either
            // the caller's own pointer (forwarding from a VSArray
            // parameter, any shape: leading, trailing, or mixed) or a
            // freshly-copied `metal::array`-free tmp whose address
            // breaks the M4 Max aliasing hazard on runtime-indexed
            // inner reads.
            //
            std::string leafType =
                vsArrayElementTypeName(cg, params[i].type.pointer());
            const bool paramWritable = params[i].isWritable();

            ArrayTypePtr actualArr = argExpr->type.cast<ArrayType>();
            if (!actualArr)
                throw IEX_NAMESPACE::LogicExc(
                    std::string("CTL Metal backend: call to '") +
                    function->name + "' passes a non-array expression to "
                    "variable-size array parameter '" +
                    params[i].name + "'.");

            const bool callerIsVSArray =
                hasAnyVSArrayDim(argExpr->type.pointer());

            std::string ptrExpr;
            if (callerIsVSArray) {
                ptrExpr = frag;
            } else {
                //
                // Caller is a fully-fixed array — a local, a struct
                // field, or a function return, all nested
                // `metal::array<...>`. Copy the contents into a plain
                // C-array local whose address is safe to reinterpret
                // as `thread (const) T*` without tripping Metal's
                // `metal::array` alias analyzer. For writable params
                // also record a writeback so the reverse copy runs
                // after the call statement.
                //
                std::vector<int> dims;
                ArrayTypePtr dimWalk = actualArr;
                while (dimWalk) {
                    if (dimWalk->size() <= 0)
                        throw IEX_NAMESPACE::LogicExc(
                            "CTL Metal backend: unexpected VSArray "
                            "level in caller-side fixed-size array.");
                    dims.push_back(dimWalk->size());
                    dimWalk = dimWalk->elementType().cast<ArrayType>();
                }

                const std::string tmp = cg.nextTempName();
                std::string shape;
                for (int d : dims)
                    shape += "[" + std::to_string(d) + "]";
                cg.writeln(leafType + " " + tmp + shape + ";");

                std::string idx;
                for (size_t k = 0; k < dims.size(); ++k) {
                    std::string iv = tmp + "_i" + std::to_string(k);
                    cg.writeln("for (uint " + iv + " = 0; " + iv +
                               " < " + std::to_string(dims[k]) +
                               "u; ++" + iv + ")");
                    cg.writeln("{");
                    cg.indent();
                    idx += "[" + iv + "]";
                }
                cg.writeln(tmp + idx + " = (" + frag + ")" + idx + ";");
                for (size_t k = 0; k < dims.size(); ++k) {
                    cg.outdent();
                    cg.writeln("}");
                }

                if (paramWritable)
                    ptrExpr = "((thread " + leafType +
                              "*)&(" + tmp + "))";
                else
                    ptrExpr = "((thread const " + leafType +
                              "*)&(" + tmp + "))";

                if (paramWritable) {
                    VSArrayWriteback wb;
                    wb.lvalue = frag;
                    wb.tmp    = tmp;
                    wb.dims   = dims;
                    writebacks.push_back(wb);
                }
            }

            //
            // Forwarding-source name + mask, only needed when the
            // caller has a variable-dim position the formal expects
            // to receive as a forwarded uniform. CTL forbids local
            // VSArrays, so any caller whose type carries a variable
            // dim must be a direct parameter reference.
            //
            NameNodePtr fwdNameNode;
            MetalDataAddrPtr fwdMda;
            std::vector<bool> callerMask;
            if (callerIsVSArray) {
                fwdNameNode = argExpr.cast<NameNode>();
                if (!fwdNameNode || !fwdNameNode->info)
                    throw IEX_NAMESPACE::NoImplExc(
                        std::string("CTL Metal backend: call to '") +
                        function->name + "' forwards a VSArray through "
                        "a non-name expression; only direct parameter "
                        "forwarding is supported.");
                fwdMda = fwdNameNode->info->addr().cast<MetalDataAddr>();
                if (!fwdMda)
                    throw IEX_NAMESPACE::LogicExc(
                        "CTL Metal backend: forwarded VSArray name has "
                        "no MetalDataAddr.");
                callerMask = vsArrayVarMask(fwdNameNode->info->type().pointer());
            }

            const std::vector<bool> formalMask =
                vsArrayVarMask(params[i].type.pointer());

            std::vector<std::string> lenExprs;

            ArrayTypePtr curArr = actualArr;
            for (size_t d = 0; d < formalMask.size(); ++d) {
                if (!curArr)
                    throw IEX_NAMESPACE::LogicExc(
                        std::string("CTL Metal backend: call to '") +
                        function->name + "' passes a non-array expression "
                        "to variable-size array parameter '" +
                        params[i].name + "' at dim " + std::to_string(d));
                if (!formalMask[d]) {
                    curArr = curArr->elementType().cast<ArrayType>();
                    continue;
                }
                if (callerIsVSArray &&
                    d < callerMask.size() && callerMask[d]) {
                    lenExprs.push_back(
                        vsDimLenName(fwdMda->mslName(), callerMask,
                                     static_cast<int>(d)));
                } else if (curArr->size() > 0) {
                    lenExprs.push_back(std::to_string(curArr->size()) + "u");
                } else {
                    throw IEX_NAMESPACE::LogicExc(
                        std::string("CTL Metal backend: call to '") +
                        function->name + "' has no length available for "
                        "variable-size array parameter '" +
                        params[i].name + "' at dim " + std::to_string(d));
                }
                curArr = curArr->elementType().cast<ArrayType>();
            }

            argFragments.push_back(ptrExpr);
            argLenFragments.push_back(lenExprs);
            continue;
        }

        argFragments.push_back(frag);
        argLenFragments.push_back(std::vector<std::string>());
    }

    //
    // Stdlib functions (MetalStdLibFuncAddr) are plain value-returning
    // MSL helpers: the call is an expression, not a statement, and is
    // pushed onto the expression stack for the enclosing context to
    // consume. CTL stdlib functions never have out-parameters, so the
    // by-value MSL call is a complete translation of the CTL semantics.
    //
    if (stdlibAddr) {
        std::string call = stdlibAddr->mslName() + "(";
        bool firstStd = true;
        for (size_t i = 0; i < argFragments.size(); ++i) {
            if (!firstStd) call += ", ";
            firstStd = false;
            call += argFragments[i];
            for (const std::string &lenExpr : argLenFragments[i]) {
                call += ", ";
                call += lenExpr;
            }
        }
        //
        // `assert` takes a trailing `device atomic_uint* flag` argument
        // that is not part of the CTL-visible signature — we append it
        // here from the enclosing helper's `__ctl_err_flag` parameter.
        // The CTL parser resolves `assert` as `void(bool)` so the user-
        // visible call shape stays identical across CPU and GPU.
        //
        if (stdlibAddr->mslName() == "ctl_stdlib_assert") {
            cg.markAssertUsed();
            if (!firstStd) call += ", ";
            call += "__ctl_err_flag";
        }
        //
        // `scatteredDataToGrid3D` is a no-op stub on the GPU that signals
        // "kernel-reachable call is unsupported" by setting bit 1 of the
        // error flag. Append the flag the same way `assert` does so the
        // stub's trailing `device atomic_uint*` parameter is satisfied.
        // The host-side read in `MetalFunctionCall::callFunction`
        // translates bit 1 into a specific `Iex::NoImplExc` message.
        //
        if (stdlibAddr->mslName() == "ctl_stdlib_scatteredDataToGrid3D") {
            if (!firstStd) call += ", ";
            call += "__ctl_err_flag";
        }
        //
        // Half-precision exp/log helpers read from the module-scope
        // `constant` tables that `MetalCodegen::source()` only emits
        // when flagged. Mark the flag on first use so the preamble
        // includes the ~12 MB table block for this module.
        //
        const std::string &mname = stdlibAddr->mslName();
        if (mname == "ctl_stdlib_exp_h"   ||
            mname == "ctl_stdlib_log_h"   ||
            mname == "ctl_stdlib_log10_h" ||
            mname == "ctl_stdlib_pow10_h" ||
            mname == "ctl_stdlib_pow_h") {
            cg.markHalfExpLogUsed();
            //
            // The half-exp/log helpers don't read `__ctl_err_flag`, but
            // they do read the three hoisted tables. Forward them from
            // the enclosing helper's unconditional trailing parameters.
            //
            if (!firstStd) call += ", ";
            call +=
                "__ctl_half_log10_tbl, __ctl_half_log_tbl, __ctl_half_exp_tbl";
        }
        call += ")";
        cg.pushExpr(call);
        return;
    }

    //
    // For non-void returns the call is emitted as a statement that
    // populates a fresh temporary, then the temporary's name is pushed
    // on the expression stack so the enclosing context (assignment,
    // operand, nested call) can consume it. For void returns the call
    // is simply a statement and nothing is pushed; ExprStatementNode
    // detects the empty stack and skips its no-op appendix.
    //
    std::string call = addr->helperName() + "(";
    bool firstArg = true;

    std::string retTmp;
    if (!voidReturn) {
        retTmp = cg.nextTempName();
        cg.writeln(registerAndNameType(cg, retType.pointer()) +
                   " " + retTmp + ";");
        call += retTmp;
        firstArg = false;
    }

    for (size_t i = 0; i < argFragments.size(); ++i) {
        if (!firstArg)
            call += ", ";
        firstArg = false;
        call += argFragments[i];
        for (const std::string &lenExpr : argLenFragments[i]) {
            call += ", ";
            call += lenExpr;
        }
    }
    //
    // Every user helper takes the trailing `device atomic_uint*
    // __ctl_err_flag` argument; forward the enclosing caller's flag so
    // any transitive `assert` inside the callee writes back to the same
    // buffer the host inspects after dispatch.
    //
    if (!firstArg)
        call += ", ";
    call += "__ctl_err_flag";
    call +=
        ", __ctl_half_log10_tbl, __ctl_half_log_tbl, __ctl_half_exp_tbl";
    call += ");";
    cg.writeln(call);

    //
    // Writeback for output VSArray params whose caller-side was a
    // fixed-size local: reverse-copy each scalar from the C-array
    // tmp back into the caller's `metal::array<...>` storage, using
    // the same index nest as the pre-copy above.
    //
    for (const VSArrayWriteback &wb : writebacks) {
        std::string idx;
        for (size_t k = 0; k < wb.dims.size(); ++k) {
            std::string iv = wb.tmp + "_o" + std::to_string(k);
            cg.writeln("for (uint " + iv + " = 0; " + iv +
                       " < " + std::to_string(wb.dims[k]) +
                       "u; ++" + iv + ")");
            cg.writeln("{");
            cg.indent();
            idx += "[" + iv + "]";
        }
        cg.writeln("(" + wb.lvalue + ")" + idx + " = " +
                   wb.tmp + idx + ";");
        for (size_t k = 0; k < wb.dims.size(); ++k) {
            cg.outdent();
            cg.writeln("}");
        }
    }

    if (!voidReturn)
        cg.pushExpr(retTmp);
}

//--- ValueNode --------------------------------------------------------------

MetalValueNode::MetalValueNode(int lineNumber, const ExprNodeVector &elements)
    : ValueNode(lineNumber, elements)
{
}

//
// CTL flattens all nested aggregate literals into a single `elements`
// vector of *scalar* expressions — a `float[5][2]` initialized with
// `{{-2, 1}, {-0.5, 0.3}, ...}` arrives here as ten scalars with no
// nested structure. To emit a well-formed MSL initializer for a nested
// `metal::array<metal::array<float, 2>, 5>` we have to re-create the
// brace nesting ourselves: MSL's aggregate-init brace elision works
// for C arrays but NOT reliably through `metal::array`'s wrapper
// struct — a flat `{-2.0f, 1.0f, -0.5f, ...}` is accepted by the
// front-end but silently leaves some nested elements
// default-initialized on M4 Max, which produced NaN from the
// `interpolate1D` binary search (later table entries read as zero,
// making `xi1 - xi == 0` and the `(p - xi) / (xi1 - xi)` divide
// NaN). The fix is to recurse over the target type shape and emit
// properly nested braces per array/struct level.
//
static std::string
emitAggregateInit(LContext &lcontext, MetalCodegen &cg,
                  const DataTypePtr &targetType,
                  const ExprNodeVector &elements,
                  size_t &idx)
{
    ArrayTypePtr  at = targetType.cast<ArrayType>();
    StructTypePtr st = targetType.cast<StructType>();

    if (at) {
        //
        // Every array-typed declaration on the Metal backend lowers
        // to a `metal::array<T, N>` (a wrapper struct containing
        // `T __elems_[N]`), so the initializer takes TWO brace
        // layers: outer opens the struct, inner opens the
        // `__elems_` C-array. Nested array levels recurse and
        // contribute their own pair of braces.
        //
        std::string s = "{{";
        for (int i = 0; i < at->size(); ++i) {
            if (i) s += ", ";
            s += emitAggregateInit(lcontext, cg,
                                   at->elementType(),
                                   elements, idx);
        }
        s += "}}";
        return s;
    }
    if (st) {
        std::string s = "{";
        const MemberVector &members = st->members();
        for (size_t i = 0; i < members.size(); ++i) {
            if (i) s += ", ";
            s += emitAggregateInit(lcontext, cg,
                                   members[i].type,
                                   elements, idx);
        }
        s += "}";
        return s;
    }

    if (idx >= elements.size())
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: aggregate literal ran out of scalar "
            "elements before filling target type.");
    const ExprNodePtr &e = elements[idx++];
    e->generateCode(lcontext);
    targetType->generateCastFrom(e, lcontext);
    return cg.popExpr();
}

void
MetalValueNode::generateCode(LContext &lcontext)
{
    //
    // CTL aggregate literals (`{1, 2, 3}` or `{.r = 1, ...}`) are only
    // valid in variable-initializer position — never as a standalone
    // expression — so the enclosing MetalVariableNode or casted
    // assignment supplies the target type. We then recursively emit
    // a fully nested MSL braced initializer that matches the target
    // type's shape, casting each scalar to the appropriate member/
    // element type so integer→float promotion in literals like
    // `{1, 2, 3}` for a float array matches the CPU SIMD semantics
    // bit-for-bit.
    //
    MetalCodegen &cg = codegenOf(lcontext);

    if (!type.cast<ArrayType>() && !type.cast<StructType>())
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: ValueNode has neither array nor struct "
            "type — type checker should have rejected this.");

    size_t idx = 0;
    std::string s = emitAggregateInit(lcontext, cg, type, elements, idx);
    cg.pushExpr(s);
}

} // namespace Ctl
