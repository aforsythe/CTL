///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "ValueFormatter.h"

#include <CtlAlign.h>
#include <CtlStdType.h>

#include <half.h>

#include <sstream>
#include <string>

namespace ctldb {

namespace {

std::string indentStr (int n)
{
    return std::string (static_cast<std::size_t>(n * 2), ' ');
}

std::string formatPrimitive (const Ctl::DataTypePtr &type, const void *data)
{
    std::ostringstream os;

    //
    // BoolType::cDataType() returns IntTypeEnum — distinguish via cast.
    // We check bool before int so a BoolType variable renders "true"/"false".
    //
    if (type.cast<Ctl::BoolType>())
    {
        os << (*static_cast<const bool*>(data) ? "true" : "false");
        return os.str();
    }

    switch (type->cDataType())
    {
      case Ctl::IntTypeEnum:
        os << *static_cast<const int*>(data);
        break;
      case Ctl::UIntTypeEnum:
        os << *static_cast<const unsigned int*>(data);
        break;
      case Ctl::HalfTypeEnum:
        os << static_cast<float>(*static_cast<const half*>(data));
        break;
      case Ctl::FloatTypeEnum:
        os << *static_cast<const float*>(data);
        break;
      case Ctl::StringTypeEnum:
        os << '"' << *static_cast<const std::string*>(data) << '"';
        break;
      case Ctl::VoidTypeEnum:
        os << "(void)";
        break;
      default:
        os << "<unknown primitive>";
        break;
    }
    return os.str();
}

} // namespace

std::string
formatValue (const Ctl::DataTypePtr &type,
             const void *data,
             int indent)
{
    if (!type) return "<no type>";
    if (!data) return "<null>";

    auto kind = type->cDataType();

    // Struct
    if (kind == Ctl::StructTypeEnum)
    {
        Ctl::StructTypePtr stype = type.cast<Ctl::StructType>();
        if (!stype) return "<struct cast failed>";

        std::ostringstream os;
        os << "{\n";
        const Ctl::MemberVector &mems = stype->members();
        for (std::size_t i = 0; i < mems.size(); ++i)
        {
            const Ctl::Member &m = mems[i];
            os << indentStr (indent + 1) << m.name << ": "
               << formatValue (m.type,
                               static_cast<const char*>(data) + m.offset,
                               indent + 1)
               << (i + 1 == mems.size() ? "\n" : ",\n");
        }
        os << indentStr (indent) << "}";
        return os.str();
    }

    // Array
    if (kind == Ctl::ArrayTypeEnum)
    {
        Ctl::ArrayTypePtr atype = type.cast<Ctl::ArrayType>();
        if (!atype) return "<array cast failed>";

        int n = atype->size();
        std::size_t stride = atype->elementType()->alignedObjectSize();
        std::ostringstream os;
        os << "[";
        for (int i = 0; i < n; ++i)
        {
            if (i) os << ", ";
            os << formatValue (atype->elementType(),
                               static_cast<const char*>(data) +
                               static_cast<std::size_t>(i) * stride,
                               indent);
            // Truncate very long arrays — print first 10, then "...".
            if (i == 9 && n > 10)
            {
                os << ", ... (" << (n - 10) << " more)";
                break;
            }
        }
        os << "]";
        return os.str();
    }

    // Primitives (bool, int, uint, half, float, string, void)
    return formatPrimitive (type, data);
}

} // namespace ctldb
