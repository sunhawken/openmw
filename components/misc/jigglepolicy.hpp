#ifndef OPENMW_COMPONENTS_MISC_JIGGLEPOLICY_H
#define OPENMW_COMPONENTS_MISC_JIGGLEPOLICY_H

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm/formid.hpp>

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

    /// Per-actor overrides set by scripts (Lua types.NPC.setJiggleEnabled), keyed by the actor's RefNum.
    /// They let a mod give jiggle to actors it spawns, such as animation stand-ins for the player.
    /// Written by delayed Lua actions and read by the render update, both on the main thread.
    /// Cleared when a game is started or loaded.
    inline std::unordered_map<ESM::FormId, bool>& overrides()
    {
        static std::unordered_map<ESM::FormId, bool> sOverrides;
        return sOverrides;
    }

    inline std::optional<bool> actorOverride(ESM::FormId refNum)
    {
        const auto& map = overrides();
        if (map.empty() || !refNum.isSet())
            return std::nullopt;
        const auto it = map.find(refNum);
        if (it == map.end())
            return std::nullopt;
        return it->second;
    }

    /// Who jiggles: an actor with a script override follows it; otherwise the player (when
    /// "jiggle player enabled" is on) and the NPCs named in "jiggle npc enabled names".
    /// Every other NPC has no body jiggle: no auto-rig, no controllers.
    inline bool actorEnabled(bool isPlayer, std::string_view actorName, ESM::FormId refNum = {})
    {
        if (const std::optional<bool> forced = actorOverride(refNum))
            return *forced;
        if (isPlayer)
            return Settings::game().mJigglePlayerEnabled;
        return containsName(Settings::game().mJiggleNpcEnabledNames.get(), actorName);
    }
}

#endif
