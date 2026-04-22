///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_TYPE_H
#define INCLUDED_CTL_METAL_TYPE_H

//-----------------------------------------------------------------------------
//
//  Metal backend subclasses of the CTL primitive DataTypes. Each adds an
//  `mslTypeName()` identifying how the type is spelled in generated Metal
//  Shading Language source. Object sizes/alignments match the CPU SIMD
//  backend so that CPU-side FunctionArg buffer packing can be shared.
//
//  Sizes and alignments match the CPU SIMD backend so CPU-side
//  FunctionArg buffer packing can be shared.  mslTypeName() is the
//  authoritative MSL spelling for code emission.
//
//-----------------------------------------------------------------------------

#include <CtlType.h>

#include <string>

namespace Ctl {

class MetalVoidType : public VoidType
{
  public:
    MetalVoidType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    const char *        mslTypeName() const { return "void"; }
};

class MetalBoolType : public BoolType
{
  public:
    MetalBoolType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

    const char *        mslTypeName() const { return "bool"; }
};

class MetalIntType : public IntType
{
  public:
    MetalIntType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

    const char *        mslTypeName() const { return "int"; }
};

class MetalUIntType : public UIntType
{
  public:
    MetalUIntType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

    const char *        mslTypeName() const { return "uint"; }
};

class MetalHalfType : public HalfType
{
  public:
    MetalHalfType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

    const char *        mslTypeName() const { return "half"; }
};

class MetalFloatType : public FloatType
{
  public:
    MetalFloatType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

    const char *        mslTypeName() const { return "float"; }
};

class MetalStringType : public StringType
{
  public:
    MetalStringType();

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;
};

class MetalArrayType : public ArrayType
{
  public:
    MetalArrayType(const DataTypePtr &elementType, int size);

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;
};

class MetalStructType : public StructType
{
  public:
    MetalStructType(const std::string &name, const MemberVector &members);

    virtual size_t      objectSize() const;
    virtual size_t      alignedObjectSize() const;
    virtual size_t      objectAlignment() const;

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;

    AddrPtr             newStaticVariable(Module *module) const;
    void                newAutomaticVariable(StatementNodePtr node,
                                             LContext &lcontext) const;

  private:
    size_t              _objectSize;
    size_t              _alignedObjectSize;
    size_t              _objectAlignment;
};

class MetalFunctionType : public FunctionType
{
  public:
    MetalFunctionType(const DataTypePtr &returnType,
                      bool returnVarying,
                      const ParamVector &parameters);

    virtual void        generateCastFrom(const ExprNodePtr &expr,
                                         LContext &lcontext) const;
    virtual void        generateCode(const SyntaxNodePtr &node,
                                     LContext &lcontext) const;
};

//-----------------------------------------------------------------------------
// metalTypeName -- look up the MSL spelling of a CTL DataType.
//
// Dispatches on the concrete Metal*Type subclass. Returns the string
// that should appear wherever the type is named in emitted MSL source
// (parameter declarations, local variable declarations, casts). Throws
// Iex::NoImplExc for data types the Metal backend does not yet support.
//
// Fixed-size CTL arrays map to nested `metal::array<T, N>`, which is a
// POD wrapper that round-trips through MSL `thread` / `device` storage
// and supports value-semantics assignment and `operator[]`. Struct types
// map to their declared name; the caller must have already arranged for
// the struct declaration to be emitted (see MetalCodegen::ensureStructDeclared).
//-----------------------------------------------------------------------------

std::string     metalTypeName(const DataType *type);

//-----------------------------------------------------------------------------
// mslStructName -- rewrite a CTL struct's fully-qualified name into a form
// that is a valid MSL identifier. MSL rejects C++ namespace syntax, so the
// `::` separator becomes `__`. Every other character passes through
// unchanged.
//-----------------------------------------------------------------------------

std::string     mslStructName(const std::string &ctlName);

} // namespace Ctl

#endif
