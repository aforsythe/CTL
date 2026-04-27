///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlPixelBinder.h"

#include <algorithm>

namespace Ctl {

namespace {

std::string lower (std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

} // namespace

void
bindPixelInputs (FunctionCallPtr &fc, const std::vector<float> &pixel)
{
    // Per-channel scalar synonyms, indexed [r=0, g=1, b=2, a=3].
    static const std::vector<std::string> channelAliases[4] = {
        // Red
        {"rin", "r", "red", "redin", "rinput", "ri", "input_r", "in_r"},
        // Green
        {"gin", "g", "green", "greenin", "ginput", "gi", "input_g", "in_g"},
        // Blue
        {"bin", "b", "blue", "bluein", "binput", "bi", "input_b", "in_b"},
        // Alpha
        {"ain", "a", "alpha", "alphain", "ainput", "ai", "input_a", "in_a"},
    };

    // Aggregate-array synonyms.  The 3-element list matches a 3-channel
    // pixel arg; the 4-element list adds alpha-bearing names that
    // accept either 3 or 4 floats from the user (extra alpha defaults
    // to 1.0 when the user passes only RGB).
    static const std::vector<std::string> arrayAliases3 = {
        "rgbin", "rgb", "pixel", "pixelin", "color", "colorin", "input"
    };
    static const std::vector<std::string> arrayAliases4 = {
        "rgbain", "rgba", "pixela", "rgbpixel", "color4", "input4"
    };

    for (std::size_t i = 0; i < fc->numInputArgs(); ++i)
    {
        FunctionArgPtr arg = fc->inputArg (i);
        if (!arg) continue;

        bool bound = false;
        std::string argLow = lower (arg->name());

        // (a) Per-channel scalars.
        for (std::size_t c = 0; c < pixel.size() && c < 4 && !bound; ++c)
        {
            for (const auto &alias : channelAliases[c])
            {
                if (argLow == alias)
                {
                    float v = pixel[c];
                    arg->set (&v, 0, 0, 1);
                    bound = true;
                    break;
                }
            }
        }

        // (b) Array input.  Each element has to be written individually
        // with a "%d" path index; passing the whole vector through
        // `count` writes garbage / segfaults because `count` is the
        // number of SAMPLES, not array slots.
        if (!bound)
        {
            bool isAggregate3 = false, isAggregate4 = false;
            for (const auto &alias : arrayAliases3)
                if (argLow == alias) { isAggregate3 = true; break; }
            if (!isAggregate3)
                for (const auto &alias : arrayAliases4)
                    if (argLow == alias) { isAggregate4 = true; break; }
            if (isAggregate3 || isAggregate4)
            {
                std::size_t n = std::min (pixel.size(), std::size_t(4));
                try {
                    for (std::size_t k = 0; k < n; ++k)
                        arg->set (&pixel[k], 0, 0, 1, "%d", static_cast<int>(k));
                    // 4-channel aggregate, but user only gave 3 — fill
                    // alpha with 1.0 so opaque-by-default makes sense.
                    if (isAggregate4 && pixel.size() == 3)
                    {
                        float one = 1.0f;
                        try { arg->set (&one, 0, 0, 1, "%d", 3); }
                        catch (...) { /* arg might actually be size 3 */ }
                    }
                    bound = true;
                }
                catch (...) { /* type mismatch — fall through */ }
            }
        }

        if (!bound && arg->hasDefaultValue())
            arg->setDefaultValue();
    }
}

void
bindUniformParams (FunctionCallPtr &fc,
                   const std::map<std::string, float> &params)
{
    if (params.empty()) return;
    for (std::size_t i = 0; i < fc->numInputArgs(); ++i)
    {
        FunctionArgPtr arg = fc->inputArg (i);
        if (!arg) continue;
        auto it = params.find (arg->name());
        if (it == params.end()) continue;
        float v = it->second;
        try { arg->set (&v, 0, 0, 1); }
        catch (...) { /* type mismatch — silently skip */ }
    }
}

} // namespace Ctl
