#include "race.hpp"

#include <algorithm>
#include <cmath>

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_UString.h>

#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadspel.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwrender/characterpreview.hpp"
#include "../mwworld/esmstore.hpp"

#include "tooltips.hpp"

namespace
{
    bool sortRaces(const std::pair<ESM::RefId, std::string>& left, const std::pair<ESM::RefId, std::string>& right)
    {
        return left.second.compare(right.second) < 0;
    }

}

namespace MWGui
{

    RaceDialog::RaceDialog(osg::Group* parent, Resource::ResourceSystem* resourceSystem)
        : WindowModal("openmw_chargen_race.layout")
        , mParent(parent)
        , mResourceSystem(resourceSystem)
        , mGenderIndex(0)
        , mFaceIndex(0)
        , mHairIndex(0)
        , mCurrentAngle(0)
        , mPreviewDirty(true)
    {
        // Centre dialog
        center();

        setText("AppearanceT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu1", "Appearance"));
        getWidget(mPreviewImage, "PreviewImage");

        mPreviewImage->eventMouseWheel += MyGUI::newDelegate(this, &RaceDialog::onPreviewScroll);

        getWidget(mHeadRotate, "HeadRotate");

        mHeadRotate->setScrollRange(1000);
        mHeadRotate->setScrollPosition(500);
        mHeadRotate->setScrollViewPage(50);
        mHeadRotate->setScrollPage(50);
        mHeadRotate->setScrollWheelPage(50);
        mHeadRotate->eventScrollChangePosition += MyGUI::newDelegate(this, &RaceDialog::onHeadRotate);

        // Set up next/previous buttons
        MyGUI::Button *prevButton, *nextButton;

        setText("GenderChoiceT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu2", "Change Sex"));
        getWidget(prevButton, "PrevGenderButton");
        getWidget(nextButton, "NextGenderButton");
        prevButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectPreviousGender);
        nextButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectNextGender);

        setText("FaceChoiceT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu3", "Change Face"));
        getWidget(prevButton, "PrevFaceButton");
        getWidget(nextButton, "NextFaceButton");
        prevButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectPreviousFace);
        nextButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectNextFace);

        setText("HairChoiceT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu4", "Change Hair"));
        getWidget(prevButton, "PrevHairButton");
        getWidget(nextButton, "NextHairButton");
        prevButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectPreviousHair);
        nextButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onSelectNextHair);

        setText("RaceT", MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu5", "Race"));
        getWidget(mRaceList, "RaceList");
        mRaceList->setScrollVisible(true);
        mRaceList->eventListSelectAccept += MyGUI::newDelegate(this, &RaceDialog::onAccept);
        mRaceList->eventListChangePosition += MyGUI::newDelegate(this, &RaceDialog::onSelectRace);
        mRaceList->eventListChangeScroll += MyGUI::newDelegate(this, &RaceDialog::onListScroll);

        if (MyGUI::Widget* client = mRaceList->getClientWidget())
        {
            mControllerHighlight = client->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setVisible(false);
        }

        setText("SkillsT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sBonusSkillTitle", "Skill Bonus"));
        getWidget(mSkillList, "SkillList");
        setText("SpellPowerT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sRaceMenu7", "Specials"));
        getWidget(mSpellPowerList, "SpellPowerList");

        getWidget(mBackButton, "BackButton");
        mBackButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onBackClicked);

        getWidget(mOkButton, "OKButton");
        mOkButton->setCaption(
            MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &RaceDialog::onOkClicked);

        if (Settings::gui().mControllerMenus)
        {
            mControllerButtons = {};
            mControllerButtons.mA = "#{Interface:OK}";
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mX = "#{Interface:Sex}";
            if (Settings::input().mControllerIconStyle.get() == "gamecube")
            {
                mControllerButtons.mL2 = "#{Interface:Hair}";
                mControllerButtons.mR2 = "#{Interface:Face}";
            }
            else
            {
                mControllerButtons.mL1 = "#{Interface:Hair}";
                mControllerButtons.mR1 = "#{Interface:Face}";
            }
        }

        updateRaces();
        updateSkills();
        updateSpellPowers();
        updateControllerListHighlight(mRaceList, mControllerHighlight);
    }

    void RaceDialog::setNextButtonShow(bool shown)
    {
        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");

        if (shown)
        {
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sNext", {})));
            mControllerButtons.mA = "#{Interface:OK}";
        }
        else if (Settings::gui().mControllerMenus)
        {
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sDone", {})));
            mControllerButtons.mA = "#{Interface:OK}";
        }
        else
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
    }

    void RaceDialog::onOpen()
    {
        WindowModal::onOpen();

        updateRaces();
        updateSkills();
        updateSpellPowers();

        mPreviewImage->setRenderItemTexture(nullptr);

        mPreview.reset(nullptr);
        mPreviewTexture.reset(nullptr);

        mPreview = std::make_unique<MWRender::RaceSelectionPreview>(mParent, mResourceSystem);
        mPreview->rebuild();
        mPreview->setAngle(mCurrentAngle);

        mPreviewTexture
            = std::make_unique<MyGUIPlatform::OSGTexture>(mPreview->getTexture(), mPreview->getTextureStateSet());
        mPreviewImage->setRenderItemTexture(mPreviewTexture.get());
        // The widget is Y-down, the RTT image is Y-up, so this UV is inverted
        mPreviewImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));

        const ESM::NPC& proto = mPreview->getPrototype();
        setRaceId(proto.mRace);
        setGender(proto.isMale() ? GM_Male : GM_Female);
        recountParts();

        for (size_t i = 0; i < mAvailableHeads.size(); ++i)
        {
            if (mAvailableHeads[i] == proto.mHead)
                mFaceIndex = i;
        }

        for (size_t i = 0; i < mAvailableHairs.size(); ++i)
        {
            if (mAvailableHairs[i] == proto.mHair)
                mHairIndex = i;
        }

        mPreviewDirty = true;

        size_t initialPos = mHeadRotate->getScrollRange() / 2 + mHeadRotate->getScrollRange() / 10;
        mHeadRotate->setScrollPosition(initialPos);
        onHeadRotate(mHeadRotate, initialPos);

        mHeadRotateAxis = 0.f;
        mHeadRotateCarry = 0.f;

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mRaceList);
        updateControllerListHighlight(mRaceList, mControllerHighlight);
    }

    void RaceDialog::setRaceId(const ESM::RefId& raceId)
    {
        mCurrentRaceId = raceId;
        mRaceList->setIndexSelected(MyGUI::ITEM_NONE);
        size_t count = mRaceList->getItemCount();
        for (size_t i = 0; i < count; ++i)
        {
            if (*mRaceList->getItemDataAt<ESM::RefId>(i) == raceId)
            {
                mRaceList->setIndexSelected(i);
                break;
            }
        }

        updateSkills();
        updateSpellPowers();
    }

    void RaceDialog::onClose()
    {
        WindowModal::onClose();

        mPreviewImage->setRenderItemTexture(nullptr);

        mPreviewTexture.reset(nullptr);
        mPreview.reset(nullptr);

        mHeadRotateAxis = 0.f;
        mHeadRotateCarry = 0.f;

        mLeftTriggerHeld = false;
        mRightTriggerHeld = false;
        mHairButtonPending = false;
        mFaceButtonPending = false;
        mHairChordUsed = false;
        mFaceChordUsed = false;
    }

    // widget controls

    void RaceDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        if (mRaceList->getIndexSelected() == MyGUI::ITEM_NONE)
            return;
        eventDone(this);
    }

    void RaceDialog::onBackClicked(MyGUI::Widget* /*sender*/)
    {
        eventBack();
    }

    void RaceDialog::onPreviewScroll(MyGUI::Widget*, int delta)
    {
        size_t oldPos = mHeadRotate->getScrollPosition();
        size_t maxPos = mHeadRotate->getScrollRange() - 1;
        size_t scrollPage = mHeadRotate->getScrollWheelPage();
        if (delta < 0)
            mHeadRotate->setScrollPosition(oldPos + std::min(maxPos - oldPos, scrollPage));
        else
            mHeadRotate->setScrollPosition(oldPos - std::min(oldPos, scrollPage));

        onHeadRotate(mHeadRotate, mHeadRotate->getScrollPosition());
    }

    void RaceDialog::onFrame(float dt)
    {
        if (!mPreview || !mHeadRotate || mHeadRotateAxis == 0.f)
            return;

        const size_t range = mHeadRotate->getScrollRange();
        if (range <= 1)
            return;

        constexpr float kTurnsPerSecond = 1.4f;
        const float scrollPerSecond = static_cast<float>(range - 1) * kTurnsPerSecond;
        mHeadRotateCarry += mHeadRotateAxis * scrollPerSecond * dt;

        const int step = static_cast<int>(std::lround(mHeadRotateCarry));
        if (step == 0)
            return;

        mHeadRotateCarry -= static_cast<float>(step);

        size_t pos = mHeadRotate->getScrollPosition();
        if (step > 0)
        {
            pos = std::min(range - 1, pos + static_cast<size_t>(step));
        }
        else
        {
            const size_t delta = static_cast<size_t>(-step);
            pos = (pos > delta) ? (pos - delta) : 0;
        }

        mHeadRotate->setScrollPosition(pos);
        onHeadRotate(mHeadRotate, pos);
    }

    void RaceDialog::onHeadRotate(MyGUI::ScrollBar* scroll, size_t position)
    {
        float angle = (float(position) / (scroll->getScrollRange() - 1) - 0.5f) * osg::PIf * 2;
        mPreview->setAngle(angle);

        mCurrentAngle = angle;
    }

    void RaceDialog::onSelectPreviousGender(MyGUI::Widget*)
    {
        mGenderIndex = wrap(mGenderIndex, 2, -1);

        recountParts();
        updatePreview();
    }

    void RaceDialog::onSelectNextGender(MyGUI::Widget*)
    {
        mGenderIndex = wrap(mGenderIndex, 2, 1);

        recountParts();
        updatePreview();
    }

    void RaceDialog::onSelectPreviousFace(MyGUI::Widget*)
    {
        mFaceIndex = wrap(mFaceIndex, mAvailableHeads.size(), -1);
        updatePreview();
    }

    void RaceDialog::onSelectNextFace(MyGUI::Widget*)
    {
        mFaceIndex = wrap(mFaceIndex, mAvailableHeads.size(), 1);
        updatePreview();
    }

    void RaceDialog::onSelectPreviousHair(MyGUI::Widget*)
    {
        mHairIndex = wrap(mHairIndex, mAvailableHairs.size(), -1);
        updatePreview();
    }

    void RaceDialog::onSelectNextHair(MyGUI::Widget*)
    {
        mHairIndex = wrap(mHairIndex, mAvailableHairs.size(), 1);
        updatePreview();
    }

    void RaceDialog::onSelectRace(MyGUI::ListBox* sender, size_t index)
    {
        if (index == MyGUI::ITEM_NONE)
        {
            updateControllerListHighlight(sender, mControllerHighlight);
            return;
        }

        ESM::RefId& raceId = *mRaceList->getItemDataAt<ESM::RefId>(index);
        if (mCurrentRaceId == raceId)
        {
            updateControllerListHighlight(sender, mControllerHighlight);
            return;
        }

        mCurrentRaceId = raceId;

        recountParts();

        updatePreview();
        updateSkills();
        updateSpellPowers();
        updateControllerListHighlight(sender, mControllerHighlight);
    }

    void RaceDialog::onListScroll(MyGUI::ListBox* sender, size_t /*position*/)
    {
        updateControllerListHighlight(sender, mControllerHighlight);
    }

    void RaceDialog::onAccept(MyGUI::ListBox* sender, size_t index)
    {
        onSelectRace(sender, index);
        if (mRaceList->getIndexSelected() == MyGUI::ITEM_NONE)
            return;
        eventDone(this);
    }

    void RaceDialog::getBodyParts(int part, std::vector<ESM::RefId>& out)
    {
        out.clear();
        const MWWorld::Store<ESM::BodyPart>& store = MWBase::Environment::get().getESMStore()->get<ESM::BodyPart>();

        for (const ESM::BodyPart& bodypart : store)
        {
            if (bodypart.mData.mFlags & ESM::BodyPart::BPF_NotPlayable)
                continue;
            if (bodypart.mData.mType != ESM::BodyPart::MT_Skin)
                continue;
            if (bodypart.mData.mPart != static_cast<ESM::BodyPart::MeshPart>(part))
                continue;
            if (mGenderIndex != (bodypart.mData.mFlags & ESM::BodyPart::BPF_Female))
                continue;
            if (ESM::isFirstPersonBodyPart(bodypart))
                continue;
            if (bodypart.mRace == mCurrentRaceId)
                out.push_back(bodypart.mId);
        }
    }

    void RaceDialog::recountParts()
    {
        getBodyParts(ESM::BodyPart::MP_Hair, mAvailableHairs);
        getBodyParts(ESM::BodyPart::MP_Head, mAvailableHeads);

        mFaceIndex = 0;
        mHairIndex = 0;
    }

    // update widget content

    void RaceDialog::updatePreview()
    {
        ESM::NPC record = mPreview->getPrototype();
        record.mRace = mCurrentRaceId;
        record.setIsMale(mGenderIndex == 0);

        if (mFaceIndex < mAvailableHeads.size())
            record.mHead = mAvailableHeads[mFaceIndex];

        if (mHairIndex < mAvailableHairs.size())
            record.mHair = mAvailableHairs[mHairIndex];

        try
        {
            mPreview->setPrototype(record);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Error creating preview: " << e.what();
        }
    }

    void RaceDialog::updateRaces()
    {
        mRaceList->removeAllItems();

        const MWWorld::Store<ESM::Race>& races = MWBase::Environment::get().getESMStore()->get<ESM::Race>();

        std::vector<std::pair<ESM::RefId, std::string>> items; // ID, name
        for (const ESM::Race& race : races)
        {
            bool playable = race.mData.mFlags & ESM::Race::Playable;
            if (!playable) // Only display playable races
                continue;

            items.emplace_back(race.mId, race.mName);
        }
        std::sort(items.begin(), items.end(), sortRaces);

        int index = 0;
        for (auto& item : items)
        {
            mRaceList->addItem(item.second, item.first);
            if (item.first == mCurrentRaceId)
                mRaceList->setIndexSelected(index);
            ++index;
        }

        updateControllerListHighlight(mRaceList, mControllerHighlight);
    }

    void RaceDialog::updateSkills()
    {
        for (MyGUI::Widget* widget : mSkillItems)
        {
            MyGUI::Gui::getInstance().destroyWidget(widget);
        }
        mSkillItems.clear();

        if (mCurrentRaceId.empty())
            return;

        Widgets::MWSkillPtr skillWidget;
        const int lineHeight = Settings::gui().mFontSize + 2;
        MyGUI::IntCoord coord1(0, 0, mSkillList->getWidth(), 18);

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Race* race = store.get<ESM::Race>().find(mCurrentRaceId);
        for (const auto& bonus : race->mData.mBonus)
        {
            ESM::RefId skill = ESM::Skill::indexToRefId(bonus.mSkill);
            if (skill.empty()) // Skip unknown skill indexes
                continue;

            skillWidget = mSkillList->createWidget<Widgets::MWSkill>("MW_StatNameValue", coord1, MyGUI::Align::Default);
            skillWidget->setSkillId(skill);
            skillWidget->setSkillValue(Widgets::MWSkill::SkillValue(static_cast<float>(bonus.mBonus), 0.f));
            ToolTips::createSkillToolTip(skillWidget, skill);

            mSkillItems.push_back(skillWidget);

            coord1.top += lineHeight;
        }
    }

    void RaceDialog::updateSpellPowers()
    {
        for (MyGUI::Widget* widget : mSpellPowerItems)
        {
            MyGUI::Gui::getInstance().destroyWidget(widget);
        }
        mSpellPowerItems.clear();

        if (mCurrentRaceId.empty())
            return;

        const int lineHeight = Settings::gui().mFontSize + 2;
        MyGUI::IntCoord coord(0, 0, mSpellPowerList->getWidth(), lineHeight);

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Race* race = store.get<ESM::Race>().find(mCurrentRaceId);

        int i = 0;
        for (const ESM::RefId& spellpower : race->mPowers.mList)
        {
            Widgets::MWSpellPtr spellPowerWidget = mSpellPowerList->createWidget<Widgets::MWSpell>(
                "MW_StatName", coord, MyGUI::Align::Default, std::string("SpellPower") + MyGUI::utility::toString(i));
            spellPowerWidget->setSpellId(spellpower);
            spellPowerWidget->setUserString("ToolTipType", "Spell");
            spellPowerWidget->setUserString("Spell", spellpower.serialize());
            spellPowerWidget->setUserString("SpellName", store.get<ESM::Spell>().find(spellpower)->mName);

            mSpellPowerItems.push_back(spellPowerWidget);

            coord.top += lineHeight;
            ++i;
        }
    }

    bool RaceDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        const bool useGamecubeHairButton = Settings::input().mControllerIconStyle.get() == "gamecube";
        const bool pressed = arg.state == SDL_PRESSED;
        const bool released = arg.state == SDL_RELEASED;

        if (useGamecubeHairButton)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            {
                return true;
            }

            if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            {
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        {
            if (pressed)
            {
                mHairButtonPending = true;
                mHairChordUsed = false;
                if (mLeftTriggerHeld)
                {
                    onSelectPreviousHair(nullptr);
                    mHairChordUsed = true;
                }
                else if (mRightTriggerHeld)
                {
                    onSelectNextHair(nullptr);
                    mHairChordUsed = true;
                }
            }
            else if (released && mHairButtonPending)
            {
                if (!mHairChordUsed)
                    onSelectNextHair(nullptr);
                mHairButtonPending = false;
                mHairChordUsed = false;
            }
            return true;
        }

        if (!useGamecubeHairButton && arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            if (pressed)
            {
                mFaceButtonPending = true;
                mFaceChordUsed = false;
                if (mLeftTriggerHeld)
                {
                    onSelectPreviousFace(nullptr);
                    mFaceChordUsed = true;
                }
                else if (mRightTriggerHeld)
                {
                    onSelectNextFace(nullptr);
                    mFaceChordUsed = true;
                }
            }
            else if (released && mFaceButtonPending)
            {
                if (!mFaceChordUsed)
                    onSelectNextFace(nullptr);
                mFaceButtonPending = false;
                mFaceChordUsed = false;
            }
            return true;
        }

        if (!pressed)
            return true;

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onBackClicked(mBackButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            onOkClicked(mOkButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onSelectNextGender(nullptr);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            winMgr->setKeyFocusWidget(mRaceList);
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowUp, 0, false);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            winMgr->setKeyFocusWidget(mRaceList);
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowDown, 0, false);
        }

        return true;
    }

    bool RaceDialog::onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg)
    {
        const bool useGamecubeHairButton = Settings::input().mControllerIconStyle.get() == "gamecube";
        constexpr int kTriggerPressThreshold = 16000;
        constexpr int kTriggerReleaseThreshold = 8000;

        if (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT)
        {
            if (!mLeftTriggerHeld && arg.value >= kTriggerPressThreshold)
            {
                mLeftTriggerHeld = true;
                if (useGamecubeHairButton)
                    onSelectNextHair(nullptr);
                else
                {
                    if (mHairButtonPending)
                    {
                        onSelectPreviousHair(nullptr);
                        mHairChordUsed = true;
                    }
                    if (mFaceButtonPending)
                    {
                        onSelectPreviousFace(nullptr);
                        mFaceChordUsed = true;
                    }
                }
            }
            else if (mLeftTriggerHeld && arg.value <= kTriggerReleaseThreshold)
            {
                mLeftTriggerHeld = false;
            }
            return true;
        }

        if (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
        {
            if (!mRightTriggerHeld && arg.value >= kTriggerPressThreshold)
            {
                mRightTriggerHeld = true;
                if (useGamecubeHairButton)
                    onSelectNextFace(nullptr);
                else
                {
                    if (mHairButtonPending)
                    {
                        onSelectNextHair(nullptr);
                        mHairChordUsed = true;
                    }
                    if (mFaceButtonPending)
                    {
                        onSelectNextFace(nullptr);
                        mFaceChordUsed = true;
                    }
                }
            }
            else if (mRightTriggerHeld && arg.value <= kTriggerReleaseThreshold)
            {
                mRightTriggerHeld = false;
            }
            return true;
        }

        if (arg.axis == SDL_CONTROLLER_AXIS_RIGHTX)
        {
            constexpr float kAxisMax = 32767.f;
            constexpr float kDeadzone = 0.6f;

            float axis = static_cast<float>(arg.value) / kAxisMax;
            axis = std::clamp(axis, -1.f, 1.f);

            const float magnitude = std::abs(axis);
            if (magnitude <= kDeadzone)
            {
                mHeadRotateAxis = 0.f;
            }
            else
            {
                const float sign = axis < 0.f ? -1.f : 1.f;
                const float scaled = (magnitude - kDeadzone) / (1.f - kDeadzone);
                mHeadRotateAxis = sign * scaled;
            }

            return true;
        }

        return false;
    }

    const ESM::NPC& RaceDialog::getResult() const
    {
        return mPreview->getPrototype();
    }
}
