///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_CODEGEN_H
#define INCLUDED_CTL_METAL_CODEGEN_H

//-----------------------------------------------------------------------------
//
//  class MetalCodegen -- incrementally builds a Metal Shading Language
//  (MSL) source string from a CTL syntax tree. Pure C++; no Metal
//  dependency — emission is unit-testable against golden strings.
//
//  The emitter owns two buffers:
//
//    - `header`   : `#include` lines, typedefs, struct declarations.
//                   Emitted before any function bodies.
//
//    - `body`     : user function definitions and the kernel entry
//                   point(s) that fan them out over a thread grid.
//
//  `source()` concatenates header + body with the MSL preamble prepended.
//
//  The MSL preamble is `#include <metal_stdlib>` followed by
//  `using namespace metal;` and the CTL stdlib helper port.
//
//-----------------------------------------------------------------------------

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Ctl {

class StructType;

class MetalCodegen
{
  public:

    MetalCodegen();
    ~MetalCodegen();

    //------------------------------------------------------------
    // Emit tokens into the currently active section.
    //
    // `write(s)`   appends literal text.
    // `writeln(s)` appends `s` then `\n`.
    // `indent()` / `outdent()` change the indentation level applied
    //   by the next `writeln` that begins a new line.
    //------------------------------------------------------------

    void        write(const std::string &text);
    void        writeln(const std::string &text);
    void        indent();
    void        outdent();

    //------------------------------------------------------------
    // Switch between the header and body sections.
    //------------------------------------------------------------

    enum Section { Header, Body };

    void        setSection(Section s);
    Section     section() const { return _section; }

    //------------------------------------------------------------
    // Kernel scaffolding.
    //
    // `beginKernel(name)` writes the kernel entry signature and
    //   opens the body. The emitted kernel has a single parameter
    //   `uint tid [[thread_position_in_grid]]` plus any buffer
    //   bindings added via `declareKernelBuffer`.
    // `declareKernelBuffer` appends one `device T* name [[buffer(N)]]`
    //   parameter to the in-progress kernel signature.
    // `endKernel()` closes the body with `}`.
    //------------------------------------------------------------

    //
    // `beginKernel` opens a fresh per-kernel buffer and redirects
    // subsequent `write` / `writeln` / indent into it. All output from
    // `beginKernel` through the matching `finishKernel` lands in that
    // buffer instead of the shared body. `sourceForKernel(name)` emits
    // header + helpers + just that kernel's buffer, so compiling a
    // single-kernel MSL source does not pay the type-check cost of the
    // other module-scope kernels (see `PRECISION.md`'s note on
    // cold-compile time for ACES-scale modules).
    //
    void        beginKernel(const std::string &name);
    void        declareKernelBuffer(const std::string &mslType,
                                    const std::string &name);
    void        endKernel();
    void        finishKernel();

    //------------------------------------------------------------
    // Expression stack.
    //
    // generateCode walks the syntax tree post-order. Leaf expression
    // nodes (literals, names) push a formatted MSL fragment; parent
    // expression nodes (binary ops, etc.) pop their operands and push
    // the combined fragment. Statement nodes pop their top-level
    // expression(s) and emit MSL statements into the body buffer.
    //------------------------------------------------------------

    void                pushExpr(const std::string &expr);
    std::string         popExpr();
    size_t              exprStackSize() const { return _exprStack.size(); }

    //------------------------------------------------------------
    // Temporary-name generator. Returns `__ctl_tmp_N` with a
    // monotonically increasing N so every synthesized scratch
    // variable (call return slot, call-by-reference cast holder,
    // etc.) gets a unique MSL identifier within the module.
    //------------------------------------------------------------

    std::string         nextTempName();

    //------------------------------------------------------------
    // Module-scope static-variable name generator. Returns
    // `staticN` with a monotonically increasing N so every
    // module-scope `constant T name = ...;` global gets a unique
    // MSL identifier across every module loaded into the
    // interpreter that owns this codegen.
    //------------------------------------------------------------

    std::string         nextStaticName();

    //------------------------------------------------------------
    // Struct declaration registry.
    //
    // Call `ensureStructDeclared` before referencing a struct type
    // name in emitted MSL. The first call for a given struct name
    // appends `struct Name { ... };` to the header section (switching
    // sections transparently). Subsequent calls for the same name are
    // cheap no-ops. Nested struct references are declared recursively,
    // so a struct whose members include another struct always declares
    // the inner one first.
    //------------------------------------------------------------

    void                ensureStructDeclared(const StructType *type);

    //------------------------------------------------------------
    // Error-recovery VSArray placeholder.
    //
    // When the CTL parser leaves a VSArray type (ArrayType with
    // size==0) on a local variable, a function return, or a struct
    // member — all of which CTL itself forbids — `metalTypeName`
    // returns `__ctl_err_placeholder_vsarray`. This method emits a
    // `typedef int __ctl_err_placeholder_vsarray;` declaration once
    // per module so referring decls are syntactically valid MSL.
    // Code that reaches one of these placeholders is unreachable at
    // runtime because the only entry points a caller can probe via
    // `newFunctionCall` are the non-erroring functions; the fixture
    // tests never drive the module into `callFunction` so the
    // placeholder MSL is never compiled.
    //------------------------------------------------------------

    void                ensureVSArrayPlaceholderDeclared();

    //------------------------------------------------------------
    // Function emission stack.
    //
    // `MetalFunctionNode::generateCode` pushes the function's CTL
    // name before walking its body and pops on exit. `MetalCallNode`
    // checks this stack to reject direct self-recursion with a clean
    // NoImplExc — MSL does not support recursion, and without this
    // check the caller would hit a more confusing "no MetalFunctionAddr"
    // LogicExc because a function's addr is only bound after its body
    // finishes emitting. Indirect recursion is already blocked by
    // CTL's definition-before-use rule.
    //------------------------------------------------------------

    void                pushEmittingFunction(const std::string &name);
    void                popEmittingFunction();
    bool                isEmittingFunction(const std::string &name) const;

    //------------------------------------------------------------
    // Print / assert tracking.
    //
    // `markPrintUsed()` is called by `MetalCallNode::generateCode`
    // the first time a `print_*` call is lowered in this module. It
    // returns true on that first call, so the caller can emit a one-
    // time stderr warning explaining that GPU print is a no-op.
    // Subsequent calls return false.
    //
    // `markAssertUsed()` is called the first time an `assert` call is
    // lowered; the flag is read by `MetalFunctionCall::callFunction`
    // to decide whether the post-dispatch atomic error-flag inspection
    // is meaningful to report.
    //------------------------------------------------------------

    bool                markPrintUsed();
    bool                assertUsed() const { return _assertUsed; }
    void                markAssertUsed() { _assertUsed = true; }

    //------------------------------------------------------------
    // Half-precision exp/log table tracking.
    //
    // The `exp_h` / `log_h` / `log10_h` / `pow10_h` stdlib helpers
    // are 0-ULP ports of the CPU SIMD backend's table-lookup
    // implementation in `halfExpLog.h`. They reference three
    // precomputed tables (`log10Table`, `logTable`, `expTable`)
    // which are ~739 KB of constant data combined. Emitting them
    // unconditionally would bloat every module; instead we track
    // whether any CTL program in this module actually calls one
    // of the four helpers, and emit the tables + helpers into the
    // MSL source only in that case.
    //
    // `markHalfExpLogUsed()` is called by `MetalCallNode::generateCode`
    // the first time it lowers a call to one of those four stdlib names.
    //------------------------------------------------------------

    bool                halfExpLogUsed() const { return _halfExpLogUsed; }
    void                markHalfExpLogUsed() { _halfExpLogUsed = true; }

    //------------------------------------------------------------
    // Retrieve the accumulated MSL source.
    //
    // `source()` concatenates preamble + header + body + every kernel
    // wrapper. Used for the debug MSL dump and any caller that wants
    // to see the full module.
    //
    // `sourceForKernel(kernelName)` concatenates preamble + header +
    // body + only the one kernel wrapper named `kernelName`. If the
    // name is unknown, falls back to `source()` so behavior stays
    // defined for callers that emit raw kernels outside the
    // MetalFunctionNode path (e.g. tests).
    //------------------------------------------------------------

    std::string source() const;
    std::string sourceForKernel(const std::string &kernelName) const;

    //------------------------------------------------------------
    // Access raw section buffers (primarily for tests).
    //------------------------------------------------------------

    const std::string & headerBuffer() const { return _header; }
    const std::string & bodyBuffer() const   { return _body; }
    const std::map<std::string, std::string> &
                        kernelBuffers() const { return _kernelWrappers; }

  private:

    std::string &       active();
    void                applyIndent();

    std::string         _header;
    std::string         _body;
    Section             _section;
    int                 _indent;
    bool                _atLineStart;

    // Kernel-in-progress state.
    bool                _inKernelSignature;
    int                 _kernelBufferIndex;

    // Per-kernel wrapper buffers. Populated while a `beginKernel` /
    // `finishKernel` span is active; `_kernelActive` points into
    // `_kernelWrappers` during that span (or is null otherwise).
    std::map<std::string, std::string>  _kernelWrappers;
    std::string *                       _kernelActive;

    std::vector<std::string>    _exprStack;

    size_t              _tempCounter;
    size_t              _staticCounter;

    std::set<std::string>       _declaredStructs;

    std::vector<std::string>    _functionStack;

    bool                        _printUsed;
    bool                        _assertUsed;
    bool                        _halfExpLogUsed;
};

} // namespace Ctl

#endif
