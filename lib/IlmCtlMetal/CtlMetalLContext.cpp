///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include <CtlMetalLContext.h>
#include <CtlMetalAddr.h>
#include <CtlMetalModule.h>
#include <CtlMetalSyntaxTree.h>
#include <CtlMetalType.h>
#include <CtlSymbolTable.h>

#include <sstream>

namespace {

std::string
mslIdent(const char *prefix, int index)
{
    std::ostringstream s;
    s << prefix << index;
    return s.str();
}

} // anonymous namespace

namespace Ctl {

MetalLContext::MetalLContext(std::istream &file,
                             Module *module,
                             SymbolTable &symtab)
    : LContext(file, module, symtab),
      _nextParameterIndex(0),
      _nextReturnIndex(0),
      _nextAutoVarIndex(0)
{
}

MetalLContext::~MetalLContext()
{
}

MetalModule *
MetalLContext::metalModule()
{
    return static_cast<MetalModule *>(module());
}

void
MetalLContext::newStackFrame()
{
    _nextParameterIndex = 0;
    _nextReturnIndex = 0;
    _nextAutoVarIndex = 0;
}

AddrPtr
MetalLContext::parameterAddr(const DataTypePtr &)
{
    return new MetalDataAddr(mslIdent("param", _nextParameterIndex++));
}

AddrPtr
MetalLContext::returnValueAddr(const DataTypePtr &)
{
    return new MetalDataAddr(mslIdent("ret", _nextReturnIndex++));
}

AddrPtr
MetalLContext::autoVariableAddr(const DataTypePtr &)
{
    return new MetalDataAddr(mslIdent("var", _nextAutoVarIndex++));
}

ModuleNodePtr
MetalLContext::newModuleNode(int lineNumber,
                             const StatementNodePtr &constants,
                             const FunctionNodePtr &functions) const
{
    return new MetalModuleNode(lineNumber, constants, functions);
}

FunctionNodePtr
MetalLContext::newFunctionNode(int lineNumber,
                               const std::string &name,
                               const SymbolInfoPtr &info,
                               const StatementNodePtr &body) const
{
    return new MetalFunctionNode(lineNumber, name, info, body);
}

VariableNodePtr
MetalLContext::newVariableNode(int lineNumber,
                               const std::string &name,
                               const SymbolInfoPtr &info,
                               const ExprNodePtr &initialValue,
                               bool assignInitialValue) const
{
    //
    // Capture the absolute (namespace-qualified) symbol name alongside
    // the relative name. The Metal backend uses it to look up module-
    // scope consts in the host-side SIMD sidecar when their RHS can't
    // be lowered to an MSL `constant` initializer (e.g. a call to a
    // user-defined CTL function, which MSL rejects in a global const
    // initializer).
    //
    const std::string absoluteName = symtab().getAbsoluteName(name);
    return new MetalVariableNode(lineNumber, name, absoluteName, info,
                                 initialValue, assignInitialValue);
}

AssignmentNodePtr
MetalLContext::newAssignmentNode(int lineNumber,
                                 const ExprNodePtr &lhs,
                                 const ExprNodePtr &rhs) const
{
    return new MetalAssignmentNode(lineNumber, lhs, rhs);
}

ExprStatementNodePtr
MetalLContext::newExprStatementNode(int lineNumber,
                                    const ExprNodePtr &expr) const
{
    return new MetalExprStatementNode(lineNumber, expr);
}

IfNodePtr
MetalLContext::newIfNode(int lineNumber,
                         const ExprNodePtr &condition,
                         const StatementNodePtr &truePath,
                         const StatementNodePtr &falsePath) const
{
    return new MetalIfNode(lineNumber, condition, truePath, falsePath);
}

ReturnNodePtr
MetalLContext::newReturnNode(int lineNumber,
                             const SymbolInfoPtr &info,
                             const ExprNodePtr &returnedValue) const
{
    return new MetalReturnNode(lineNumber, info, returnedValue);
}

WhileNodePtr
MetalLContext::newWhileNode(int lineNumber,
                            const ExprNodePtr &condition,
                            const StatementNodePtr &loopBody) const
{
    return new MetalWhileNode(lineNumber, condition, loopBody);
}

BinaryOpNodePtr
MetalLContext::newBinaryOpNode(int lineNumber, Token op,
                               const ExprNodePtr &leftOperand,
                               const ExprNodePtr &rightOperand) const
{
    return new MetalBinaryOpNode(lineNumber, op, leftOperand, rightOperand);
}

UnaryOpNodePtr
MetalLContext::newUnaryOpNode(int lineNumber, Token op,
                              const ExprNodePtr &operand) const
{
    return new MetalUnaryOpNode(lineNumber, op, operand);
}

ArrayIndexNodePtr
MetalLContext::newArrayIndexNode(int lineNumber,
                                 const ExprNodePtr &array,
                                 const ExprNodePtr &index) const
{
    return new MetalArrayIndexNode(lineNumber, array, index);
}

SizeNodePtr
MetalLContext::newSizeNode(int lineNumber, const ExprNodePtr &obj) const
{
    return new MetalSizeNode(lineNumber, obj);
}

MemberNodePtr
MetalLContext::newMemberNode(int lineNumber,
                             const ExprNodePtr &obj,
                             const std::string &member) const
{
    return new MetalMemberNode(lineNumber, obj, member);
}

NameNodePtr
MetalLContext::newNameNode(int lineNumber,
                           const std::string &name,
                           const SymbolInfoPtr &info) const
{
    return new MetalNameNode(lineNumber, name, info);
}

BoolLiteralNodePtr
MetalLContext::newBoolLiteralNode(int lineNumber, bool value) const
{
    return new MetalBoolLiteralNode(lineNumber, *this, value);
}

IntLiteralNodePtr
MetalLContext::newIntLiteralNode(int lineNumber, int value) const
{
    return new MetalIntLiteralNode(lineNumber, *this, value);
}

UIntLiteralNodePtr
MetalLContext::newUIntLiteralNode(int lineNumber, unsigned value) const
{
    return new MetalUIntLiteralNode(lineNumber, *this, value);
}

HalfLiteralNodePtr
MetalLContext::newHalfLiteralNode(int lineNumber, half value) const
{
    return new MetalHalfLiteralNode(lineNumber, *this, value);
}

FloatLiteralNodePtr
MetalLContext::newFloatLiteralNode(int lineNumber, float value) const
{
    return new MetalFloatLiteralNode(lineNumber, *this, value);
}

StringLiteralNodePtr
MetalLContext::newStringLiteralNode(int lineNumber,
                                    const std::string &value) const
{
    return new MetalStringLiteralNode(lineNumber, *this, value);
}

CallNodePtr
MetalLContext::newCallNode(int lineNumber,
                           const NameNodePtr &function,
                           const ExprNodeVector &arguments) const
{
    return new MetalCallNode(lineNumber, function, arguments);
}

ValueNodePtr
MetalLContext::newValueNode(int lineNumber,
                            const ExprNodeVector &elements) const
{
    return new MetalValueNode(lineNumber, elements);
}

VoidTypePtr
MetalLContext::newVoidType() const
{
    return VoidTypePtr(new MetalVoidType());
}

BoolTypePtr
MetalLContext::newBoolType() const
{
    return BoolTypePtr(new MetalBoolType());
}

IntTypePtr
MetalLContext::newIntType() const
{
    return IntTypePtr(new MetalIntType());
}

UIntTypePtr
MetalLContext::newUIntType() const
{
    return UIntTypePtr(new MetalUIntType());
}

HalfTypePtr
MetalLContext::newHalfType() const
{
    return HalfTypePtr(new MetalHalfType());
}

FloatTypePtr
MetalLContext::newFloatType() const
{
    return FloatTypePtr(new MetalFloatType());
}

StringTypePtr
MetalLContext::newStringType() const
{
    return StringTypePtr(new MetalStringType());
}

ArrayTypePtr
MetalLContext::newArrayType(const DataTypePtr &baseType, int size,
                            ArrayTypeUsage)
{
    return ArrayTypePtr(new MetalArrayType(baseType, size));
}

StructTypePtr
MetalLContext::newStructType(const std::string &name,
                             const MemberVector &members) const
{
    return StructTypePtr(new MetalStructType(name, members));
}

FunctionTypePtr
MetalLContext::newFunctionType(const DataTypePtr &returnType,
                               bool returnVarying,
                               const ParamVector &parameters) const
{
    return FunctionTypePtr(
        new MetalFunctionType(returnType, returnVarying, parameters));
}

} // namespace Ctl
