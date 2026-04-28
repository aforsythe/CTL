///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_STAGE_CHAIN_H
#define INCLUDED_CTL_STAGE_CHAIN_H

#include <CtlFunctionCall.h>

#include <map>
#include <string>
#include <vector>

namespace Ctl {

// Path helpers for chained-stage debug drivers (ctldb, ctldap).  C++11 —
// std::filesystem isn't available everywhere we ship.

// Last directory component of `p`, or "." if no slash, or "/" if root.
std::string pathDirname (const std::string &p);

// Filename of `p` minus its last extension.  "foo/bar.ctl" -> "bar".
std::string pathStem (const std::string &p);

// Parse a colon-separated $CTL_MODULE_PATH-style env value into segments.
// Empty segments are dropped.  `env == nullptr` returns an empty vector.
std::vector<std::string> parseColonPath (const char *env);

// Capture float outputs of `fc` into a name -> value map for the next
// stage in a chain.  Non-float outputs are silently ignored (the chain
// only carries scalar floats today).
std::map<std::string, float> captureOutputs (FunctionCallPtr &fc);

// Bind inputs of `fc` from `prevOutputs` (the captureOutputs() result of
// the prior stage).  Matching rule:
//   1. Exact name match.
//   2. In-suffix synonym: input "rIn" matches output "rOut", etc.
// Inputs without a match fall back to setDefaultValue() if available, or
// stay zero-initialised.
void bindFromPriorStage (FunctionCallPtr &fc,
                         const std::map<std::string, float> &prevOutputs);

} // namespace Ctl

#endif
