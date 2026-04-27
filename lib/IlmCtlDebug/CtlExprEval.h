///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_EXPR_EVAL_H
#define INCLUDED_CTL_EXPR_EVAL_H

#include <CtlSimdInspector.h>

#include <string>
#include <vector>

namespace Ctl {

struct EvalResult
{
    bool        ok    = false;
    double      value = 0.0;
    std::string error;       // populated when ok == false
};

// Evaluate a small expression in the scope of `vars` (an inspector
// snapshot of the current pause).
//
// Recognized grammar — intentionally narrow, sized to "things you'd
// reasonably type into a hover, watch panel, conditional breakpoint,
// or ctldb `print` command":
//
//   expr   := orExpr
//   orExpr := andExpr ('||' andExpr)*
//   andExpr:= compExpr ('&&' compExpr)*
//   compExpr:= addExpr (compOp addExpr)?       // ==, !=, <, <=, >, >=
//   addExpr:= mulExpr (('+'|'-') mulExpr)*
//   mulExpr:= unary (('*'|'/') unary)*
//   unary  := ('-' | '!')? primary
//   primary:= number | ident postfix* | '(' expr ')'
//   postfix:= '[' expr ']'                     // array index
//
// All values are computed as double.  Identifiers resolve against the
// `vars` snapshot — bare name picks up the scalar value, `name[i]`
// indexes the array stored at that name.
//
// On parse / lookup failure, returns ok=false with a human-readable
// error message; never throws.
EvalResult evalExpression (const std::string &expr,
                           const std::vector<InspectableVar> &vars);

} // namespace Ctl

#endif
