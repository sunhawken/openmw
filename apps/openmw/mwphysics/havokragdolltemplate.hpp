#ifndef OPENMW_MWPHYSICS_HAVOKRAGDOLLTEMPLATE_H
#define OPENMW_MWPHYSICS_HAVOKRAGDOLLTEMPLATE_H

#include "ragdollbuilder.hpp"

namespace Resource
{
    class ResourceSystem;
}

namespace MWWorld
{
    class Ptr;
}

namespace MWPhysics
{
    /// Scans an actor's model NIF for an authored Havok ragdoll template
    /// (bhkRagdollTemplate extra data, as found in NIFs with skeletons ported
    /// from later Bethesda games such as Oblivion or Skyrim) and converts any
    /// bones it describes into per-bone JointConfig overrides.
    ///
    /// For each bone the template covers, the returned config starts from
    /// RagdollSettingsBuilder::getDefaultConfig() and overlays the authored
    /// mass/friction/restitution/radius (always meaningful regardless of
    /// constraint type) plus swing/twist joint limits when the bone's own
    /// constraint is a type that maps onto Jolt's SwingTwistConstraint
    /// (Havok's Ragdoll or LimitedHinge constraints) - other constraint types
    /// (BallAndSocket, unlimited Hinge, StiffSpring, Prismatic, Malleable) keep
    /// the heuristic default limits, since they don't have compatible
    /// cone/twist semantics to translate.
    ///
    /// Returns an empty map if the model has no bhkRagdollTemplate (e.g. every
    /// vanilla Morrowind creature/NPC skeleton), in which case the caller
    /// should build the ragdoll exactly as before, with no overrides.
    RagdollSettingsBuilder::JointConfigMap loadHavokRagdollTemplate(
        const MWWorld::Ptr& ptr, Resource::ResourceSystem* resourceSystem);
}

#endif
