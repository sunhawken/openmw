#ifndef OPENMW_MWMECHANICS_BRUTALFOOTSTEPS_H
#define OPENMW_MWMECHANICS_BRUTALFOOTSTEPS_H

namespace MWWorld
{
    class Ptr;
}

// Dark Messiah footsteps, from the table and sounds brutalmorrowind-import writes
// (brutal/footsteps.txt, sound/brutal_dm/steps/). The player steps by surface and gait;
// NPCs by footwear, like Dark Messiah's guard and villager.
namespace MWMechanics::BrutalFootsteps
{
    enum class Gait
    {
        Walk,
        Stealth,
        Run,
        Sprint,
    };

    // A footstep or, with landing, the landing sound. False when Dark Messiah footsteps
    // are off or unavailable, or the actor is not on a surface they cover (swimming,
    // flying); the caller then plays Morrowind's sound.
    bool play(const MWWorld::Ptr& actor, bool left, bool landing, Gait gait);

    // Dark Messiah footsteps are on and their sounds were imported.
    bool available();

    // Dark Messiah's jump sound for the player.
    bool playJump(const MWWorld::Ptr& player);
}

#endif
