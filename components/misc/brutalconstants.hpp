#ifndef OPENMW_MISC_BRUTALCONSTANTS_H
#define OPENMW_MISC_BRUTALCONSTANTS_H
namespace Misc
{
    // Deliberate gameplay addition to DM cut velocity: a small world-up separation
    // for the head only, in Morrowind units/s. Shared with its wound particles.
    inline constexpr float BrutalHeadSeparationSpeed = 80.f;
}
#endif
