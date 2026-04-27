///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_PIXEL_EVOLUTION_H
#define INCLUDED_CTL_PIXEL_EVOLUTION_H

#include <CtlSimdInspector.h>

#include <string>
#include <vector>

namespace Ctl {

// Build a one-line summary of every "color-shaped" local in `vars` —
// any float[3] / float[4] / half[3] / half[4] array, formatted as
//   "name=[r, g, b], other=[r, g, b, a], …"
// with the namespace prefix stripped from each name for compactness.
//
// Returns the empty string when no color-shaped variables are
// present, so callers can wrap with "→ paused " unconditionally
// and skip emission on empty.
//
// Used by:
//   - ctldap when launched with `pixelEvolution: true`: emits the
//     line on every pause via DAP `output` event.
//   - ctldb when launched with `--pixel-evolution`: emits the line
//     on every pause to stdout right before entering the REPL.
std::string formatColorEvolution (const std::vector<InspectableVar> &vars);

} // namespace Ctl

#endif
