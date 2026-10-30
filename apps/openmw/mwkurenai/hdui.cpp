#include "hdui.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <osg/Image>
#include <osgDB/ReadFile>

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <components/debug/debuglog.hpp>

namespace MWKurenai
{
    namespace
    {
        constexpr float kGameW = 640, kGameH = 480;
        constexpr int kPicScale = 3; // the baked pictures are 3 pixels per PS2 pixel

        std::string itemFile(const char* kind, std::size_t i)
        {
            char b[32];
            std::snprintf(b, sizeof b, "%s_%02zu.png", kind, i);
            return b;
        }
    }

    bool HdUi::loadImage(const std::string& dir, const std::string& file)
    {
        osg::ref_ptr<osg::Image> img = osgDB::readImageFile(dir + "/" + file);
        if (!img || img->s() <= 0 || img->t() <= 0)
            return false;
        int tw = 1, th = 1;
        while (tw < img->s())
            tw *= 2;
        while (th < img->t())
            th *= 2;
        MyGUI::RenderManager& rm = MyGUI::RenderManager::getInstance();
        Image out;
        out.name = "kurenai_hdui_" + file;
        out.w = img->s();
        out.h = img->t();
        out.texture = rm.createTexture(out.name);
        out.texture->createManual(tw, th, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
        auto* data = static_cast<std::uint8_t*>(out.texture->lock(MyGUI::TextureUsage::Write));
        if (!data)
            return false;
        std::fill_n(data, static_cast<std::size_t>(tw) * th * 4, std::uint8_t(0));
        const bool alpha = img->getPixelFormat() == GL_RGBA;
        const bool rgb = img->getPixelFormat() == GL_RGB;
        if (!alpha && !rgb)
        {
            out.texture->unlock();
            Log(Debug::Warning) << "Kurenai: hd ui: " << file << " has an unsupported pixel format";
            return false;
        }
        const int bpp = alpha ? 4 : 3;
        for (int y = 0; y < img->t(); ++y)
        {
            // osg images start at the bottom row
            const std::uint8_t* src = img->data(0, img->t() - 1 - y);
            std::uint8_t* dst = data + static_cast<std::size_t>(y) * tw * 4;
            for (int x = 0; x < img->s(); ++x)
            {
                dst[x * 4 + 0] = src[x * bpp + 0];
                dst[x * 4 + 1] = src[x * bpp + 1];
                dst[x * 4 + 2] = src[x * bpp + 2];
                dst[x * 4 + 3] = alpha ? src[x * bpp + 3] : 255;
            }
        }
        out.texture->unlock();
        mImages[file] = out;
        return true;
    }

    MyGUI::ImageBox* HdUi::box(const std::string& image)
    {
        MyGUI::ImageBox* b = MyGUI::Gui::getInstance().createWidget<MyGUI::ImageBox>(
            "ImageBox", MyGUI::IntCoord(0, 0, 1, 1), MyGUI::Align::Default, "HUD");
        b->setNeedMouseFocus(false);
        b->setVisible(false);
        if (!image.empty())
            setImage(b, image);
        return b;
    }

    void HdUi::setImage(MyGUI::ImageBox* b, const std::string& image)
    {
        const auto it = mImages.find(image);
        if (it == mImages.end())
            return;
        b->setImageTexture(it->second.name);
        b->setImageCoord(MyGUI::IntCoord(0, 0, it->second.w, it->second.h));
        b->setImageTile(MyGUI::IntSize(it->second.w, it->second.h));
    }

    bool HdUi::load(const std::string& dir)
    {
        std::ifstream in(dir + "/manifest.txt");
        if (!in)
            return false;
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream s(line);
            std::string key;
            s >> key;
            if (key.rfind("item", 0) == 0 && key.size() > 4 && std::isdigit(static_cast<unsigned char>(key[4])))
            {
                const std::size_t i = std::stoul(key.substr(4));
                if (i < mItems.size())
                {
                    ItemInfo& it = mItems[i];
                    s >> it.cx >> it.cy >> it.iw >> it.ih >> it.gw >> it.gh >> it.tx >> it.ty >> it.tw >> it.th;
                }
            }
            else
            {
                Rect r;
                s >> r.x >> r.y >> r.w >> r.h;
                mRects[key] = r;
            }
        }
        bool ok = loadImage(dir, "menu_bg.png");
        for (int i = 0; ok && i < 4; ++i)
            ok = loadImage(dir, "wname_" + std::to_string(i) + ".png");
        for (int i = 1; ok && i <= 3; ++i)
            ok = loadImage(dir, "lv_" + std::to_string(i) + ".png");
        for (std::size_t i = 0; ok && i < kurenai::proto::kUiItems; ++i)
            ok = loadImage(dir, itemFile("item", i)) && loadImage(dir, itemFile("glow", i)) && loadImage(dir, itemFile("text", i));
        // the purple flames of worn and assigned items (optional: an older bake has none and the glow stands in)
        for (std::size_t i = 0; ok && i < kurenai::proto::kUiItems; ++i)
            loadImage(dir, itemFile("flame", i));
        loadImage(dir, "flame_bar.png");
        for (char d = '0'; ok && d <= '9'; ++d)
            ok = loadImage(dir, std::string("digit_") + d + ".png");
        for (const char* f : { "label_map.png", "label_options.png", "hints.png", "ring.png", "weapon_a.png", "weapon_b.png", "slot_tri.png",
                 "slot_circ.png", "whints.png", "backdrop.png", "bar_orange.png", "bar_navy.png", "bar_frame.png" })
            ok = ok && loadImage(dir, f);
        if (!ok)
        {
            Log(Debug::Error) << "Kurenai: hd ui: pictures missing in " << dir << " (run tools/krn_hdui_bake.py)";
            return false;
        }
        mBackdrop = box("backdrop.png");
        mBg = box("menu_bg.png");
        for (std::size_t i = 0; i < mItems.size(); ++i)
        {
            mGlows.push_back(box(itemFile("glow", i)));
            mFlames.push_back(box(mImages.count(itemFile("flame", i)) ? itemFile("flame", i) : itemFile("glow", i)));
            mIcons.push_back(box(itemFile("item", i)));
            mTexts.push_back(box(itemFile("text", i)));
            mCounts1.push_back(box(""));
            mCounts2.push_back(box(""));
        }
        mMap = box("label_map.png");
        mOptions = box("label_options.png");
        mHints = box("hints.png");
        mRing = box("ring.png");
        mWeapon = box("weapon_a.png");
        mFrame = box("bar_frame.png");
        mHp = box("bar_orange.png");
        mNinja = box("bar_navy.png");
        mSlotTri = box("slot_tri.png");
        mSlotCirc = box("slot_circ.png");
        mSlotTriItem = box("");
        mSlotCircItem = box("");
        for (int i = 0; i < 4; ++i)
        {
            mWBarBack.push_back(box("bar_frame.png"));
            mWBars.push_back(box("bar_orange.png"));
            mWNames.push_back(box("wname_" + std::to_string(i) + ".png"));
            mWLevels.push_back(box("lv_1.png"));
            mWIcons.push_back(box(i < 2 ? "weapon_b.png" : "weapon_a.png")); // the Blade rows, then the Fundo rows
            mWFlames.push_back(box(mImages.count("flame_bar.png") ? "flame_bar.png" : "bar_orange.png"));
        }
        mWHints = box("whints.png");
        mLoaded = true;
        Log(Debug::Info) << "Kurenai: hd ui loaded from " << dir;
        return true;
    }

    // The Tetsugen's four parts are the item menu's second screen: with `page` up the items give way to the list of parts (same
    // background and HUD), one row lit; `browsed` is the version shown, `equipped` the one the game has (a purple flame under its bar).
    void HdUi::updateWeapons(const std::array<std::uint8_t, 4>& browsed, const std::array<std::uint8_t, 4>& equipped, int row,
        bool page, bool menuUp)
    {
        if (!mLoaded)
            return;
        std::vector<MyGUI::ImageBox*> weapon = { mWHints };
        for (auto* v : { &mWNames, &mWLevels, &mWBars, &mWBarBack, &mWIcons, &mWFlames })
            weapon.insert(weapon.end(), v->begin(), v->end());
        if (!page || !menuUp)
        {
            if (mWeaponsUp)
                for (MyGUI::ImageBox* b : weapon)
                    b->setVisible(false);
            mWeaponsUp = false;
            return;
        }
        // The items give way to the list.
        for (MyGUI::ImageBox* b : { mMap, mOptions, mHints })
            if (b)
                b->setVisible(false);
        for (auto* v : { &mIcons, &mGlows, &mFlames, &mTexts, &mCounts1, &mCounts2 })
            for (MyGUI::ImageBox* b : *v)
                b->setVisible(false);
        const MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
        const float fit = std::min(view.width / kGameW, view.height / kGameH);
        const float ox = (view.width - kGameW * fit) / 2, oy = (view.height - kGameH * fit) / 2;
        auto put = [&](MyGUI::ImageBox* b, float x, float y, float w, float h) {
            b->setCoord(static_cast<int>(std::lround(ox + x * fit)), static_cast<int>(std::lround(oy + y * fit)),
                std::max(1, static_cast<int>(std::lround(w * fit))), std::max(1, static_cast<int>(std::lround(h * fit))));
            b->setVisible(true);
        };
        const auto find = [&](const std::string& key) {
            const auto it = mRects.find(key);
            return it != mRects.end() ? it->second : Rect{};
        };
        for (int i = 0; i < 4; ++i)
        {
            const float alpha = i == row ? 1.0f : 0.5f;
            const Rect n = find("wname_" + std::to_string(i) + ".png");
            put(mWNames[i], n.x, n.y, n.w, n.h);
            const int level = std::clamp<int>(browsed[i], 0, 2) + 1;
            setImage(mWLevels[i], "lv_" + std::to_string(level) + ".png");
            const Rect l = find("lv_" + std::to_string(level) + ".png");
            put(mWLevels[i], l.x, l.y + 76.0f * i, l.w, l.h);
            const float by = 72.0f + 76.0f * i;
            put(mWBarBack[i], 254, by - 2, 284, 14);
            put(mWBars[i], 256, by, 280.0f * level / 3.0f, 8);
            put(mWIcons[i], 18, 34 + 76.0f * i, 36, 16);
            // the version the game has: a purple flame under its bar
            mWFlames[i]->setVisible(false);
            if (browsed[i] == equipped[i])
            {
                put(mWFlames[i], 254, by - 6, 284, 22);
                mWFlames[i]->setAlpha(alpha);
            }
            for (MyGUI::ImageBox* b : { mWNames[i], mWLevels[i], mWBars[i], mWBarBack[i], mWIcons[i] })
                b->setAlpha(alpha);
        }
        const auto it = mRects.find("whints.png");
        if (it != mRects.end())
            put(mWHints, it->second.x, it->second.y, it->second.w, it->second.h);
        mWeaponsUp = true;
    }

    void HdUi::hide()
    {
        if (mWeaponsUp)
            updateWeapons({}, {}, 0, false, false);
        if (!mMenuUp && !mHudUp)
            return;
        std::vector<MyGUI::ImageBox*> all = { mBackdrop, mBg, mMap, mOptions, mHints, mRing, mWeapon, mFrame, mHp, mNinja,
            mSlotTri, mSlotCirc, mSlotTriItem, mSlotCircItem };
        for (auto* v : { &mIcons, &mGlows, &mFlames, &mTexts, &mCounts1, &mCounts2 })
            all.insert(all.end(), v->begin(), v->end());
        for (MyGUI::ImageBox* b : all)
            if (b)
                b->setVisible(false);
        mMenuUp = mHudUp = false;
    }

    void HdUi::update(const kurenai::proto::UiState& ui, float health, bool hud, bool fundo, float hudScale)
    {
        if (!mLoaded)
            return;
        const bool menu = ui.menuOpen && hud;
        if (!menu && !hud)
        {
            hide();
            return;
        }
        const MyGUI::IntSize view = MyGUI::RenderManager::getInstance().getViewSize();
        // The menu fits the window whole, centred; the HUD keeps its corners, as high as on the PS2's screen.
        const float fit = std::min(view.width / kGameW, view.height / kGameH);
        const float menuOx = (view.width - kGameW * fit) / 2, menuOy = (view.height - kGameH * fit) / 2;
        const float hs = view.height / kGameH * std::clamp(hudScale, 0.25f, 4.0f);
        auto place = [&](MyGUI::ImageBox* b, const Rect& r, bool inMenu, bool right, bool bottom) {
            if (!b)
                return;
            const float s = inMenu ? fit : hs;
            float x = inMenu ? menuOx + r.x * s : (right ? view.width - (kGameW - r.x) * s : r.x * s);
            float y = inMenu ? menuOy + r.y * s : (bottom ? view.height - (kGameH - r.y) * s : r.y * s);
            b->setCoord(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                std::max(1, static_cast<int>(std::lround(r.w * s))), std::max(1, static_cast<int>(std::lround(r.h * s))));
            b->setVisible(true);
        };
        auto rect = [&](const char* key) {
            const auto it = mRects.find(key);
            return it != mRects.end() ? it->second : Rect{};
        };
        // item icon on a slot (triangle / circle): centred, a bit under half the icon's menu size
        auto slotItem = [&](MyGUI::ImageBox* b, const Rect& slot, std::uint8_t item, bool inMenu, bool right, bool bottom) {
            if (item >= mItems.size())
            {
                b->setVisible(false);
                return;
            }
            const ItemInfo& it = mItems[item];
            const float k = 0.5f;
            Rect r{ slot.x + slot.w / 2 - it.iw * k / 2, slot.y + slot.h / 2 - it.ih * k / 2 + 3,
                it.iw * k, it.ih * k };
            setImage(b, itemFile("item", item));
            place(b, r, inMenu, right, bottom);
        };

        std::vector<MyGUI::ImageBox*> menuBoxes = { mBackdrop, mBg, mMap, mOptions, mHints };
        for (auto* v : { &mIcons, &mGlows, &mFlames, &mTexts, &mCounts1, &mCounts2 })
            menuBoxes.insert(menuBoxes.end(), v->begin(), v->end());
        if (menu)
        {
            mBackdrop->setCoord(0, 0, view.width, view.height);
            mBackdrop->setVisible(true);
            mBackdrop->setAlpha(1);
            place(mBg, Rect{ 0, 0, kGameW, kGameH }, true, false, false);
            for (std::size_t i = 0; i < mItems.size(); ++i)
            {
                const ItemInfo& it = mItems[i];
                const bool owned = (ui.owned >> i) & 1;
                place(mIcons[i], Rect{ it.cx - it.iw / 2, it.cy - it.ih / 2, it.iw, it.ih }, true, false, false);
                mIcons[i]->setAlpha(owned ? 1.0f : 0.28f);
                mGlows[i]->setVisible(false);
                mTexts[i]->setVisible(false);
                // purple flame: worn equipment, and what is on the Triangle / Circle buttons
                mFlames[i]->setVisible(false);
                if (owned && (((ui.equipMask >> i) & 1) || ui.slotTriangle == i || ui.slotCircle == i))
                    place(mFlames[i], Rect{ it.cx - it.gw / 2, it.cy - it.gh / 2, it.gw, it.gh }, true, false, false);
                if (i == ui.cursor)
                {
                    place(mGlows[i], Rect{ it.cx - it.gw / 2, it.cy - it.gh / 2, it.gw, it.gh }, true, false, false);
                    place(mTexts[i], Rect{ it.tx, it.ty, it.tw, it.th }, true, false, false);
                }
                // consumables show their count
                for (auto* b : { mCounts1[i], mCounts2[i] })
                    b->setVisible(false);
                if (owned && i >= 1 && i <= 7)
                {
                    const int c = ui.counts[i];
                    float x = it.cx + 6;
                    const float y = it.cy + 10;
                    const std::string digits = std::to_string(c);
                    MyGUI::ImageBox* bs[2] = { mCounts1[i], mCounts2[i] };
                    for (std::size_t d = 0; d < digits.size() && d < 2; ++d)
                    {
                        const std::string f = std::string("digit_") + digits[d] + ".png";
                        setImage(bs[d], f);
                        const Image& im = mImages[f];
                        const float w = im.w / float(kPicScale), h = im.h / float(kPicScale);
                        place(bs[d], Rect{ x, y, w, h }, true, false, false);
                        x += w + 0.3f;
                    }
                }
            }
            place(mMap, rect("label_map.png"), true, false, false);
            place(mOptions, rect("label_options.png"), true, false, false);
            place(mHints, rect("hints.png"), true, false, false);
        }
        else if (mMenuUp)
        {
            for (MyGUI::ImageBox* b : menuBoxes)
                b->setVisible(false);
            for (MyGUI::ImageBox* b : mFlames)
                b->setVisible(false);
        }

        // the HUD (also under the menu: the ring and bars are part of the menu's picture, drawn at the menu's scale)
        const bool inMenu = menu;
        place(mRing, rect("ring.png"), inMenu, false, true);
        setImage(mWeapon, fundo ? "weapon_a.png" : "weapon_b.png"); // the pack lists the Fundo first
        place(mWeapon, rect("weapon_a.png"), inMenu, false, true);
        const Rect frame{ 67, 441, 378, 18 };
        place(mFrame, frame, inMenu, false, true);
        place(mHp, Rect{ 68, 443, std::max(0.0f, 375 * std::clamp(health, 0.0f, 1.0f)), 6 }, inMenu, false, true);
        place(mNinja, Rect{ 68, 450, std::max(0.0f, 375 * std::clamp(ui.ninjutsu, 0.0f, 1.0f)), 6 }, inMenu, false, true);
        mHp->setVisible(health > 0.002f);
        mNinja->setVisible(ui.ninjutsu > 0.002f);
        place(mSlotTri, rect("slot_tri.png"), inMenu, true, false);
        place(mSlotCirc, rect("slot_circ.png"), inMenu, true, false);
        slotItem(mSlotTriItem, rect("slot_tri.png"), ui.slotTriangle, inMenu, true, false);
        slotItem(mSlotCircItem, rect("slot_circ.png"), ui.slotCircle, inMenu, true, false);
        mMenuUp = menu;
        mHudUp = true;
    }
}
