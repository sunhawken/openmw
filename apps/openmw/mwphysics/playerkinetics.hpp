#ifndef OPENMW_MWPHYSICS_PLAYERKINETICS_H
#define OPENMW_MWPHYSICS_PLAYERKINETICS_H

namespace MWPhysics::PlayerKinetics
{
    // Dark Messiah's player movement, in Morrowind units (1 DM inch = DmScale units).
    // Speeds are the server's hl2_* ConVars as selected by its movement-state table:
    // the default gait uses hl2_sprintspeed, Alt (+speed) hl2_walkspeed and Shift
    // (+sprint) hl2_megasprintspeed. Ground movement follows Source's sv_accelerate 10,
    // sv_friction 4 and sv_stopspeed 50; air movement sv_airaccelerate 10 with HL2's
    // 30-inch air wish-speed cap.
    inline constexpr float DmScale = 1.84265547703229f;
    inline constexpr float WalkSpeed = 100.f * DmScale; // hl2_walkspeed
    inline constexpr float RunSpeed = 180.f * DmScale; // hl2_sprintspeed
    inline constexpr float SprintSpeed = 270.f * DmScale; // hl2_megasprintspeed
    inline constexpr float SneakSpeed = 50.f * DmScale; // hl2_duckspeed
    inline constexpr float SwimSpeed = 180.f;
    inline constexpr float FlySpeed = 220.f;
    inline constexpr float JumpSpeed = 320.f;
    inline constexpr float Accelerate = 10.f; // per second, times the wish speed
    inline constexpr float Friction = 4.f;
    inline constexpr float SlideFriction = 1.f;
    // Authored slide entry push; cap repeated entries at 115% of sprint speed.
    inline constexpr float SlideEntrySpeedMultiplier = 1.15f;
    // Allow the queued physical knockdown to replace a contacted NPC's standing
    // hull. The cumulative budget bounds failed handoffs over the whole glide.
    inline constexpr float SlideActorContactDuration = .75f;
    inline constexpr float SlideHeightFraction = 0.42f;
    inline constexpr float StopSpeed = 50.f * DmScale;
    inline constexpr float AirAccelerate = 10.f;
    inline constexpr float AirWishCap = 30.f * DmScale;

    enum Gait { Gait_Stance = 0, Gait_Walk = 1, Gait_Run = 2, Gait_Sprint = 3 };

    // DM's directional speeds: x = side input, y = forward input (unit vector).
    inline float directionalSpeed(int gait, bool sneak, float x, float y)
    {
        const bool side = x * x > 0.09f, forward = y > 0.3f, back = y < -0.3f;
        float dm;
        if (sneak)
            dm = back ? (side ? 60.f : 30.f) : forward ? (side ? 60.f : 50.f) : 45.f;
        else if (back)
            dm = side ? 80.f : 60.f; // hl2_walkforwardsidespeed / hl2_sprintbackspeed
        else if (gait == Gait_Walk)
            dm = forward && side ? 80.f : 100.f;
        else if (gait == Gait_Sprint && forward && !side)
            dm = 270.f;
        else
            dm = 180.f;
        return dm * DmScale;
    }
}
#endif
