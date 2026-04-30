///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

//
// Own-copy of the CPU SIMD backend's precomputed half-precision exp/log
// tables. `halfExpLogTable.h` defines three arrays at namespace scope:
// `log10Table[65536]`, `logTable[65536]`, `expTable[113536]`. We pull the
// file in inside an anonymous namespace so the symbols stay local to this
// translation unit and cannot collide with IlmCtlSimd's copy if both
// libraries are linked into the same binary. The accessors below expose
// the tables to `CtlMetalCodegen.cpp` without leaking the symbols any
// further.
//

#include <CtlMetalHalfExpLogTable.h>

namespace Ctl {

namespace {

#include "../IlmCtlSimd/halfExpLogTable.h"

} // anonymous namespace

const unsigned int *
halfLog10Table()
{
    return log10Table;
}

const unsigned int *
halfLogTable()
{
    return logTable;
}

const unsigned short *
halfExpTable()
{
    return expTable;
}

size_t
halfExpTableSize()
{
    return sizeof(expTable) / sizeof(expTable[0]);
}

} // namespace Ctl
