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

    /// Who jiggles: the player (when "jiggle player enabled" is on) and the NPCs named in
    /// "jiggle npc enabled names". Every other NPC has no body jiggle: no auto-rig, no controllers.
    inline bool actorEnabled(bool isPlayer, std::string_view actorName)
    {
        if (isPlayer)
            return Settings::game().mJigglePlayerEnabled;
        return containsName(Settings::game().mJiggleNpcEnabledNames.get(), actorName);
    }
}

#endif
