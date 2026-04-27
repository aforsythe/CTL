///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlPixelEvolution.h"

#include <CtlType.h>

#include <cstdio>

namespace Ctl {

namespace {

// Tiny formatter that mirrors what ValueFormatter does for arrays.
// Lifted in here so this lib doesn't pull in the ctldb-only formatter.
std::string formatArray (const ArrayTypePtr &at, const void *data)
{
    DataTypePtr et = at->elementType().cast<DataType>();
    if (!et) return "<bad-type>";
    std::size_t stride = et->alignedObjectSize();
    int sz = at->size();

    std::string out = "[";
    for (int i = 0; i < sz; ++i)
    {
        const char *p =
            reinterpret_cast<const char*>(data) + std::size_t(i) * stride;
        char buf[32];
        switch (et->cDataType())
        {
          case FloatTypeEnum:
            std::snprintf (buf, sizeof buf, "%g",
                           static_cast<double>(
                               *reinterpret_cast<const float*>(p)));
            break;
          // half: use the half->float widening that's already linked
          // for ctlrender; we don't need to be precise about the bit
          // pattern, just printable.
          case HalfTypeEnum:
            std::snprintf (buf, sizeof buf, "%g",
                           static_cast<double>(
                               *reinterpret_cast<const float*>(p)));
            break;
          default:
            std::snprintf (buf, sizeof buf, "?");
        }
        out += buf;
        if (i + 1 < sz) out += ", ";
    }
    out += "]";
    return out;
}

} // namespace

std::string
formatColorEvolution (const std::vector<InspectableVar> &vars)
{
    std::string out;
    bool any = false;
    for (const auto &v : vars)
    {
        ArrayTypePtr atype = v.type.cast<ArrayType>();
        if (!atype) continue;
        int sz = atype->size();
        if (sz != 3 && sz != 4) continue;
        DataTypePtr et = atype->elementType();
        if (!et) continue;
        // Float-y elements only — treat half / float as colors, drop
        // int / bool arrays (LUT indices, branch masks, etc.).
        auto k = et->cDataType();
        if (k != FloatTypeEnum && k != HalfTypeEnum) continue;

        // Strip the qualified-name namespace prefix for compactness.
        std::string name = v.name;
        auto rpos = name.rfind ("::");
        if (rpos != std::string::npos) name = name.substr (rpos + 2);

        if (any) out += ", ";
        out += name + "=" + formatArray (atype, v.data);
        any = true;
    }
    return out;
}

} // namespace Ctl
