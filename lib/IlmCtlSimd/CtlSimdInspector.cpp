///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlSimdInspector.h"

#include "CtlSimdAddr.h"
#include "CtlSimdInterpreter.h"
#include "CtlSimdReg.h"
#include "CtlSimdXContext.h"

#include <CtlModule.h>
#include <CtlModuleSet.h>
#include <CtlSymbolTable.h>

#include <stdexcept>

namespace Ctl {

namespace {

// Helper: try to resolve a SymbolInfoPtr's address through SimdDataAddr and
// return a pointer to lane-0 storage.  Returns null if:
//   - the address is not a SimdDataAddr
//   - the reg dereference fails (e.g. fp-relative offset out of range for
//     locals that belong to a different stack frame than the current one)
const void *
resolveSimdAddr (const SymbolInfoPtr &info, SimdXContext &xcontext)
{
    if (!info->isData()) return nullptr;
    if (!info->addr())   return nullptr;

    SimdDataAddrPtr saddr = info->addr().cast<SimdDataAddr>();
    if (!saddr) return nullptr;

    try
    {
        SimdReg &reg = saddr->reg (xcontext);
        return reg[0];
    }
    catch (...)
    {
        // fp-relative access out of range — variable belongs to a different
        // (inactive) stack frame; skip it.
        return nullptr;
    }
}

} // anonymous namespace

std::vector<InspectableVar>
inspectVariables (const SimdInterpreter &interp,
                  SimdXContext &xcontext,
                  const std::string &currentFile,
                  int currentLine,
                  const std::string &currentFunction)
{
    (void) currentFile;

    std::vector<InspectableVar> vars;

    // 1. Walk every symbol in the interpreter's global symbol table.
    //    After loadModule the table only contains module-scope (non-local)
    //    symbols, so these are globals, constants, and function signatures.
    const SymbolTable &symbols = interp.symbolTable();
    for (SymbolTable::SymbolMap::const_iterator it = symbols.begin();
         it != symbols.end(); ++it)
    {
        const SymbolInfoPtr &info = it->second;
        if (!info) continue;

        const void *data = resolveSimdAddr (info, xcontext);
        if (!data) continue;

        InspectableVar v;
        v.name = it->first;
        v.type = info->dataType();
        v.data = data;
        vars.push_back (v);
    }

    // 2. Walk per-module local symbol snapshots.
    //    captureLocalSymbols() was called just before deleteAllLocalSymbols()
    //    in Interpreter::_loadModule, so each Module object holds a copy of
    //    all function-local symbols that were otherwise erased from the table.
    //
    //    CTL names locals with anonymous namespace tokens (N0, N1, …), not
    //    with the function name, so we cannot reliably filter by function name
    //    prefix.  Instead we return all captured locals and rely on the
    //    fp-relative bounds check in resolveSimdAddr to naturally exclude
    //    variables that belong to inactive stack frames — those accesses throw
    //    and resolveSimdAddr returns null.
    //
    //    `currentFunction` is accepted for API compatibility and may be used
    //    for additional filtering in future when CTL stores function-name
    //    metadata in symbol info.
    {
        const ModuleSet &mset = interp.moduleSet();
        for (ModuleSet::const_iterator mit = mset.begin();
             mit != mset.end(); ++mit)
        {
            const Module *mod = mit->second;
            if (!mod) continue;

            for (const auto &entry : mod->localSymbols())
            {
                const std::string   &absName = entry.first;
                const SymbolInfoPtr &info    = entry.second;
                if (!info) continue;

                // Filter to symbols owned by the active function.  The
                // fp-relative bounds check below isn't strict enough on
                // its own — neighbour-frame addresses inside the live
                // stack range pass it and return stale data.
                //
                // Match either the exact name OR the suffix form: the
                // call stack records the bare AST name from the call
                // site (e.g. `inner` for an unqualified intra-namespace
                // call), but `_owningFunction` is always the fully
                // qualified `namespace::name` set by the parser.  Accept
                // any "::<currentFunction>" suffix so a 3-deep chain in
                // a single namespace correctly resolves each frame.
                if (!currentFunction.empty() &&
                    !info->owningFunction().empty())
                {
                    const std::string &owning = info->owningFunction();
                    bool matches = (owning == currentFunction);
                    if (!matches)
                    {
                        std::string suffix = "::" + currentFunction;
                        if (owning.size() > suffix.size() &&
                            owning.compare (owning.size() - suffix.size(),
                                            suffix.size(), suffix) == 0)
                            matches = true;
                    }
                    if (!matches) continue;
                }

                // Hide locals declared at or after the current pause line.
                // CTL allocates all function-locals at function entry, so
                // without this they'd appear as zero-valued ghosts before
                // (and on) their declaration line — and the user expects
                // a variable to "come into existence" only AFTER the line
                // that declares it has finished executing.
                //
                // EXCEPTION: function parameters are exempt.  They're
                // inputs and should be visible from function entry,
                // even when paused on the function-signature line
                // itself (where step-in lands).
                int declLine = info->declarationLine();
                if (!info->isParameter() &&
                    declLine > 0 && currentLine > 0 && declLine >= currentLine)
                    continue;

                // resolveSimdAddr catches out-of-range fp-relative accesses
                // and returns null for locals in inactive frames.
                const void *data = resolveSimdAddr (info, xcontext);
                if (!data) continue;

                InspectableVar v;
                v.name = absName;
                v.type = info->dataType();
                v.data = data;
                vars.push_back (v);
            }
        }
    }

    return vars;
}

} // namespace Ctl
