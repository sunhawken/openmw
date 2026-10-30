#ifndef OPENMW_MWKURENAI_NATIVEBODY_H
#define OPENMW_MWKURENAI_NATIVEBODY_H

#include <kurenai/protocol.hpp>

#include <osg/Matrixf>
#include <osg/observer_ptr>
#include <osg/ref_ptr>

#include <cstdint>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace osg
{
    class Group;
    class MatrixTransform;
    class Node;
    class Callback;
}

namespace MWWorld
{
    class Ptr;
}

namespace MWKurenai
{
    // Kurenai's own PS2 model, posed with the game's bone matrices unchanged (Kurenai mode). The model is a NIF that
    // tools/krn_native_body.py builds from the user's disc: the game's mesh, skin weights and every node of its
    // skeleton, named krn_<index>_<game name>. It is its own node beside the player's body, not a body part: Morrowind's
    // animation, jiggle and ragdoll never touch it, and only update() writes its bones, once a frame, from PCSX2's Pose.
    // While it shows, the player's Morrowind body is hidden (not drawn, still animated), so F6 gets it back as it was.
    class NativeBody
    {
    public:
        // models: VFS paths of the native models (one per outfit); missing files are skipped.
        explicit NativeBody(std::vector<std::string> models);
        ~NativeBody();

        // Main thread, after the player is placed. show: Kurenai mode with PS2 animation; pose and state from the same
        // game frame (state: her position and rotation, Red Ninja coordinates); scale: Morrowind units per game unit.
        // Returns true while the native body is shown.
        // faint: Kiton hides her (she stands or creeps while it lasts): the game draws her as a faint dark shape.
        bool update(const MWWorld::Ptr& player, bool show, const kurenai::proto::Pose* pose,
            const kurenai::proto::PlayerState* state, float scale, bool faint = false);

        // Diagnostics for the status line and the log.
        std::size_t modelsLoaded() const { return mModels.size(); }
        // The outfit to show: the path of one of the loaded models (empty: the first that fits).
        void setOutfit(const std::string& path);
        const std::string& outfit() const { return mWanted; }
        std::vector<std::string> outfits();
        static std::string twin(const std::string& path);
        const std::string& current() const;

        // Her tetsugen's attachment: 0 the blade (the game's own held blade), 1 the fundo (the weight, drawn in the blade's place).
        void setAttachment(int attachment) { mAttachment = attachment; }
        void setWeaponLevels(int blade, int fundo)
        {
            mBladeLevel = std::clamp(blade, 0, 2);
            mFundoLevel = std::clamp(fundo, 0, 2);
        }
        int attachment() const { return mAttachment; }

    private:
        struct Bone
        {
            osg::MatrixTransform* node = nullptr;
            std::string name;
            int parent = -1; // model index of the nearest bone above, -1 = the model's root
            osg::Matrixf rest; // bind pose, relative to the parent
        };
        struct Model
        {
            std::string path;
            osg::ref_ptr<osg::Node> instance;
            std::vector<Bone> bones; // by model node index (nullptr node: index unused)
            std::map<int, std::vector<osg::ref_ptr<osg::Node>>> materials; // by the game's material index
        };
        // Which model fits a game model id, and the live slot of each of its bones (-1: not animated, kept as modelled).
        struct Fit
        {
            int model = -1;
            std::vector<int> slots;
            int matched = 0;
            int shift = 0;
        };

        void load();
        Fit fit(const kurenai::proto::Pose& pose) const;
        Fit fitModel(const kurenai::proto::Pose& pose, int model, int hipSlot) const;
        void attach(const MWWorld::Ptr& player, int model);
        void hide(bool hidden);

        std::vector<std::string> mPaths;
        std::string mWanted;
        bool mLoaded = false;
        std::vector<Model> mModels;
        std::map<std::uint32_t, Fit> mFits;
        int mShown = -1; // model attached and visible
        int mAttachment = 1; // 0 blade, 1 fundo (the game draws the weight in her hand from the start)
        osg::ref_ptr<osg::MatrixTransform> mPlace; // game character space -> the actor's space
        osg::observer_ptr<osg::Group> mParent;
        osg::observer_ptr<osg::Node> mHiddenRoot;
        osg::ref_ptr<osg::Callback> mHideCallback;
        std::vector<osg::Matrixf> mWorld; // scratch: each bone in game character space

        // Her items (meshes/krn_native/krn_item_<slot>.nif), each placed at the game's matrix while the game draws it.
        struct Item
        {
            osg::ref_ptr<osg::MatrixTransform> place;
            std::map<int, std::vector<osg::ref_ptr<osg::Node>>> materials;
            std::vector<osg::ref_ptr<osg::Node>> versions; // the blade's and the weight's upgrade versions (level 1..3)
        };
        std::map<std::uint32_t, Item> mItems;
        int mBladeLevel = 0, mFundoLevel = 0; // 0..2: which version of the blade / the weight is drawn
        std::vector<kurenai::proto::Color> mColors; // last applied to the body
        bool mWarnedNoMaterials = false;
        void applyColors(std::map<int, std::vector<osg::ref_ptr<osg::Node>>>& nodes,
            const std::vector<kurenai::proto::Color>& colors);
    };
}

#endif
