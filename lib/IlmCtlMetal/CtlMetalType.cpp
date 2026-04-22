///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalType.h>
#include <CtlAlign.h>
#include <CtlMetalAddr.h>
#include <CtlMetalCodegen.h>
#include <CtlMetalLContext.h>
#include <CtlMetalModule.h>
#include <CtlSyntaxTree.h>
#include <Iex.h>
#include <half.h>

namespace {

[[noreturn]] void
notImplemented(const char *what)
{
    throw IEX_NAMESPACE::NoImplExc(
        std::string("CTL Metal backend: ") + what +
        " is not supported.");
}

} // anonymous namespace

namespace Ctl {

//
// CTL struct types carry their fully qualified name (e.g. `aggr::Pair`)
// via the symbol table. MSL does not support C++ namespace syntax in
// type names, so rewrite the `::` separator as `__` when producing the
// identifier that appears in emitted MSL. Keep every other character
// intact — CTL forbids non-identifier characters in names, so the
// result is always a valid MSL identifier.
//
std::string
mslStructName(const std::string &ctlName)
{
    std::string out;
    out.reserve(ctlName.size());
    for (size_t i = 0; i < ctlName.size(); ++i) {
        if (ctlName[i] == ':' && i + 1 < ctlName.size() && ctlName[i + 1] == ':') {
            out += "__";
            ++i;
        } else {
            out += ctlName[i];
        }
    }
    return out;
}

namespace {

MetalCodegen &
codegenOf(LContext &lcontext)
{
    return static_cast<MetalLContext &>(lcontext).metalModule()->codegen();
}

//
// Allocate a fresh `MetalStaticAddr` on the module for a module-scope
// static variable. All primitive `Metal*Type::newStaticVariable` bodies
// delegate here — the type-specific lowering happens later in
// MetalVariableNode::generateCode, which emits `constant T name = init;`
// into the codegen header once the initializer expression is known.
// Array and struct static variables stay unsupported until we need them
// (the CTL parser drives them through the same entry point, and we want
// a loud NoImplExc rather than silent half-support for those shapes).
//
AddrPtr
newMetalStatic(Module *module)
{
    MetalModule *mm = dynamic_cast<MetalModule *>(module);
    if (!mm)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: newStaticVariable called with a non-Metal "
            "Module.");
    return new MetalStaticAddr(mm->nextStaticName());
}

//
// If `expr`'s source type already matches `targetMslType` (i.e. they are
// the same Metal-backend concrete type), leave the expression stack
// unchanged. Otherwise wrap the top-of-stack expression in an MSL
// constructor-style cast `T(x)`. `sameConcreteType` is supplied by the
// caller because each Metal*Type knows its own CTL base class.
//
template <class BaseType>
void
castTopOfStackTo(LContext &lcontext,
                 const ExprNodePtr &expr,
                 const char *targetMslType)
{
    MetalCodegen &cg = codegenOf(lcontext);
    // If the source expression is already of the same CTL base type, a
    // no-op cast is correct — both will emit the same MSL spelling.
    if (expr && expr->type && expr->type.cast<BaseType>())
        return;

    const std::string inner = cg.popExpr();
    cg.pushExpr(std::string(targetMslType) + "(" + inner + ")");
}

} // anonymous namespace

//--- void ------------------------------------------------------------------

MetalVoidType::MetalVoidType() : VoidType() {}

size_t MetalVoidType::objectSize()        const { return 0; }
size_t MetalVoidType::alignedObjectSize() const { return 0; }
size_t MetalVoidType::objectAlignment()   const { return 1; }

void MetalVoidType::generateCastFrom(const ExprNodePtr &, LContext &) const { }
void MetalVoidType::generateCode    (const SyntaxNodePtr &, LContext &) const { }

//--- bool ------------------------------------------------------------------

MetalBoolType::MetalBoolType() : BoolType() {}

size_t MetalBoolType::objectSize()        const { return sizeof(bool); }
size_t MetalBoolType::alignedObjectSize() const { return sizeof(bool); }
size_t MetalBoolType::objectAlignment()   const { return sizeof(bool); }

void MetalBoolType::generateCastFrom(const ExprNodePtr &expr, LContext &lcontext) const
{
    castTopOfStackTo<BoolType>(lcontext, expr, "bool");
}
void MetalBoolType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalBoolType::generateCode"); }
AddrPtr MetalBoolType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalBoolType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalBoolType::newAutomaticVariable"); }

//--- int -------------------------------------------------------------------

MetalIntType::MetalIntType() : IntType() {}

size_t MetalIntType::objectSize()        const { return sizeof(int); }
size_t MetalIntType::alignedObjectSize() const { return sizeof(int); }
size_t MetalIntType::objectAlignment()   const { return sizeof(int); }

void MetalIntType::generateCastFrom(const ExprNodePtr &expr, LContext &lcontext) const
{
    castTopOfStackTo<IntType>(lcontext, expr, "int");
}
void MetalIntType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalIntType::generateCode"); }
AddrPtr MetalIntType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalIntType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalIntType::newAutomaticVariable"); }

//--- unsigned int ----------------------------------------------------------

MetalUIntType::MetalUIntType() : UIntType() {}

size_t MetalUIntType::objectSize()        const { return sizeof(unsigned); }
size_t MetalUIntType::alignedObjectSize() const { return sizeof(unsigned); }
size_t MetalUIntType::objectAlignment()   const { return sizeof(unsigned); }

void MetalUIntType::generateCastFrom(const ExprNodePtr &expr, LContext &lcontext) const
{
    castTopOfStackTo<UIntType>(lcontext, expr, "uint");
}
void MetalUIntType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalUIntType::generateCode"); }
AddrPtr MetalUIntType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalUIntType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalUIntType::newAutomaticVariable"); }

//--- half ------------------------------------------------------------------

MetalHalfType::MetalHalfType() : HalfType() {}

size_t MetalHalfType::objectSize()        const { return sizeof(half); }
size_t MetalHalfType::alignedObjectSize() const { return sizeof(half); }
size_t MetalHalfType::objectAlignment()   const { return sizeof(half); }

void MetalHalfType::generateCastFrom(const ExprNodePtr &expr, LContext &lcontext) const
{
    castTopOfStackTo<HalfType>(lcontext, expr, "half");
}
void MetalHalfType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalHalfType::generateCode"); }
AddrPtr MetalHalfType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalHalfType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalHalfType::newAutomaticVariable"); }

//--- float -----------------------------------------------------------------

MetalFloatType::MetalFloatType() : FloatType() {}

size_t MetalFloatType::objectSize()        const { return sizeof(float); }
size_t MetalFloatType::alignedObjectSize() const { return sizeof(float); }
size_t MetalFloatType::objectAlignment()   const { return sizeof(float); }

void MetalFloatType::generateCastFrom(const ExprNodePtr &expr, LContext &lcontext) const
{
    castTopOfStackTo<FloatType>(lcontext, expr, "float");
}
void MetalFloatType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalFloatType::generateCode"); }
AddrPtr MetalFloatType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalFloatType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalFloatType::newAutomaticVariable"); }

//--- string (throws; strings outside print/assert are out of scope)

MetalStringType::MetalStringType() : StringType() {}

size_t MetalStringType::objectSize()        const { return sizeof(char *); }
size_t MetalStringType::alignedObjectSize() const { return sizeof(char *); }
size_t MetalStringType::objectAlignment()   const { return sizeof(char *); }

void MetalStringType::generateCastFrom(const ExprNodePtr &, LContext &) const { notImplemented("MetalStringType::generateCastFrom"); }
void MetalStringType::generateCode    (const SyntaxNodePtr &, LContext &) const { notImplemented("MetalStringType::generateCode"); }
AddrPtr MetalStringType::newStaticVariable(Module *) const { notImplemented("MetalStringType::newStaticVariable"); }
void MetalStringType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalStringType::newAutomaticVariable"); }

//--- array -----------------------------------------------------------------

MetalArrayType::MetalArrayType(const DataTypePtr &elementType, int size_)
    : ArrayType(elementType, size_)
{
}

//
// Match SimdArrayType's layout contract, not StdArrayType's. Sidecar
// bytes are written through SimdLContext, so sub-array strides follow
// SimdArrayType: the aligned object size of a CTL array is the total
// packed size of its elements (elementSize() * size()), not one
// element's stride. For a nested array `float[3][3]` this makes the
// outer element's stride = 12 instead of 4 so `formatSidecarLiteral`
// walks the blob at the same cadence the sidecar packed it.
//
size_t MetalArrayType::objectSize()        const { return size() * elementSize(); }
size_t MetalArrayType::alignedObjectSize() const { return size() * elementSize(); }
size_t MetalArrayType::objectAlignment()   const { return elementType()->objectAlignment(); }

void MetalArrayType::generateCastFrom(const ExprNodePtr &, LContext &) const { }
void MetalArrayType::generateCode    (const SyntaxNodePtr &, LContext &) const { }
AddrPtr MetalArrayType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalArrayType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalArrayType::newAutomaticVariable"); }

//--- struct ----------------------------------------------------------------

MetalStructType::MetalStructType(const std::string &name,
                                 const MemberVector &members)
    : StructType(name, members),
      _objectSize(0),
      _alignedObjectSize(0),
      _objectAlignment(1)
{
    //
    // Lay the struct out using the same offset rules as SimdStructType so
    // harvested sidecar bytes (which were packed against SimdStructType's
    // offsets) can be decoded against this Metal-side type directly when
    // the persistent sidecar cache short-circuits the live sidecar load.
    // Without this pass member offsets stay at their default (zero) and
    // formatSidecarLiteral reads every member from byte 0 of the blob.
    //
    for (size_t i = 0; i < this->members().size(); ++i)
    {
        Member &m = member(i);
        m.offset = align(_objectSize, m.type->objectAlignment());
        _objectSize = m.offset + m.type->objectSize();
        _objectAlignment =
            leastCommonMultiple(_objectAlignment, m.type->objectAlignment());
    }
    _alignedObjectSize = align(_objectSize, _objectAlignment);
}

size_t MetalStructType::objectSize()        const { return _objectSize; }
size_t MetalStructType::alignedObjectSize() const { return _alignedObjectSize; }
size_t MetalStructType::objectAlignment()   const { return _objectAlignment; }

void MetalStructType::generateCastFrom(const ExprNodePtr &, LContext &) const { }
void MetalStructType::generateCode    (const SyntaxNodePtr &, LContext &) const { }
AddrPtr MetalStructType::newStaticVariable(Module *module) const { return newMetalStatic(module); }
void MetalStructType::newAutomaticVariable(StatementNodePtr, LContext &) const { notImplemented("MetalStructType::newAutomaticVariable"); }

//--- function --------------------------------------------------------------

MetalFunctionType::MetalFunctionType(const DataTypePtr &returnType,
                                     bool returnVarying,
                                     const ParamVector &parameters)
    : FunctionType(returnType, returnVarying, parameters)
{
}

void MetalFunctionType::generateCastFrom(const ExprNodePtr &, LContext &) const { }
void MetalFunctionType::generateCode    (const SyntaxNodePtr &, LContext &) const { }

//--- metalTypeName --------------------------------------------------------

std::string
metalTypeName(const DataType *type)
{
    if (!type)
        throw IEX_NAMESPACE::LogicExc(
            "CTL Metal backend: metalTypeName called on a null type.");

    if (auto p = dynamic_cast<const MetalVoidType *>(type))   return p->mslTypeName();
    if (auto p = dynamic_cast<const MetalBoolType *>(type))   return p->mslTypeName();
    if (auto p = dynamic_cast<const MetalIntType *>(type))    return p->mslTypeName();
    if (auto p = dynamic_cast<const MetalUIntType *>(type))   return p->mslTypeName();
    if (auto p = dynamic_cast<const MetalHalfType *>(type))   return p->mslTypeName();
    if (auto p = dynamic_cast<const MetalFloatType *>(type))  return p->mslTypeName();

    if (auto p = dynamic_cast<const ArrayType *>(type)) {
        //
        // VSArray parameters (ArrayType with size()==0) never reach this
        // path — they're lowered to `device T*` + length uniform by the
        // parameter-emission code in CtlMetalSyntaxTree.cpp, which calls
        // `metalVSArrayPointee` directly. A size==0 ArrayType arriving
        // here therefore means a non-parameter context: a local variable,
        // a return type, or a struct member. CTL itself forbids all three
        // (the language spec requires a compile-time size outside of a
        // function parameter), so this only happens in `@error`-harness
        // fixtures that intentionally author illegal code to exercise
        // parser diagnostics (testVSArrays: `int bLocalUnk[];`,
        // `int[] checkReturn(...)`, etc.). The errors are paired with
        // their `@error` markers at load time so the module is otherwise
        // valid; we emit a placeholder type so codegen for the enclosing
        // declaration finishes. The resulting MSL source is never
        // compiled because a user would call only the non-erroring
        // entry points, and `newFunctionCall` doesn't compile.
        //
        if (p->size() <= 0)
            return "__ctl_err_placeholder_vsarray";
        return "metal::array<" + metalTypeName(p->elementType().pointer()) +
               ", " + std::to_string(p->size()) + ">";
    }

    if (auto p = dynamic_cast<const StructType *>(type)) {
        //
        // An empty-name, zero-member StructType is the CTL parser's
        // error-recovery placeholder for `<unknown_type> var;` — see
        // CtlParser.cpp around line 1092. Any `@error`-harness fixture
        // that intentionally references an undefined type (testParse,
        // testScope, ...) leaves one of these in the AST. The errors
        // get paired with their `@error` markers at load time, so the
        // module is otherwise valid; we just need a syntactically
        // acceptable MSL spelling so codegen for the surrounding decl
        // doesn't blow up. A shared `__ctl_err_placeholder` struct
        // works: if no valid function reaches the declaration it is
        // simply unused MSL.
        //
        if (p->name().empty()) {
            if (p->members().size() == 0)
                return "__ctl_err_placeholder";
            throw IEX_NAMESPACE::NoImplExc(
                "CTL Metal backend: anonymous struct types are not "
                "supported.");
        }
        return mslStructName(p->name());
    }

    throw IEX_NAMESPACE::NoImplExc(
        "CTL Metal backend: metalTypeName: unsupported data type "
        "for current phase.");
}

} // namespace Ctl
