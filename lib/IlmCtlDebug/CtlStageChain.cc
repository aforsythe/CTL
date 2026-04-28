///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "CtlStageChain.h"

#include <CtlType.h>

namespace Ctl {

std::string
pathDirname (const std::string &p)
{
    std::size_t slash = p.find_last_of ('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0)                 return "/";
    return p.substr (0, slash);
}

std::string
pathStem (const std::string &p)
{
    std::size_t slash = p.find_last_of ('/');
    std::string base  = (slash == std::string::npos) ? p : p.substr (slash + 1);
    std::size_t dot   = base.find_last_of ('.');
    // dot==0 keeps dotfiles intact (".ctlrc" -> ".ctlrc", not "").
    if (dot == std::string::npos || dot == 0) return base;
    return base.substr (0, dot);
}

std::vector<std::string>
parseColonPath (const char *env)
{
    std::vector<std::string> out;
    if (!env) return out;
    std::string s = env;
    std::size_t pos = 0;
    while (pos < s.size())
    {
        std::size_t colon = s.find (':', pos);
        std::string seg = s.substr (
            pos,
            colon == std::string::npos ? std::string::npos : colon - pos);
        if (!seg.empty()) out.push_back (seg);
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return out;
}

std::map<std::string, float>
captureOutputs (FunctionCallPtr &fc)
{
    std::map<std::string, float> result;
    for (std::size_t i = 0; i < fc->numOutputArgs(); ++i)
    {
        FunctionArgPtr arg = fc->outputArg (i);
        if (!arg) continue;
        if (arg->type() && arg->type()->cDataType() == FloatTypeEnum)
        {
            float v = 0.0f;
            arg->get (&v, 0, 0, 1);
            result[arg->name()] = v;
        }
    }
    return result;
}

void
bindFromPriorStage (FunctionCallPtr &fc,
                    const std::map<std::string, float> &prevOutputs)
{
    for (std::size_t i = 0; i < fc->numInputArgs(); ++i)
    {
        FunctionArgPtr arg = fc->inputArg (i);
        if (!arg) continue;

        const std::string &inName = arg->name();
        bool bound = false;

        // 1) Exact name match.
        {
            auto it = prevOutputs.find (inName);
            if (it != prevOutputs.end())
            {
                float v = it->second;
                arg->set (&v, 0, 0, 1);
                bound = true;
            }
        }

        // 2) "*In" -> "*Out" suffix synonym (rIn<-rOut, gIn<-gOut, ...).
        if (!bound && inName.size() >= 2 &&
            inName.substr (inName.size() - 2) == "In")
        {
            std::string candidate = inName.substr (0, inName.size() - 2) + "Out";
            auto it = prevOutputs.find (candidate);
            if (it != prevOutputs.end())
            {
                float v = it->second;
                arg->set (&v, 0, 0, 1);
                bound = true;
            }
        }

        if (!bound && arg->hasDefaultValue())
            arg->setDefaultValue();
    }
}

} // namespace Ctl
