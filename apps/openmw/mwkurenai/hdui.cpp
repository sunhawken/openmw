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
        for (std::size_t i = 0; ok && i < kurenai::proto::kUiItems; ++i)
            ok = loadImage(dir, itemFile("item", i)) && loadImage(dir, itemFile("glow", i)) && loadImage(dir, itemFile("text", i));
        for (char d = '0'; ok && d <= '9'; ++d)
            ok = loadImage(dir, std::string("digit_") + d + ".png");
        for (const char* f : { "label_map.png", "label_options.png", "hints.png", "ring.png", "weapon_a.png", "slot_tri.png",
                 "slot_circ.png", "backdrop.png", "bar_orange.png", "bar_navy.png", "bar_frame.png" })
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
        mLoaded = true;
        Log(Debug::Info) << "Kurenai: hd ui loaded from " << dir;
        return true;
    }

    void HdUi::hide()
    {
        if (!mMenuUp && !mHudUp)
            return;
        std::vector<MyGUI::ImageBox*> all = { mBackdrop, mBg, mMap, mOptions, mHints, mRing, mWeapon, mFrame, mHp, mNinja,
            mSlotTri, mSlotCirc, mSlotTriItem, mSlotCircItem };
        for (auto* v : { &mIcons, &mGlows, &mTexts, &mCounts1, &mCounts2 })
            all.insert(all.end(), v->begin(), v->end());
        for (MyGUI::ImageBox* b : all)
            if (b)
                b->setVisible(false);
        mMenuUp = mHudUp = false;
    }

    void HdUi::update(const kurenai::proto::UiState& ui, float health, bool hud)
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
        const float hs = view.height / kGameH;
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
        for (auto* v : { &mIcons, &mGlows, &mTexts, &mCounts1, &mCounts2 })
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
            for (MyGUI::ImageBox* b : menuBoxes)
                b->setVisible(false);

        // the HUD (also under the menu: the ring and bars are part of the menu's picture, drawn at the menu's scale)
        const bool inMenu = menu;
        place(mRing, rect("ring.png"), inMenu, false, true);
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
