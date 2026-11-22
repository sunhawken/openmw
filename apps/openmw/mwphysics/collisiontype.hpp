#ifndef OPENMW_MWPHYSICS_COLLISIONTYPE_H
#define OPENMW_MWPHYSICS_COLLISIONTYPE_H

#include "joltlayers.hpp"

namespace MWPhysics
{
    // Shared gameplay query names mapped to the native Jolt layers.
    inline constexpr int CollisionType_None = 0;
    inline constexpr int CollisionType_World = Layers::WORLD;
    inline constexpr int CollisionType_HeightMap = Layers::HEIGHTMAP;
    inline constexpr int CollisionType_Door = Layers::DOOR;
    inline constexpr int CollisionType_Actor = Layers::ACTOR;
    inline constexpr int CollisionType_Player = Layers::ACTOR;
    inline constexpr int CollisionType_Dynamic = Layers::DYNAMIC_WORLD;
}

#endif
