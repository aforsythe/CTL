///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDB_VALUE_FORMATTER_H
#define CTLDB_VALUE_FORMATTER_H

#include <CtlType.h>

#include <string>

namespace ctldb {

// Render a CTL value at `data` of declared type `type` to a string.
// Recurses into struct members and array elements.
//
// `indent` is current indentation depth for pretty-printing nested
// aggregates; pass 0 at the top level.
//
std::string formatValue (const Ctl::DataTypePtr &type,
                         const void *data,
                         int indent = 0);

} // namespace ctldb

#endif
