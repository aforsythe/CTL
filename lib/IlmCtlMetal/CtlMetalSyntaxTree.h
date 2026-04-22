///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_SYNTAX_TREE_H
#define INCLUDED_CTL_METAL_SYNTAX_TREE_H

//-----------------------------------------------------------------------------
//
//  Metal backend subclasses of the CTL syntax-tree node types.  Each
//  node's generateCode() emits MSL into the enclosing MetalModule's
//  codegen buffer.
//
//-----------------------------------------------------------------------------

#include <CtlSyntaxTree.h>

namespace Ctl {

struct MetalModuleNode : public ModuleNode
{
    MetalModuleNode(int lineNumber,
                    const StatementNodePtr &constants,
                    const FunctionNodePtr &functions);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalFunctionNode : public FunctionNode
{
    MetalFunctionNode(int lineNumber,
                      const std::string &name,
                      const SymbolInfoPtr &info,
                      const StatementNodePtr &body);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalVariableNode : public VariableNode
{
    MetalVariableNode(int lineNumber,
                      const std::string &name,
                      const std::string &absoluteName,
                      const SymbolInfoPtr &info,
                      const ExprNodePtr &initialValue,
                      bool assignInitialValue);

    virtual void        generateCode(LContext &lcontext);

    //
    // Absolute (namespace-qualified) name of the variable as stored in
    // the symbol table. Captured at parse time so that module-scope
    // consts whose RHS can't be lowered to an MSL `constant` initializer
    // can be looked up in the Metal backend's host-side SIMD sidecar
    // (which has already run init code for this module) and substituted
    // with a literal aggregate spelling the evaluated bytes.
    //
    std::string         absoluteName;
};

struct MetalAssignmentNode : public AssignmentNode
{
    MetalAssignmentNode(int lineNumber,
                        const ExprNodePtr &lhs,
                        const ExprNodePtr &rhs);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalExprStatementNode : public ExprStatementNode
{
    MetalExprStatementNode(int lineNumber, const ExprNodePtr &expr);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalIfNode : public IfNode
{
    MetalIfNode(int lineNumber,
                const ExprNodePtr &condition,
                const StatementNodePtr &truePath,
                const StatementNodePtr &falsePath);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalReturnNode : public ReturnNode
{
    MetalReturnNode(int lineNumber,
                    const SymbolInfoPtr &info,
                    const ExprNodePtr &returnedValue);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalWhileNode : public WhileNode
{
    MetalWhileNode(int lineNumber,
                   const ExprNodePtr &condition,
                   const StatementNodePtr &loopBody);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalBinaryOpNode : public BinaryOpNode
{
    MetalBinaryOpNode(int lineNumber,
                      Token op,
                      const ExprNodePtr &leftOperand,
                      const ExprNodePtr &rightOperand);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalUnaryOpNode : public UnaryOpNode
{
    MetalUnaryOpNode(int lineNumber,
                     Token op,
                     const ExprNodePtr &operand);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalArrayIndexNode : public ArrayIndexNode
{
    MetalArrayIndexNode(int lineNumber,
                        const ExprNodePtr &array,
                        const ExprNodePtr &index);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalMemberNode : public MemberNode
{
    MetalMemberNode(int lineNumber,
                    const ExprNodePtr &obj,
                    const std::string &member);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalSizeNode : public SizeNode
{
    MetalSizeNode(int lineNumber, const ExprNodePtr &obj);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalNameNode : public NameNode
{
    MetalNameNode(int lineNumber,
                  const std::string &name,
                  const SymbolInfoPtr &info);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalBoolLiteralNode : public BoolLiteralNode
{
    MetalBoolLiteralNode(int lineNumber,
                         const LContext &lcontext,
                         bool value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalIntLiteralNode : public IntLiteralNode
{
    MetalIntLiteralNode(int lineNumber,
                        const LContext &lcontext,
                        int value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalUIntLiteralNode : public UIntLiteralNode
{
    MetalUIntLiteralNode(int lineNumber,
                         const LContext &lcontext,
                         unsigned value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalHalfLiteralNode : public HalfLiteralNode
{
    MetalHalfLiteralNode(int lineNumber,
                         const LContext &lcontext,
                         half value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalFloatLiteralNode : public FloatLiteralNode
{
    MetalFloatLiteralNode(int lineNumber,
                          const LContext &lcontext,
                          float value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalStringLiteralNode : public StringLiteralNode
{
    MetalStringLiteralNode(int lineNumber,
                           const LContext &lcontext,
                           const std::string &value);

    virtual void        generateCode(LContext &lcontext);
    virtual char *      valuePtr();
};

struct MetalCallNode : public CallNode
{
    MetalCallNode(int lineNumber,
                  const NameNodePtr &function,
                  const ExprNodeVector &arguments);

    virtual void        generateCode(LContext &lcontext);
};

struct MetalValueNode : public ValueNode
{
    MetalValueNode(int lineNumber, const ExprNodeVector &elements);

    virtual void        generateCode(LContext &lcontext);
};

} // namespace Ctl

#endif
