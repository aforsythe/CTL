///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef INCLUDED_CTL_METAL_HALF_EXP_LOG_TABLE_H
#define INCLUDED_CTL_METAL_HALF_EXP_LOG_TABLE_H

//-----------------------------------------------------------------------------
//
//  Accessors for the precomputed half-precision exp/log tables used by
//  the Metal backend's `exp_h` / `log_h` / `log10_h` / `pow10_h` stdlib
//  helpers. The tables themselves are the same data the CPU SIMD
//  backend ships in `lib/IlmCtlSimd/halfExpLogTable.h`; we pull that
//  file in via the .cpp so IlmCtlMetal owns its own copy of the table
//  symbols and does not have to link against IlmCtlSimd.
//
//  `halfLog10Table()[i]` / `halfLogTable()[i]` return the 32-bit
//  bit-pattern for `float(log10(half(i)))` / `float(log(half(i)))` where
//  `i` is the 16-bit half bit-pattern. `halfExpTable()[k]` returns the
//  16-bit half bit-pattern for `half(exp(kLmin + k/kExpScale))` where
//  `kLmin`, `kExpScale`, and `kExpOffset` match the CPU SIMD backend's
//  `exp_h` constants byte-for-byte. `halfExpTableSize()` is the number
//  of entries in `halfExpTable()`.
//
//-----------------------------------------------------------------------------

#include <cstddef>

namespace Ctl {

const unsigned int *   halfLog10Table();
const unsigned int *   halfLogTable();
const unsigned short * halfExpTable();

size_t                 halfExpTableSize();

} // namespace Ctl

#endif
