///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_L_CONTEXT_H
#define INCLUDED_CTL_METAL_L_CONTEXT_H

//-----------------------------------------------------------------------------
//
//  class MetalLContext -- Ctl::LContext subclass for the Metal backend.
//
//  Implements the syntax-tree-node and type factories; codegen for each
//  factory is in CtlMetalSyntaxTree.cpp / CtlMetalType.cpp.
//
//-----------------------------------------------------------------------------

#include <CtlLContext.h>

namespace Ctl {

class MetalModule;

class MetalLContext : public LContext
{
  public:

    MetalLContext(std::istream &file,
                  Module *module,
                  SymbolTable &symtab);

    virtual ~MetalLContext();

    //
    // The owning module, downcast for convenience. Every MetalLContext is
    // constructed against a MetalModule.
    //
    MetalModule *       metalModule();

    //-------------------------------------------------------------
    // Data-addressing factories.
    //
    // Each address is an MSL identifier string. parameters are named
    // `param0`, `param1`, …; autoVariables are named `var0`, `var1`, …;
    // returnValueAddr names are `ret0`, `ret1`, … (the CTL parser can
    // request more than one return value slot). newStackFrame() resets
    // all three counters.
    //-------------------------------------------------------------

    virtual void        newStackFrame();

    virtual AddrPtr     parameterAddr(const DataTypePtr &parameterType);
    virtual AddrPtr     returnValueAddr(const DataTypePtr &returnType);
    virtual AddrPtr     autoVariableAddr(const DataTypePtr &variableType);

    //-------------------------------------------------------------
    // Syntax-tree-node factories
    //-------------------------------------------------------------

    virtual ModuleNodePtr       newModuleNode
                                    (int lineNumber,
                                     const StatementNodePtr &constants,
                                     const FunctionNodePtr &functions) const;

    virtual FunctionNodePtr     newFunctionNode
                                    (int lineNumber,
                                     const std::string &name,
                                     const SymbolInfoPtr &info,
                                     const StatementNodePtr &body) const;

    virtual VariableNodePtr     newVariableNode
                                    (int lineNumber,
                                     const std::string &name,
                                     const SymbolInfoPtr &info,
                                     const ExprNodePtr &initialValue,
                                     bool assignInitialValue) const;

    virtual AssignmentNodePtr   newAssignmentNode
                                    (int lineNumber,
                                     const ExprNodePtr &lhs,
                                     const ExprNodePtr &rhs) const;

    virtual ExprStatementNodePtr newExprStatementNode
                                    (int lineNumber,
                                     const ExprNodePtr &expr) const;

    virtual IfNodePtr           newIfNode
                                    (int lineNumber,
                                     const ExprNodePtr &condition,
                                     const StatementNodePtr &truePath,
                                     const StatementNodePtr &falsePath) const;

    virtual ReturnNodePtr       newReturnNode
                                    (int lineNumber,
                                     const SymbolInfoPtr &info,
                                     const ExprNodePtr &returnedValue) const;

    virtual WhileNodePtr        newWhileNode
                                    (int lineNumber,
                                     const ExprNodePtr &condition,
                                     const StatementNodePtr &loopBody) const;

    virtual BinaryOpNodePtr     newBinaryOpNode
                                    (int lineNumber,
                                     Token op,
                                     const ExprNodePtr &leftOperand,
                                     const ExprNodePtr &rightOperand) const;

    virtual UnaryOpNodePtr      newUnaryOpNode
                                    (int lineNumber,
                                     Token op,
                                     const ExprNodePtr &operand) const;

    virtual ArrayIndexNodePtr   newArrayIndexNode
                                    (int lineNumber,
                                     const ExprNodePtr &array,
                                     const ExprNodePtr &index) const;

    virtual SizeNodePtr         newSizeNode
                                    (int lineNumber,
                                     const ExprNodePtr &obj) const;

    virtual MemberNodePtr       newMemberNode
                                    (int lineNumber,
                                     const ExprNodePtr &obj,
                                     const std::string &member) const;

    virtual NameNodePtr         newNameNode
                                    (int lineNumber,
                                     const std::string &name,
                                     const SymbolInfoPtr &info) const;

    virtual BoolLiteralNodePtr  newBoolLiteralNode
                                    (int lineNumber, bool value) const;

    virtual IntLiteralNodePtr   newIntLiteralNode
                                    (int lineNumber, int value) const;

    virtual UIntLiteralNodePtr  newUIntLiteralNode
                                    (int lineNumber, unsigned value) const;

    virtual HalfLiteralNodePtr  newHalfLiteralNode
                                    (int lineNumber, half value) const;

    virtual FloatLiteralNodePtr newFloatLiteralNode
                                    (int lineNumber, float value) const;

    virtual StringLiteralNodePtr newStringLiteralNode
                                    (int lineNumber,
                                     const std::string &value) const;

    virtual CallNodePtr         newCallNode
                                    (int lineNumber,
                                     const NameNodePtr &function,
                                     const ExprNodeVector &arguments) const;

    virtual ValueNodePtr        newValueNode
                                    (int lineNumber,
                                     const ExprNodeVector &elements) const;

    //-------------------------------------------------------------
    // Type factories
    //-------------------------------------------------------------

    virtual VoidTypePtr         newVoidType() const;
    virtual BoolTypePtr         newBoolType() const;
    virtual IntTypePtr          newIntType() const;
    virtual UIntTypePtr         newUIntType() const;
    virtual HalfTypePtr         newHalfType() const;
    virtual FloatTypePtr        newFloatType() const;
    virtual StringTypePtr       newStringType() const;

    virtual ArrayTypePtr        newArrayType
                                    (const DataTypePtr &baseType,
                                     int size,
                                     ArrayTypeUsage usage = NON_PARAMETER);

    virtual StructTypePtr       newStructType
                                    (const std::string &name,
                                     const MemberVector &members) const;

    virtual FunctionTypePtr     newFunctionType
                                    (const DataTypePtr &returnType,
                                     bool returnVarying,
                                     const ParamVector &parameters) const;

  private:

    int                 _nextParameterIndex;
    int                 _nextReturnIndex;
    int                 _nextAutoVarIndex;
};

} // namespace Ctl

#endif
