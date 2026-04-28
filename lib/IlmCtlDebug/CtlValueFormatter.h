///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_VALUE_FORMATTER_H
#define INCLUDED_CTL_VALUE_FORMATTER_H

#include <CtlType.h>

#include <string>

namespace Ctl {

// Render a CTL value at `data` of declared type `type` to a string.
// Recurses into struct members and array elements.  `indent` is the
// current indentation depth for pretty-printing nested aggregates;
// pass 0 at the top level.
std::string formatValue (const DataTypePtr &type,
                         const void *data,
                         int indent = 0);

} // namespace Ctl

#endif
