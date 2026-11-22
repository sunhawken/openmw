#ifndef GAME_MWMECHANICS_BRUTALOPTIONS_H
#define GAME_MWMECHANICS_BRUTALOPTIONS_H

#include <components/esm3/loadweap.hpp>
#include <components/settings/values.hpp>

namespace MWMechanics
{
    // Dark Messiah first-person mode. The classic mode restores OpenMW's own arms,
    // attack animations, passive block and third-person toggle at run time.
    inline bool brutalDmFirstPerson()
    {
        return Settings::game().mBrutalEnabled && Settings::game().mFirstPersonRigidViewmodel && !Settings::game().mBrutalClassicFirstPerson;
    }

    // Requested dual-wield preference is owned by the permanent Lua settings section.
    // This runtime switch avoids a second, potentially conflicting settings.cfg owner.
    inline bool sBrutalDualWield = true;
    inline bool brutalDualWield()
    {
        return Settings::game().mBrutalEnabled && sBrutalDualWield;
    }

    inline bool brutalStaffWeapon(int type)
    {
        return type == ESM::Weapon::BluntTwoWide || type == ESM::Weapon::SpearTwoWide;
    }
}

#endif
