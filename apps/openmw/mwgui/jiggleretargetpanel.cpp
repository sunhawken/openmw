#include "jiggleretargetpanel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include <components/misc/jiggleanchors.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwrender/animation.hpp"
#include "../mwrender/characterpreview.hpp"
#include "../mwworld/ptr.hpp"

#include "windowbase.hpp"

namespace MWGui
{
    namespace
    {
        constexpr std::size_t sBreastLeft = 0, sBreastRight = 1, sButtLeft = 2, sButtRight = 3;

        bool isFrontMarker(std::size_t index)
        {
            return index < 2;
        }

        bool isLeftMarker(std::size_t index)
        {
            return index == sBreastLeft || index == sButtLeft;
        }
    }

    JiggleRetargetPanel::JiggleRetargetPanel(osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem)
        : Layout("openmw_jiggle_retarget.layout")
        , mSceneRoot(sceneRoot)
        , mResourceSystem(resourceSystem)
    {
        mWindow = mMainWidget->castType<MyGUI::Window>();
        getWidget(mFrontView, "FrontView");
        getWidget(mBackView, "BackView");
        getWidget(mFrontLive, "FrontLive");
        getWidget(mBackLive, "BackLive");
        getWidget(mStatus, "RetargetStatus");
        getWidget(mWalkButton, "RetargetWalkButton");
        getWidget(mMarkers[sBreastLeft], "MarkBreastL");
        getWidget(mMarkers[sBreastRight], "MarkBreastR");
        getWidget(mMarkers[sButtLeft], "MarkButtL");
        getWidget(mMarkers[sButtRight], "MarkButtR");

        for (MyGUI::Button* marker : mMarkers)
            marker->eventMouseDrag += MyGUI::newDelegate(this, &JiggleRetargetPanel::onMarkerDrag);

        MyGUI::Button* button;
        getWidget(button, "RetargetAutoButton");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &JiggleRetargetPanel::onAutoClicked);
        getWidget(button, "RetargetSaveButton");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &JiggleRetargetPanel::onSaveClicked);
        getWidget(button, "RetargetForgetButton");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &JiggleRetargetPanel::onForgetClicked);
        getWidget(button, "RetargetCloseButton");
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &JiggleRetargetPanel::onCloseClicked);
        mWalkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &JiggleRetargetPanel::onWalkClicked);

        mFrontLive->setVisible(false);
        mBackLive->setVisible(false);
        mMainWidget->setVisible(false);
    }

    JiggleRetargetPanel::~JiggleRetargetPanel()
    {
        destroyPreviews();
        if (mFrontTexture)
            MyGUI::RenderManager::getInstance().destroyTexture(mFrontTexture);
        if (mBackTexture)
            MyGUI::RenderManager::getInstance().destroyTexture(mBackTexture);
    }

    bool JiggleRetargetPanel::isVisible() const
    {
        return mMainWidget->getVisible();
    }

    void JiggleRetargetPanel::setVisible(bool visible)
    {
        if (visible == isVisible())
            return;
        if (visible)
            open();
        else
            close();
    }

    void JiggleRetargetPanel::open()
    {
        mWalking = false;
        mWalkButton->setCaption("Walking Animation: Off");
        mFrontView->setVisible(true);
        mBackView->setVisible(true);
        mFrontLive->setVisible(false);
        mBackLive->setVisible(false);
        mMainWidget->setVisible(true);
        WindowBase::clampWindowCoordinates(mWindow);
        refresh();
    }

    void JiggleRetargetPanel::close()
    {
        destroyPreviews();
        mMainWidget->setVisible(false);
    }

    void JiggleRetargetPanel::onFrame(float dt)
    {
        if (!isVisible() || !mWalking)
            return;
        if (mFrontPreview)
            mFrontPreview->tick(dt);
        if (mBackPreview)
            mBackPreview->tick(dt);
    }

    MyGUI::IntPoint JiggleRetargetPanel::toPixel(bool front, float y, float z) const
    {
        // Seen from the front the character's left (+y) is on the viewer's right; from behind it is on the left.
        const float px = sViewWidth * 0.5f + (front ? 1.f : -1.f) * (y - mYCenter) * mScale;
        const float py = sViewHeight * 0.5f - (z - mZCenter) * mScale;
        return MyGUI::IntPoint(static_cast<int>(std::lround(px)), static_cast<int>(std::lround(py)));
    }

    osg::Vec2f JiggleRetargetPanel::toMesh(bool front, const MyGUI::IntPoint& center) const
    {
        const float y = mYCenter + (front ? 1.f : -1.f) * (center.left - sViewWidth * 0.5f) / mScale;
        const float z = mZCenter - (center.top - sViewHeight * 0.5f) / mScale;
        return osg::Vec2f(y, z);
    }

    void JiggleRetargetPanel::placeMarker(std::size_t index, const osg::Vec2f& yz)
    {
        const MyGUI::IntPoint p = toPixel(isFrontMarker(index), yz.x(), yz.y());
        const int half = sMarkerSize / 2;
        const int x = std::clamp(p.left - half, 0, sViewWidth - sMarkerSize);
        const int y = std::clamp(p.top - half, 0, sViewHeight - sMarkerSize);
        mMarkers[index]->setPosition(x, y);
    }

    osg::Vec2f JiggleRetargetPanel::markerYZ(std::size_t index) const
    {
        const MyGUI::IntPoint pos = mMarkers[index]->getPosition();
        return toMesh(isFrontMarker(index), MyGUI::IntPoint(pos.left + sMarkerSize / 2, pos.top + sMarkerSize / 2));
    }

    void JiggleRetargetPanel::moveMarkerTo(std::size_t index, int absX, int absY)
    {
        MyGUI::Widget* parent = mMarkers[index]->getParent();
        const MyGUI::IntPoint origin = parent->getAbsolutePosition();
        const int half = sMarkerSize / 2;
        const int x = std::clamp(absX - origin.left - half, 0, sViewWidth - sMarkerSize);
        const int y = std::clamp(absY - origin.top - half, 0, sViewHeight - sMarkerSize);
        mMarkers[index]->setPosition(x, y);
    }

    void JiggleRetargetPanel::onMarkerDrag(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton)
    {
        for (std::size_t i = 0; i < mMarkers.size(); ++i)
            if (mMarkers[i] == sender)
                moveMarkerTo(i, left, top);
    }

    void JiggleRetargetPanel::placeAllMarkers(bool preferAuto)
    {
        const float ySpan = std::max(0.001f, mPreview.mYMax - mPreview.mYMin);
        const float zSpan = std::max(0.001f, mPreview.mZMax - mPreview.mZMin);
        for (std::size_t i = 0; i < mMarkers.size(); ++i)
        {
            const bool left = isLeftMarker(i);
            const bool breast = isFrontMarker(i);
            std::optional<osg::Vec2f> yz;
            if (!preferAuto)
                yz = mPreview.mSaved[i];
            if (!yz)
                yz = mPreview.mAuto[i];
            if (!yz) // no detection: a sensible guess from the outfit's size
                yz = osg::Vec2f((left ? 1.f : -1.f) * ySpan * 0.07f, mPreview.mZMin + zSpan * (breast ? 0.72f : 0.5f));
            placeMarker(i, *yz);
        }
    }

    void JiggleRetargetPanel::renderView(bool front, std::vector<unsigned char>& rgba) const
    {
        const int w = sViewWidth, h = sViewHeight;
        rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
        for (std::size_t i = 0; i < rgba.size(); i += 4)
        {
            rgba[i] = 22;
            rgba[i + 1] = 25;
            rgba[i + 2] = 31;
            rgba[i + 3] = 255;
        }
        if (!mHasPreview || mWalking) // walking: a plain dark backdrop behind the live views
            return;
        std::vector<float> depth(static_cast<std::size_t>(w) * h, -std::numeric_limits<float>::max());
        const float view = front ? 1.f : -1.f; // the viewer sits on +x (front) or -x (back)

        for (std::size_t t = 0; t + 2 < mPreview.mVertices.size(); t += 3)
        {
            float px[3], py[3], pd[3];
            for (int k = 0; k < 3; ++k)
            {
                const osg::Vec3f& v = mPreview.mVertices[t + k];
                const MyGUI::IntPoint unused;
                (void)unused;
                px[k] = w * 0.5f + (front ? 1.f : -1.f) * (v.y() - mYCenter) * mScale;
                py[k] = h * 0.5f - (v.z() - mZCenter) * mScale;
                pd[k] = view * v.x();
            }
            const float minX = std::floor(std::min({ px[0], px[1], px[2] }));
            const float maxX = std::ceil(std::max({ px[0], px[1], px[2] }));
            const float minY = std::floor(std::min({ py[0], py[1], py[2] }));
            const float maxY = std::ceil(std::max({ py[0], py[1], py[2] }));
            const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
            if (std::abs(area) < 1e-6f)
                continue;
            const int x0 = std::max(0, static_cast<int>(minX)), x1 = std::min(w - 1, static_cast<int>(maxX));
            const int y0 = std::max(0, static_cast<int>(minY)), y1 = std::min(h - 1, static_cast<int>(maxY));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float cx = x + 0.5f, cy = y + 0.5f;
                    const float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                    const float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                    const float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f)
                        continue;
                    const float d = w0 * pd[0] + w1 * pd[1] + w2 * pd[2];
                    float& stored = depth[static_cast<std::size_t>(y) * w + x];
                    if (d <= stored)
                        continue;
                    stored = d;
                    osg::Vec3f n = mPreview.mNormals[t] * w0 + mPreview.mNormals[t + 1] * w1
                        + mPreview.mNormals[t + 2] * w2;
                    n.normalize();
                    // light from the viewer, slightly from above, so curves read as shape
                    const float lit = std::abs(n.x() * view * 0.85f + n.z() * 0.25f + n.y() * 0.1f);
                    const float shade = 0.28f + 0.72f * std::min(1.f, lit);
                    unsigned char* px4 = &rgba[(static_cast<std::size_t>(y) * w + x) * 4];
                    px4[0] = static_cast<unsigned char>(std::min(255.f, 214.f * shade));
                    px4[1] = static_cast<unsigned char>(std::min(255.f, 196.f * shade));
                    px4[2] = static_cast<unsigned char>(std::min(255.f, 178.f * shade));
                    px4[3] = 255;
                }
        }
    }

    void JiggleRetargetPanel::uploadView(
        const char* textureName, MyGUI::ImageBox* box, const std::vector<unsigned char>& rgba)
    {
        // The GL texture must be a power of two or it gets rescaled under us; the view lives in its corner.
        constexpr int texSize = 512;
        MyGUI::ITexture*& tex = (box == mFrontView) ? mFrontTexture : mBackTexture;
        if (!tex)
        {
            tex = MyGUI::RenderManager::getInstance().createTexture(textureName);
            tex->createManual(texSize, texSize, MyGUI::TextureUsage::Write, MyGUI::PixelFormat::R8G8B8A8);
        }
        unsigned char* data = reinterpret_cast<unsigned char*>(tex->lock(MyGUI::TextureUsage::Write));
        if (data)
            for (int y = 0; y < sViewHeight; ++y)
                std::copy_n(&rgba[static_cast<std::size_t>(y) * sViewWidth * 4], sViewWidth * 4,
                    data + static_cast<std::size_t>(y) * texSize * 4);
        tex->unlock();
        box->setImageTexture(textureName);
        box->setImageCoord(MyGUI::IntCoord(0, 0, sViewWidth, sViewHeight));
        box->setImageTile(MyGUI::IntSize(sViewWidth, sViewHeight));
    }

    void JiggleRetargetPanel::refresh()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        MWWorld::Ptr player = world->getPlayerPtr();
        MWRender::Animation* animation = world->getAnimation(player);
        mHasPreview = animation && MWRender::JiggleAutoRig::buildRetargetPreview(animation->getObjectRoot(), mPreview);
        if (mHasPreview)
        {
            const float ySpan = std::max(1.f, mPreview.mYMax - mPreview.mYMin);
            const float zSpan = std::max(1.f, mPreview.mZMax - mPreview.mZMin);
            mScale = std::min((sViewWidth - 24.f) / ySpan, (sViewHeight - 24.f) / zSpan);
            mYCenter = (mPreview.mYMin + mPreview.mYMax) * 0.5f;
            mZCenter = (mPreview.mZMin + mPreview.mZMax) * 0.5f;
        }
        std::vector<unsigned char> rgba;
        renderView(true, rgba);
        uploadView("JiggleRetargetFront", mFrontView, rgba);
        renderView(false, rgba);
        uploadView("JiggleRetargetBack", mBackView, rgba);
        for (MyGUI::Button* marker : mMarkers)
            marker->setVisible(mHasPreview && !mWalking);
        if (mHasPreview)
            placeAllMarkers(false);
        updateStatus();
        if (mWalking)
        {
            destroyPreviews();
            createPreviews();
        }
    }

    void JiggleRetargetPanel::updateStatus()
    {
        if (!mHasPreview)
        {
            mStatus->setCaption("Put on a body, clothing or armor first: nothing to show yet.");
            return;
        }
        auto shortName = [](const std::string& path) {
            const std::size_t slash = path.find_last_of('/');
            return slash == std::string::npos ? path : path.substr(slash + 1);
        };
        const bool savedBreast = mPreview.mSaved[sBreastLeft].has_value();
        const bool savedButt = mPreview.mSaved[sButtLeft].has_value();
        mStatus->setCaption("Outfit: " + shortName(mPreview.mChestMesh) + " / " + shortName(mPreview.mPelvisMesh)
            + "   Saved - breasts: " + (savedBreast ? "yes" : "no") + ", butt: " + (savedButt ? "yes" : "no"));
    }

    void JiggleRetargetPanel::createPreviews()
    {
        MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        mFrontPreview = std::make_unique<MWRender::JigglePreview>(mSceneRoot, mResourceSystem, player, true);
        mBackPreview = std::make_unique<MWRender::JigglePreview>(mSceneRoot, mResourceSystem, player, false);
        mFrontPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
            mFrontPreview->getTexture(), mFrontPreview->getTextureStateSet());
        mBackPreviewTexture = std::make_unique<MyGUIPlatform::OSGTexture>(
            mBackPreview->getTexture(), mBackPreview->getTextureStateSet());
        mFrontLive->setRenderItemTexture(mFrontPreviewTexture.get());
        mFrontLive->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
        mBackLive->setRenderItemTexture(mBackPreviewTexture.get());
        mBackLive->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
        mFrontPreview->rebuild();
        mBackPreview->rebuild();
        mFrontPreview->setWalking(mWalking);
        mBackPreview->setWalking(mWalking);
    }

    void JiggleRetargetPanel::destroyPreviews()
    {
        mFrontLive->setRenderItemTexture(nullptr);
        mBackLive->setRenderItemTexture(nullptr);
        mFrontPreview.reset();
        mBackPreview.reset();
        mFrontPreviewTexture.reset();
        mBackPreviewTexture.reset();
    }

    void JiggleRetargetPanel::setWalking(bool walking)
    {
        mWalking = walking;
        mWalkButton->setCaption(walking ? "Walking Animation: On" : "Walking Animation: Off");
        mFrontLive->setVisible(walking);
        mBackLive->setVisible(walking);
        for (MyGUI::Button* marker : mMarkers)
            marker->setVisible(mHasPreview && !walking);
        destroyPreviews();
        std::vector<unsigned char> rgba;
        renderView(true, rgba);
        uploadView("JiggleRetargetFront", mFrontView, rgba);
        renderView(false, rgba);
        uploadView("JiggleRetargetBack", mBackView, rgba);
        if (walking)
            createPreviews();
    }

    void JiggleRetargetPanel::onWalkClicked(MyGUI::Widget*)
    {
        setWalking(!mWalking);
    }

    void JiggleRetargetPanel::onAutoClicked(MyGUI::Widget*)
    {
        if (mHasPreview)
            placeAllMarkers(true);
    }

    void JiggleRetargetPanel::applyToPlayer()
    {
        // Re-run the auto-rigger on the player so the new anchors take effect right away.
        Settings::game().mJiggleAutoRig.set(true);
        Settings::game().mJigglePlayerEnabled.set(true);
        MWBase::Environment::get().getWorld()->renderPlayer();
        refresh();
    }

    void JiggleRetargetPanel::onSaveClicked(MyGUI::Widget*)
    {
        if (!mHasPreview || mPreview.mChestMesh.empty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("Put on a body, clothing or armor first.");
            return;
        }
        Misc::JiggleAnchors::Pair breast{ markerYZ(sBreastLeft).x(), markerYZ(sBreastLeft).y(),
            markerYZ(sBreastRight).x(), markerYZ(sBreastRight).y() };
        Misc::JiggleAnchors::save('b', mPreview.mChestMesh, breast);
        if (!mPreview.mPelvisMesh.empty())
        {
            Misc::JiggleAnchors::Pair butt{ markerYZ(sButtLeft).x(), markerYZ(sButtLeft).y(),
                markerYZ(sButtRight).x(), markerYZ(sButtRight).y() };
            Misc::JiggleAnchors::save('u', mPreview.mPelvisMesh, butt);
        }
        applyToPlayer();
        MWBase::Environment::get().getWindowManager()->messageBox("Saved for this outfit.");
    }

    void JiggleRetargetPanel::onForgetClicked(MyGUI::Widget*)
    {
        if (!mHasPreview)
            return;
        Misc::JiggleAnchors::reset('b', mPreview.mChestMesh);
        Misc::JiggleAnchors::reset('u', mPreview.mPelvisMesh);
        applyToPlayer();
        MWBase::Environment::get().getWindowManager()->messageBox("Saved positions removed for this outfit.");
    }

    void JiggleRetargetPanel::onCloseClicked(MyGUI::Widget*)
    {
        close();
    }
}
