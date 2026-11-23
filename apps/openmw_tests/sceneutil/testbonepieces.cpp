#include <components/sceneutil/bonepieces.hpp>

#include <gtest/gtest.h>

#include <osg/MatrixTransform>

#include <map>
#include <string>

namespace
{
    using namespace SceneUtil;

    osg::ref_ptr<osg::MatrixTransform> makeNode(const std::string& name, const osg::Vec3f& offset = {})
    {
        osg::ref_ptr<osg::MatrixTransform> node = new osg::MatrixTransform(osg::Matrix::translate(offset));
        node->setName(name);
        return node;
    }

    // A minimal skeleton: root -> right hand, left hand, right foot.
    struct TestSkeleton
    {
        osg::ref_ptr<osg::Group> mRoot = makeNode("Bip01");
        std::map<std::string, osg::ref_ptr<osg::Group>, std::less<>> mBones;

        TestSkeleton()
        {
            for (const char* name : { "Bip01 R Hand", "Bip01 L Hand", "Bip01 R Foot" })
            {
                osg::ref_ptr<osg::Group> bone = makeNode(name);
                mRoot->addChild(bone);
                mBones.emplace(name, bone);
            }
        }

        FindBone finder()
        {
            return [this](std::string_view name) -> osg::Group* {
                const auto it = mBones.find(name);
                return it == mBones.end() ? nullptr : it->second.get();
            };
        }
    };

    TEST(SceneUtilBonePiecesTest, parsesTargetBone)
    {
        EXPECT_EQ(getBonePieceTarget("AttachTo:Bip01 L Hand"), std::string_view("Bip01 L Hand"));
        EXPECT_EQ(getBonePieceTarget("attachto:Bip01 L Hand|claw2"), std::string_view("Bip01 L Hand"));
        EXPECT_EQ(getBonePieceTarget("AttachTo: Bip01 R Foot |x"), std::string_view("Bip01 R Foot"));
        EXPECT_FALSE(getBonePieceTarget("Bip01 L Hand"));
        EXPECT_FALSE(getBonePieceTarget("AttachTo:"));
        EXPECT_FALSE(getBonePieceTarget("AttachTo:|label"));
    }

    TEST(SceneUtilBonePiecesTest, modelWithoutPiecesIsUnchanged)
    {
        TestSkeleton skeleton;
        osg::ref_ptr<osg::Group> part = makeNode("Weapon");
        part->addChild(makeNode("Blade"));
        skeleton.mBones["Bip01 R Hand"]->addChild(part);

        const BonePieceResult result = relocateBonePieces(*part, skeleton.finder());

        EXPECT_TRUE(result.mRelocated.empty());
        EXPECT_TRUE(result.mMissingBones.empty());
        EXPECT_EQ(part->getNumChildren(), 1u);
    }

    TEST(SceneUtilBonePiecesTest, piecesMoveToTheirBonesAndKeepLocalOffset)
    {
        TestSkeleton skeleton;
        osg::ref_ptr<osg::Group> part = makeNode("Weapon");
        osg::ref_ptr<osg::MatrixTransform> main = makeNode("Main");
        osg::ref_ptr<osg::MatrixTransform> left = makeNode("AttachTo:Bip01 L Hand|a", osg::Vec3f(1, 2, 3));
        osg::ref_ptr<osg::MatrixTransform> foot1 = makeNode("AttachTo:Bip01 R Foot|a");
        osg::ref_ptr<osg::MatrixTransform> foot2 = makeNode("AttachTo:Bip01 R Foot|b");
        part->addChild(main);
        part->addChild(left);
        main->addChild(foot1); // A piece nested below a normal node still moves.
        part->addChild(foot2);
        skeleton.mBones["Bip01 R Hand"]->addChild(part);

        const BonePieceResult result = relocateBonePieces(*part, skeleton.finder());

        ASSERT_EQ(result.mRelocated.size(), 3u);
        EXPECT_TRUE(result.mMissingBones.empty());
        // Only the normal node remains on the part.
        ASSERT_EQ(part->getNumChildren(), 1u);
        EXPECT_EQ(part->getChild(0), main.get());
        EXPECT_EQ(main->getNumChildren(), 0u);
        // Each piece has exactly one parent: its bone.
        ASSERT_EQ(left->getNumParents(), 1u);
        EXPECT_EQ(left->getParent(0), skeleton.mBones["Bip01 L Hand"].get());
        EXPECT_EQ(foot1->getParent(0), skeleton.mBones["Bip01 R Foot"].get());
        EXPECT_EQ(foot2->getParent(0), skeleton.mBones["Bip01 R Foot"].get());
        EXPECT_EQ(left->getMatrix().getTrans(), osg::Vec3d(1, 2, 3));
    }

    TEST(SceneUtilBonePiecesTest, nestedPiecesTravelWithOutermostPiece)
    {
        TestSkeleton skeleton;
        osg::ref_ptr<osg::Group> part = makeNode("Weapon");
        osg::ref_ptr<osg::MatrixTransform> outer = makeNode("AttachTo:Bip01 L Hand");
        osg::ref_ptr<osg::MatrixTransform> inner = makeNode("AttachTo:Bip01 R Foot");
        outer->addChild(inner);
        part->addChild(outer);

        const BonePieceResult result = relocateBonePieces(*part, skeleton.finder());

        ASSERT_EQ(result.mRelocated.size(), 1u);
        EXPECT_EQ(outer->getParent(0), skeleton.mBones["Bip01 L Hand"].get());
        EXPECT_EQ(inner->getParent(0), outer.get());
    }

    TEST(SceneUtilBonePiecesTest, missingBoneLeavesPieceInPlace)
    {
        TestSkeleton skeleton;
        osg::ref_ptr<osg::Group> part = makeNode("Weapon");
        osg::ref_ptr<osg::MatrixTransform> tail = makeNode("AttachTo:Bip01 Tail");
        part->addChild(tail);

        const BonePieceResult result = relocateBonePieces(*part, skeleton.finder());

        EXPECT_TRUE(result.mRelocated.empty());
        ASSERT_EQ(result.mMissingBones.size(), 1u);
        EXPECT_EQ(result.mMissingBones[0], "Bip01 Tail");
        EXPECT_EQ(tail->getParent(0), part.get());
    }

    TEST(SceneUtilBonePiecesTest, partRootIsNeverRelocated)
    {
        TestSkeleton skeleton;
        osg::ref_ptr<osg::Group> part = makeNode("AttachTo:Bip01 L Hand");
        skeleton.mBones["Bip01 R Hand"]->addChild(part);

        const BonePieceResult result = relocateBonePieces(*part, skeleton.finder());

        EXPECT_TRUE(result.mRelocated.empty());
        EXPECT_EQ(part->getParent(0), skeleton.mBones["Bip01 R Hand"].get());
    }

    TEST(SceneUtilBonePiecesTest, detachRemovesEveryParent)
    {
        osg::ref_ptr<osg::Group> a = makeNode("A");
        osg::ref_ptr<osg::Group> b = makeNode("B");
        osg::ref_ptr<osg::Node> child = makeNode("C");
        a->addChild(child);
        b->addChild(child);

        detachFromParents(*child);

        EXPECT_EQ(child->getNumParents(), 0u);
        EXPECT_EQ(a->getNumChildren(), 0u);
        EXPECT_EQ(b->getNumChildren(), 0u);
    }
}
