#ifndef OPENMW_COMPONENTS_SCENEUTIL_BONEPIECES_H
#define OPENMW_COMPONENTS_SCENEUTIL_BONEPIECES_H

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include <osg/Group>
#include <osg/ref_ptr>

namespace SceneUtil
{
    /// Multi-piece equipment: a part model (for example a weapon) may contain
    /// nodes named "AttachTo:<bone>" or "AttachTo:<bone>|<label>". When the part
    /// is attached to an actor, each such node, with its children, is moved from
    /// the part onto the named bone of the actor skeleton. The node keeps its own
    /// local transform, which becomes its offset from that bone. The label after
    /// '|' only keeps node names unique when several pieces target one bone.
    ///
    /// Nodes outside any "AttachTo:" node stay on the part's normal attachment
    /// bone, so a model without such nodes behaves exactly as before.
    constexpr std::string_view sBonePiecePrefix = "AttachTo:";

    /// Returns the bone a piece node targets, or nothing if the name is not a
    /// piece name. "AttachTo:Bip01 L Hand|claw" -> "Bip01 L Hand".
    std::optional<std::string_view> getBonePieceTarget(std::string_view nodeName);

    using FindBone = std::function<osg::Group*(std::string_view boneName)>;

    struct BonePieceResult
    {
        /// Pieces now parented to their target bone. The caller owns their
        /// lifetime and must remove them when the part is removed.
        std::vector<osg::ref_ptr<osg::Node>> mRelocated;
        /// Bone names that could not be found; those pieces stay where they were.
        std::vector<std::string> mMissingBones;
    };

    /// Moves every "AttachTo:" node under \a part onto the bone returned by
    /// \a findBone. Nested piece nodes move together with their outermost piece.
    BonePieceResult relocateBonePieces(osg::Node& part, const FindBone& findBone);

    /// Detaches \a node from all of its parents.
    void detachFromParents(osg::Node& node);
}

#endif
