///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_SIMD_INSPECTOR_H
#define INCLUDED_CTL_SIMD_INSPECTOR_H

#include <CtlType.h>

#include <string>
#include <vector>

namespace Ctl {

class SimdXContext;
class SimdInterpreter;

//
// One inspectable variable: name + declared type + raw lane-0 pointer.
// The caller owns the pointer's lifetime via the xcontext snapshot —
// must not retain it across an interpreter resume().
//
struct InspectableVar
{
    std::string  name;
    DataTypePtr  type;
    const void  *data;        // pointer to lane-0 storage
};

//
// Walks the symbol table AND per-module local symbol snapshots for the
// function currently executing on `xcontext`.  Returns:
//   - every global/module-scope data symbol from the interpreter's symbol table
//   - every function-local variable that was captured by captureLocalSymbols()
//     BEFORE deleteAllLocalSymbols() in Interpreter::_loadModule AND whose
//     fp-relative stack address is valid in the current execution frame
//
// CTL names locals with anonymous namespace tokens (e.g. "N0", "N1") rather
// than function names, so filtering by function name prefix is not reliable.
// Variables from inactive stack frames are silently skipped because their
// fp-relative access throws and resolveSimdAddr returns null.
//
// `interp`          — the interpreter that owns the modules and symbol table.
// `currentFunction` — accepted for API compatibility; reserved for future
//                     function-scoped filtering when CTL stores function-name
//                     metadata in symbol info.
// `currentFile`/`currentLine` — accepted for API compatibility; line-scope
//                     filtering is not yet implemented.
//
// Each returned `InspectableVar::data` points at lane 0 of the symbol's
// SimdReg and is only valid while the interpreter is paused (i.e. while
// inside a beforeInst() callback).
//
std::vector<InspectableVar>
inspectVariables (const SimdInterpreter &interp,
                  SimdXContext &xcontext,
                  const std::string &currentFile,
                  int currentLine,
                  const std::string &currentFunction = "");

} // namespace Ctl

#endif
