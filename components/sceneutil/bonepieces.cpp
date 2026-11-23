#include "bonepieces.hpp"

#include <string>

#include <osg/NodeVisitor>

#include <components/misc/strings/algorithm.hpp>

namespace SceneUtil
{
    namespace
    {
        // Collects the outermost piece nodes; nested pieces travel with their
        // ancestor, so the traversal does not descend into a found piece.
        class CollectPieces : public osg::NodeVisitor
        {
        public:
            CollectPieces()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Node& node) override
            {
                if (getBonePieceTarget(node.getName()))
                {
                    mFound.emplace_back(&node);
                    return;
                }
                traverse(node);
            }

            std::vector<osg::ref_ptr<osg::Node>> mFound;
        };
    }

    std::optional<std::string_view> getBonePieceTarget(std::string_view nodeName)
    {
        if (!Misc::StringUtils::ciStartsWith(nodeName, sBonePiecePrefix))
            return std::nullopt;
        std::string_view bone = nodeName.substr(sBonePiecePrefix.size());
        if (const std::size_t label = bone.find('|'); label != std::string_view::npos)
            bone = bone.substr(0, label);
        while (!bone.empty() && bone.front() == ' ')
            bone.remove_prefix(1);
        while (!bone.empty() && bone.back() == ' ')
            bone.remove_suffix(1);
        if (bone.empty())
            return std::nullopt;
        return bone;
    }

    BonePieceResult relocateBonePieces(osg::Node& part, const FindBone& findBone)
    {
        BonePieceResult result;
        CollectPieces collect;
        part.accept(collect);

        for (const osg::ref_ptr<osg::Node>& piece : collect.mFound)
        {
            // The part root itself cannot be a piece: it is already attached by
            // the caller and its lifetime is managed there.
            if (piece.get() == &part)
                continue;
            const std::string_view boneName = *getBonePieceTarget(piece->getName());
            osg::Group* bone = findBone(boneName);
            if (!bone)
            {
                result.mMissingBones.emplace_back(boneName);
                continue;
            }
            detachFromParents(*piece);
            bone->addChild(piece);
            result.mRelocated.push_back(piece);
        }
        return result;
    }

    void detachFromParents(osg::Node& node)
    {
        // Copy the list: removeChild() edits it.
        const osg::Node::ParentList parents = node.getParents();
        for (osg::Group* parent : parents)
            parent->removeChild(&node);
    }
}
