#ifndef OPENMW_MWKURENAI_HDUI_H
#define OPENMW_MWKURENAI_HDUI_H

#include <kurenai/protocol.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

namespace MyGUI
{
    class ImageBox;
    class ITexture;
}

namespace MWKurenai
{
    // [Kurenai] Red Ninja's item menu and gameplay HUD, drawn natively at the window's resolution from textures baked
    // out of the user's own HD texture pack (tools/krn_hdui_bake.py). The game only says what to show (UiState).
    class HdUi
    {
    public:
        // The folder holding menu_bg.png, item_NN.png, manifest.txt ...; false if it is not there.
        bool load(const std::string& dir);
        bool loaded() const { return mLoaded; }
        // Shows the menu (ui.menuOpen) or the HUD (hud), or nothing. health/ninjutsu are shares 0..1.
        void update(const kurenai::proto::UiState& ui, float health, bool hud);
        void hide();
        bool menuUp() const { return mMenuUp; }

    private:
        struct Image
        {
            MyGUI::ITexture* texture = nullptr;
            std::string name;
            int w = 0, h = 0; // pixels of the picture
        };
        struct Rect
        {
            float x = 0, y = 0, w = 0, h = 0;
        };
        struct ItemInfo
        {
            float cx = 0, cy = 0, iw = 0, ih = 0, gw = 0, gh = 0, tx = 0, ty = 0, tw = 0, th = 0;
        };
        bool mLoaded = false, mMenuUp = false, mHudUp = false;
        std::map<std::string, Image> mImages;
        std::map<std::string, Rect> mRects;
        std::array<ItemInfo, kurenai::proto::kUiItems> mItems;
        MyGUI::ImageBox* mBackdrop = nullptr;
        MyGUI::ImageBox* mBg = nullptr;
        std::vector<MyGUI::ImageBox*> mIcons, mGlows, mTexts, mCounts1, mCounts2;
        MyGUI::ImageBox *mMap = nullptr, *mOptions = nullptr, *mHints = nullptr;
        MyGUI::ImageBox *mRing = nullptr, *mWeapon = nullptr, *mFrame = nullptr, *mHp = nullptr, *mNinja = nullptr;
        MyGUI::ImageBox *mSlotTri = nullptr, *mSlotCirc = nullptr, *mSlotTriItem = nullptr, *mSlotCircItem = nullptr;

        bool loadImage(const std::string& dir, const std::string& file);
        MyGUI::ImageBox* box(const std::string& image);
        void set(MyGUI::ImageBox* b, float x, float y, float w, float h, float scale, float ox, float oy, bool cornerRight,
            bool cornerBottom, int viewW, int viewH);
        void setImage(MyGUI::ImageBox* b, const std::string& image);
    };
}

#endif
