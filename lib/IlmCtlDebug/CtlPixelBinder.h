///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_PIXEL_BINDER_H
#define INCLUDED_CTL_PIXEL_BINDER_H

#include <CtlFunctionCall.h>

#include <map>
#include <string>
#include <vector>

namespace Ctl {

// Bind pixel inputs to a function call.  Supports two CTL signatures
// the same way both ctldb and ctldap do — kept here so the matching
// logic doesn't drift between them.
//
//   (a) Per-channel scalars — accepts the common naming variants
//       seen across ACES, OCIO, and ad-hoc CTL: rIn/gIn/bIn/aIn,
//       r/g/b/a (case-insensitive), red/green/blue/alpha, and the
//       i/I/in suffixed forms (rI, RI, redIn, …).  Match is
//       case-insensitive on the leaf name to spare the user from
//       guessing what the transform's author preferred.
//   (b) An array input named rgbIn / rgbaIn / rgb / rgba / RGB(A) /
//       pixel — typical of transforms that hand the pixel through as
//       a vector.  4-channel aggregates auto-fill alpha=1.0 when the
//       caller supplies only 3 channels.
//
// Args that aren't matched by either rule keep their CTL default value
// when one exists; otherwise zero-init.
void bindPixelInputs (FunctionCallPtr &fc,
                      const std::vector<float> &pixel);

// Bind launch-config / CLI uniform-float `params` (name → value) onto
// any matching input arg of the function call.  Doesn't override args
// that were already bound by bindPixelInputs.  Type mismatches are
// silently skipped (the param's intended target is missing or has a
// different type than expected).
void bindUniformParams (FunctionCallPtr &fc,
                        const std::map<std::string, float> &params);

} // namespace Ctl

#endif
