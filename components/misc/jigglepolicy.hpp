#ifndef OPENMW_COMPONENTS_MISC_JIGGLEPOLICY_H
#define OPENMW_COMPONENTS_MISC_JIGGLEPOLICY_H

#include <string>
#include <string_view>
#include <vector>

#include <components/misc/strings/algorithm.hpp>
#include <components/settings/values.hpp>

namespace Misc::JigglePolicy
{
    inline bool containsName(const std::vector<std::string>& list, std::string_view name)
    {
        if (name.empty())
            return false;
        for (const std::string& item : list)
            if (!item.empty() && Misc::StringUtils::ciEqual(item, name))
                return true;
        return false;
    }

    inline bool actorEnabled(bool isPlayer, std::string_view actorName)
    {
        if (isPlayer)
            return Settings::game().mJigglePlayerEnabled;

        // Explicit per-NPC rules are highest priority. This intentionally lets
        // "Force ON" opt a named NPC in even while the legacy Player Only switch
        // is enabled, which makes selective NPC setup practical.
        if (containsName(Settings::game().mJiggleNpcDisabledNames.get(), actorName))
            return false;
        if (containsName(Settings::game().mJiggleNpcEnabledNames.get(), actorName))
            return true;

        if (Settings::game().mJiggleBonePlayerOnly)
            return false;

        return Settings::game().mJiggleNpcDefaultEnabled;
    }
}

#endif
