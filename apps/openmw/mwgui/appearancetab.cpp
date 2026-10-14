#include "appearancetab.hpp"

#include <algorithm>
#include <string>

#include <MyGUI_Button.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_TabItem.h>
#include <MyGUI_TextBox.h>

#include <osg/Texture2D>

#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/myguiplatform/myguitexture.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwrender/characterpreview.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "inventorywindow.hpp"

namespace MWGui
{
    namespace
    {
        constexpr int sColumn = 340; // width of the slider column; the preview takes the rest of the tab
        struct ViewChoice
        {
            const char* mName;
            MWRender::JigglePreview::View mView;
        };
        constexpr ViewChoice sViews[] = {
            { "Face", MWRender::JigglePreview::View::Face },
            { "Front", MWRender::JigglePreview::View::Front },
            { "Side", MWRender::JigglePreview::View::Side },
            { "Back", MWRender::JigglePreview::View::Back },
        };
    }

    AppearanceTab::AppearanceTab(MyGUI::TabItem* tab, osg::Group* sceneRoot, Resource::ResourceSystem* resourceSystem)
        : mTab(tab)
        , mSceneRoot(sceneRoot)
        , mResourceSystem(resourceSystem)
    {
        mHeadLabel = mTab->createWidget<MyGUI::TextBox>(
            "NormalText", MyGUI::IntCoord(6, 8, sColumn - 12, 18), MyGUI::Align::Left | MyGUI::Align::Top);
        mHeadSlider = mTab->createWidget<MyGUI::ScrollBar>(
            "MW_HScroll", MyGUI::IntCoord(6, 28, sColumn - 12, 18), MyGUI::Align::Left | MyGUI::Align::Top);
        mHairLabel = mTab->createWidget<MyGUI::TextBox>(
            "NormalText", MyGUI::IntCoord(6, 60, sColumn - 12, 18), MyGUI::Align::Left | MyGUI::Align::Top);
        mHairSlider = mTab->createWidget<MyGUI::ScrollBar>(
            "MW_HScroll", MyGUI::IntCoord(6, 80, sColumn - 12, 18), MyGUI::Align::Left | MyGUI::Align::Top);
        mNote = mTab->createWidget<MyGUI::TextBox>(
            "SandText", MyGUI::IntCoord(6, 112, sColumn - 12, 120), MyGUI::Align::Left | MyGUI::Align::Top);
        mNote->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
        mNote->setCaption(
            "Every head and hair your race and sex\ncan have. The change applies at once\nand is kept in your save.");

        mPreviewFrame = mTab->createWidget<MyGUI::Widget>("", MyGUI::IntCoord(sColumn, 2, 1, 1), MyGUI::Align::Default);
        mPreviewImage = mPreviewFrame->createWidget<MyGUI::ImageBox>(
            "ImageBox", MyGUI::IntCoord(0, 0, 1, 1), MyGUI::Align::Default);
        mViewButton = mPreviewFrame->createWidget<MyGUI::Button>(
            "MW_Button", MyGUI::IntCoord(0, 0, 1, 1), MyGUI::Align::Default);

        for (MyGUI::ScrollBar* slider : { mHeadSlider, mHairSlider })
        {
            slider->setScrollPage(1);
            slider->setScrollViewPage(1);
            slider->setScrollWheelPage(1);
        }
        mHeadSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AppearanceTab::onHeadScroll);
        mHairSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &AppearanceTab::onHairScroll);
        mViewButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AppearanceTab::onViewClicked);
        updateViewCaption();
    }

    AppearanceTab::~AppearanceTab()
    {
        destroyPreview();
    }

    void AppearanceTab::setShown(bool shown)
    {
        if (shown == mShown)
            return;
        mShown = shown;
        if (!shown)
        {
            destroyPreview();
            return;
        }
        collectParts();
        auto setup = [](MyGUI::ScrollBar* slider, const std::vector<ESM::RefId>& list, int index) {
            slider->setScrollRange(std::max<size_t>(1, list.size()));
            slider->setScrollPosition(std::max(index, 0));
            slider->setEnabled(list.size() > 1);
        };
        setup(mHeadSlider, mHeads, mHeadIndex);
        setup(mHairSlider, mHairs, mHairIndex);
        updateLabels();
        layout();

        destroyPreview();
        MWBase::World* world = MWBase::Environment::get().getWorld();
        mPreview = std::make_unique<MWRender::JigglePreview>(mSceneRoot, mResourceSystem, world->getPlayerPtr(), true);
        mPreviewTexture
            = std::make_unique<MyGUIPlatform::OSGTexture>(mPreview->getTexture(), mPreview->getTextureStateSet());
        mPreviewImage->setRenderItemTexture(mPreviewTexture.get());
        mPreviewImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));
        mPreview->rebuild();
        mPreview->setView(sViews[mView].mView);
        mPreview->setMotion(MWRender::JigglePreview::Motion::Still);
    }

    void AppearanceTab::layout()
    {
        const MyGUI::IntSize tab = mTab->getSize();
        const int width = std::max(1, tab.width - sColumn - 4);
        const int height = std::max(1, tab.height - 6);
        mPreviewFrame->setCoord(sColumn, 2, width, height);
        constexpr int buttonH = 26, gap = 4;
        const int imageH = std::max(1, height - buttonH - gap);
        // The render is 2:3; keep that, centred.
        const int imageW = std::max(1, std::min(width, imageH * 2 / 3));
        const int fittedH = std::max(1, imageW * 3 / 2);
        mPreviewImage->setCoord((width - imageW) / 2, (imageH - fittedH) / 2, imageW, fittedH);
        mViewButton->setCoord(0, height - buttonH, width, buttonH);
        mPreviewFrame->setVisible(width >= 80);
    }

    void AppearanceTab::onFrame(float dt)
    {
        if (mShown && mPreview)
            mPreview->tick(dt);
    }

    void AppearanceTab::destroyPreview()
    {
        if (mPreviewImage)
            mPreviewImage->setRenderItemTexture(nullptr);
        mPreview.reset();
        mPreviewTexture.reset();
    }

    void AppearanceTab::updateViewCaption()
    {
        mViewButton->setCaption(std::string("View: ") + sViews[mView].mName);
    }

    void AppearanceTab::onViewClicked(MyGUI::Widget* /*sender*/)
    {
        mView = (mView + 1) % static_cast<int>(std::size(sViews));
        updateViewCaption();
        if (mPreview)
            mPreview->setView(sViews[mView].mView);
    }

    void AppearanceTab::collectParts()
    {
        mHeads.clear();
        mHairs.clear();
        mHeadIndex = -1;
        mHairIndex = -1;

        const MWWorld::Ptr player = MWMechanics::getPlayer();
        const ESM::NPC* npc = player.get<ESM::NPC>()->mBase;
        const bool wantFemale = !npc->isMale();
        const MWWorld::Store<ESM::BodyPart>& store = MWBase::Environment::get().getESMStore()->get<ESM::BodyPart>();

        auto collect = [&](ESM::BodyPart::MeshPart part, std::vector<ESM::RefId>& out) {
            for (const ESM::BodyPart& bodypart : store)
            {
                if (bodypart.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                    continue;
                if (bodypart.mData.mType != ESM::BodyPart::MT_Skin || bodypart.mData.mPart != part)
                    continue;
                if (((bodypart.mData.mFlags & ESM::BodyPart::BPF_Female) != 0) != wantFemale)
                    continue;
                if (ESM::isFirstPersonBodyPart(bodypart))
                    continue;
                if (bodypart.mRace == npc->mRace)
                    out.push_back(bodypart.mId);
            }
        };
        collect(ESM::BodyPart::MP_Head, mHeads);
        collect(ESM::BodyPart::MP_Hair, mHairs);

        for (size_t i = 0; i < mHeads.size(); ++i)
            if (mHeads[i] == npc->mHead)
                mHeadIndex = static_cast<int>(i);
        for (size_t i = 0; i < mHairs.size(); ++i)
            if (mHairs[i] == npc->mHair)
                mHairIndex = static_cast<int>(i);
        if (mHeadIndex < 0)
            mHeadIndex = mHeads.empty() ? -1 : 0;
        if (mHairIndex < 0)
            mHairIndex = mHairs.empty() ? -1 : 0;
    }

    void AppearanceTab::updateLabels()
    {
        auto& lang = MyGUI::LanguageManager::getInstance();
        auto label
            = [&](MyGUI::TextBox* widget, const std::string& tag, const std::vector<ESM::RefId>& list, int index) {
                  std::string text = lang.replaceTags(tag);
                  if (list.empty())
                      text += " (0/0)";
                  else
                      text += " (" + std::to_string(index + 1) + "/" + std::to_string(list.size()) + ")";
                  widget->setCaption(text);
              };
        label(mHeadLabel, "#{OMWEngine:AppearanceHead}", mHeads, mHeadIndex);
        label(mHairLabel, "#{OMWEngine:AppearanceHair}", mHairs, mHairIndex);
    }

    void AppearanceTab::apply()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::NPC record = *world->getPlayerPtr().get<ESM::NPC>()->mBase;
        const ESM::RefId head
            = (mHeadIndex >= 0 && static_cast<size_t>(mHeadIndex) < mHeads.size()) ? mHeads[mHeadIndex] : record.mHead;
        const ESM::RefId hair
            = (mHairIndex >= 0 && static_cast<size_t>(mHairIndex) < mHairs.size()) ? mHairs[mHairIndex] : record.mHair;
        if (record.mHead == head && record.mHair == hair)
            return;
        record.mHead = head;
        record.mHair = hair;
        // A dynamic override of the player's NPC record: for a post-chargen player the record is dynamic and
        // insert() assigns it in place, so the live record the animations read now carries the new head/hair.
        world->getStore().insert(record);
        // Rebuild the player in the world (safe while menus pause world updates), the inventory paperdoll and
        // this tab's preview.
        world->reattachPlayerCamera();
        if (InventoryWindow* inventory = MWBase::Environment::get().getWindowManager()->getInventoryWindow())
            inventory->rebuildAvatar();
        if (mPreview)
        {
            mPreview->rebuild();
            mPreview->setView(sViews[mView].mView);
            mPreview->setMotion(MWRender::JigglePreview::Motion::Still);
        }
    }

    void AppearanceTab::onHeadScroll(MyGUI::ScrollBar* /*sender*/, size_t pos)
    {
        if (mHeads.empty())
            return;
        mHeadIndex = std::min(static_cast<int>(pos), static_cast<int>(mHeads.size()) - 1);
        apply();
        updateLabels();
    }

    void AppearanceTab::onHairScroll(MyGUI::ScrollBar* /*sender*/, size_t pos)
    {
        if (mHairs.empty())
            return;
        mHairIndex = std::min(static_cast<int>(pos), static_cast<int>(mHairs.size()) - 1);
        apply();
        updateLabels();
    }
}
