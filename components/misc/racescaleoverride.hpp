#ifndef OPENMW_COMPONENTS_MISC_RACESCALEOVERRIDE_H
#define OPENMW_COMPONENTS_MISC_RACESCALEOVERRIDE_H

#include <unordered_set>

#include <components/esm/formid.hpp>

namespace Misc::RaceScaleOverride
{
    /// NPCs (by RefNum) drawn without their race's height and weight scaling, set by scripts
    /// (Lua types.NPC.setRaceScaleIgnored). Paired-animation mods use it so two actors of different
    /// races line up as authored. Main thread only; cleared when a game is started or loaded.
    inline std::unordered_set<ESM::FormId>& ignored()
    {
        static std::unordered_set<ESM::FormId> sIgnored;
        return sIgnored;
    }

    inline bool isIgnored(ESM::FormId refNum)
    {
        const auto& set = ignored();
        return !set.empty() && refNum.isSet() && set.contains(refNum);
    }
}

#endif
