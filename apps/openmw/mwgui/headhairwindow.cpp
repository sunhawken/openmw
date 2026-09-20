#include "headhairwindow.hpp"

#include <algorithm>
#include <string>

#include <MyGUI_LanguageManager.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_TextBox.h>

#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadnpc.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "inventorywindow.hpp"

namespace MWGui
{
    HeadHairWindow::HeadHairWindow(InventoryWindow* inventoryWindow)
        : WindowBase("openmw_head_hair_window.layout")
        , mInventoryWindow(inventoryWindow)
        , mHeadSlider(nullptr)
        , mHairSlider(nullptr)
        , mHeadLabel(nullptr)
        , mHairLabel(nullptr)
        , mHeadIndex(0)
        , mHairIndex(0)
    {
        getWidget(mHeadLabel, "HeadLabel");
        getWidget(mHairLabel, "HairLabel");
        getWidget(mHeadSlider, "HeadSlider");
        getWidget(mHairSlider, "HairSlider");

        mHeadSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &HeadHairWindow::onHeadScroll);
        mHairSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &HeadHairWindow::onHairScroll);
    }

    void HeadHairWindow::onOpen()
    {
        collectParts();

        auto setupSlider = [](MyGUI::ScrollBar* slider, const std::vector<ESM::RefId>& list, int index) {
            const size_t count = std::max<size_t>(1, list.size());
            slider->setScrollRange(count);
            slider->setScrollPage(1);
            slider->setScrollViewPage(1);
            slider->setScrollPosition(index >= 0 && static_cast<size_t>(index) < list.size() ? index : 0);
            slider->setEnabled(list.size() > 1);
        };
        setupSlider(mHeadSlider, mHeads, mHeadIndex);
        setupSlider(mHairSlider, mHairs, mHairIndex);

        updateLabels();
    }

    void HeadHairWindow::collectParts()
    {
        mHeads.clear();
        mHairs.clear();
        mHeadIndex = -1;
        mHairIndex = -1;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        const ESM::NPC* npc = player.get<ESM::NPC>()->mBase;
        const ESM::RefId race = npc->mRace;
        const bool wantFemale = !npc->isMale();

        const MWWorld::Store<ESM::BodyPart>& store
            = MWBase::Environment::get().getESMStore()->get<ESM::BodyPart>();

        auto collect = [&](ESM::BodyPart::MeshPart part, std::vector<ESM::RefId>& out) {
            for (const ESM::BodyPart& bodypart : store)
            {
                if (bodypart.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                    continue;
                if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                    continue;
                if (bodypart.mData.mPart != part)
                    continue;
                const bool isFemale = (bodypart.mData.mFlags & ESM::BodyPart::BPF_Female) != 0;
                if (isFemale != wantFemale)
                    continue;
                if (ESM::isFirstPersonBodyPart(bodypart))
                    continue;
                if (bodypart.mRace == race)
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

    void HeadHairWindow::updateLabels()
    {
        auto& lang = MyGUI::LanguageManager::getInstance();
        auto label = [&](MyGUI::TextBox* widget, const std::string& tag, const std::vector<ESM::RefId>& list,
                         int index) {
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

    void HeadHairWindow::apply()
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::NPC record = *world->getPlayerPtr().get<ESM::NPC>()->mBase;

        const ESM::RefId head = (mHeadIndex >= 0 && static_cast<size_t>(mHeadIndex) < mHeads.size())
            ? mHeads[mHeadIndex]
            : record.mHead;
        const ESM::RefId hair = (mHairIndex >= 0 && static_cast<size_t>(mHairIndex) < mHairs.size())
            ? mHairs[mHairIndex]
            : record.mHair;

        if (record.mHead == head && record.mHair == hair)
            return;

        record.mHead = head;
        record.mHair = hair;
        // Insert a dynamic override of the player's NPC record. For a post-chargen player this record
        // is dynamic, and insert() assigns it in place, so the live record the animations read now
        // carries the new head/hair.
        world->getStore().insert(record);
        if (mInventoryWindow)
            mInventoryWindow->rebuildAvatar();
        // Rebuild the existing player animation immediately. Unlike renderPlayer(), this leaves
        // mechanics and physics alone, so it is safe while the inventory, magic, map, stats, or
        // spell menus have paused normal world updates.
        world->reattachPlayerCamera();
    }

    void HeadHairWindow::onHeadScroll(MyGUI::ScrollBar* sender, size_t pos)
    {
        if (mHeads.empty())
            return;
        mHeadIndex = std::min(static_cast<int>(pos), static_cast<int>(mHeads.size()) - 1);
        apply();
        updateLabels();
    }

    void HeadHairWindow::onHairScroll(MyGUI::ScrollBar* sender, size_t pos)
    {
        if (mHairs.empty())
            return;
        mHairIndex = std::min(static_cast<int>(pos), static_cast<int>(mHairs.size()) - 1);
        apply();
        updateLabels();
    }
}
