#include "statswindow.hpp"

#include <MyGUI_Button.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_TextIterator.h>
#include <MyGUI_Window.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <components/debug/debuglog.hpp>

#include <components/esm3/loadbsgn.hpp>
#include <components/esm3/loadclas.hpp>
#include <components/esm3/loadfact.hpp>
#include <components/esm3/loadgmst.hpp>
#include <components/esm3/loadrace.hpp>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "tooltips.hpp"

namespace
{
    MyGUI::Widget* createControllerHighlight(MyGUI::Widget* parent, const MyGUI::IntCoord& coord)
    {
        if (!parent)
            return nullptr;

        auto* highlight = parent->createWidget<MyGUI::Widget>("ControllerHighlight", coord, MyGUI::Align::Default);
        highlight->setNeedMouseFocus(false);
        highlight->setDepth(1);
        highlight->setVisible(false);
        return highlight;
    }
}

namespace MWGui
{
    StatsWindow::StatsWindow(DragAndDrop* drag)
        : WindowPinnableBase("openmw_stats_window.layout")
        , NoDrop(drag, mMainWidget)
        , mSkillView(nullptr)
        , mReputation(0)
        , mBounty(0)
        , mChanged(true)
        , mMinFullWidth(mMainWidget->getSize().width)
    {

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        MyGUI::Widget* attributeView = getWidget("AttributeView");
        MyGUI::IntCoord coord{ 0, 0, 204, 18 };
        const MyGUI::Align alignment = MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch;
        for (const ESM::Attribute& attribute : store.get<ESM::Attribute>())
        {
            auto* box = attributeView->createWidget<MyGUI::Button>({}, coord, alignment);
            box->setUserString("ToolTipDynamic", "Stats");
            box->setUserString("ToolTipType", "Layout");
            box->setUserString("ToolTipLayout", "AttributeToolTip");
            box->setUserString("Caption_AttributeName", attribute.mName);
            box->setUserString("Caption_AttributeDescription", attribute.mDescription);
            box->setUserString("ImageTexture_AttributeImage", attribute.mIcon);
            coord.top += coord.height;
            auto* name = box->createWidget<MyGUI::TextBox>("SandTextController", { 0, 0, 160, 18 }, alignment);
            name->setNeedMouseFocus(false);
            name->setCaption(attribute.mName);
            auto* value = box->createWidget<MyGUI::TextBox>(
                "SandTextRightController", { 160, 0, 44, 18 }, MyGUI::Align::Right | MyGUI::Align::Top);
            value->setNeedMouseFocus(false);
            mAttributeWidgets.emplace(attribute.mId, value);
            mAttributeControllerEntries.push_back({ box, name, value });
        }

        getWidget(mSkillView, "SkillView");
        getWidget(mLeftPane, "LeftPane");
        getWidget(mRightPane, "RightPane");

        for (const ESM::Skill& skill : store.get<ESM::Skill>())
        {
            mSkillValues.emplace(skill.mId, MWMechanics::SkillValue());
            mSkillWidgetMap.emplace(skill.mId, std::make_pair<MyGUI::TextBox*, MyGUI::TextBox*>(nullptr, nullptr));
        }

        MyGUI::Window* t = mMainWidget->castType<MyGUI::Window>();
        t->eventWindowChangeCoord += MyGUI::newDelegate(this, &StatsWindow::onWindowResize);

        {
            MyGUI::Widget* healthWidget = nullptr;
            MyGUI::Widget* magickaWidget = nullptr;
            MyGUI::Widget* fatigueWidget = nullptr;
            MyGUI::TextBox* healthName = nullptr;
            MyGUI::TextBox* magickaName = nullptr;
            MyGUI::TextBox* fatigueName = nullptr;
            MyGUI::TextBox* healthValue = nullptr;
            MyGUI::TextBox* magickaValue = nullptr;
            MyGUI::TextBox* fatigueValue = nullptr;

            getWidget(healthWidget, "Health");
            getWidget(magickaWidget, "Magicka");
            getWidget(fatigueWidget, "Fatigue");
            getWidget(healthName, "Health_str");
            getWidget(magickaName, "Magicka_str");
            getWidget(fatigueName, "Fatigue_str");
            getWidget(healthValue, "HBarT");
            getWidget(magickaValue, "MBarT");
            getWidget(fatigueValue, "FBarT");

            if (healthWidget)
                healthWidget->setUserString("ToolTipDynamic", "Stats");
            if (magickaWidget)
                magickaWidget->setUserString("ToolTipDynamic", "Stats");
            if (fatigueWidget)
                fatigueWidget->setUserString("ToolTipDynamic", "Stats");

            addStaticControllerEntry(healthWidget, { healthName, healthValue });
            addStaticControllerEntry(magickaWidget, { magickaName, magickaValue });
            addStaticControllerEntry(fatigueWidget, { fatigueName, fatigueValue });

            MyGUI::Widget* levelWidget = nullptr;
            MyGUI::Widget* raceWidget = nullptr;
            MyGUI::Widget* classWidget = nullptr;
            MyGUI::Widget* nameWidget = nullptr;
            MyGUI::TextBox* levelName = nullptr;
            MyGUI::TextBox* raceName = nullptr;
            MyGUI::TextBox* className = nullptr;
            MyGUI::TextBox* nameName = nullptr;
            MyGUI::TextBox* levelValue = nullptr;
            MyGUI::TextBox* raceValue = nullptr;
            MyGUI::TextBox* classValue = nullptr;
            MyGUI::TextBox* nameValue = nullptr;

            getWidget(nameWidget, "NameText");
            getWidget(levelWidget, "LevelText");
            getWidget(raceWidget, "RaceText");
            getWidget(classWidget, "ClassText");
            getWidget(nameName, "Name_str");
            getWidget(levelName, "Level_str");
            getWidget(raceName, "Race_str");
            getWidget(className, "Class_str");
            getWidget(nameValue, "NameText");
            getWidget(levelValue, "LevelText");
            getWidget(raceValue, "RaceText");
            getWidget(classValue, "ClassText");

            if (levelWidget)
                levelWidget->setUserString("ToolTipDynamic", "Stats");
            if (levelName)
                levelName->setUserString("ToolTipDynamic", "Stats");

            mNameRow = nameName ? nameName->getParent() : nullptr;
            mLevelRow = levelName ? levelName->getParent() : nullptr;
            mRaceRow = raceName ? raceName->getParent() : nullptr;
            mClassRow = className ? className->getParent() : nullptr;

            if (mNameRow && mNameRow->getParent())
                mNameHighlight
                    = createControllerHighlight(mNameRow->getParent(), MyGUI::IntCoord(mNameRow->getCoord()));
            if (mLevelRow && mLevelRow->getParent())
                mLevelHighlight
                    = createControllerHighlight(mLevelRow->getParent(), MyGUI::IntCoord(mLevelRow->getCoord()));
            if (mRaceRow && mRaceRow->getParent())
                mRaceHighlight
                    = createControllerHighlight(mRaceRow->getParent(), MyGUI::IntCoord(mRaceRow->getCoord()));
            if (mClassRow && mClassRow->getParent())
                mClassHighlight
                    = createControllerHighlight(mClassRow->getParent(), MyGUI::IntCoord(mClassRow->getCoord()));

            addStaticControllerEntry(nameWidget, { nameName, nameValue }, mNameHighlight, false);
            addStaticControllerEntry(levelWidget, { levelName, levelValue }, mLevelHighlight, false);
            addStaticControllerEntry(raceWidget, { raceName, raceValue }, mRaceHighlight, false);
            addStaticControllerEntry(classWidget, { className, classValue }, mClassHighlight, false);
        }

        if (Settings::gui().mControllerMenus)
        {
            setPinButtonVisible(false);
            mControllerButtons = {};
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mY = "#{Interface:Info}";
            if (Settings::gui().mXboxTabOrder)
            {
                mControllerButtons.mL2 = "#{Interface:Map}";
                mControllerButtons.mR2 = "#{Interface:Inventory}";
            }
            else
            {
                mControllerButtons.mL2 = "#{Interface:Magic}";
                mControllerButtons.mR2 = "#{Interface:Map}";
            }
            mDisableGamepadCursor = true;
        }

        if (t)
        {
            const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            t->setCoord(getFixedWindowCoord(viewSize));
        }
        onWindowResize(t);
    }

    ControllerButtons* StatsWindow::getControllerButtons()
    {
        if (Settings::gui().mControllerMenus)
        {
            const bool skipMap = MWBase::Environment::get().getWindowManager()->isCrassifiedNavigationEnabled();
            if (Settings::gui().mXboxTabOrder)
            {
                mControllerButtons.mL2 = skipMap ? "#{Interface:Magic}" : "#{Interface:Map}";
                mControllerButtons.mR2 = "#{Interface:Inventory}";
            }
            else
            {
                mControllerButtons.mL2 = "#{Interface:Magic}";
                mControllerButtons.mR2 = skipMap ? "#{Interface:Inventory}" : "#{Interface:Map}";
            }
        }

        return &mControllerButtons;
    }

    void StatsWindow::onOpen()
    {
        resetFixedWindowGeometry();
        onWindowResize(mMainWidget->castType<MyGUI::Window>());

        const MyGUI::IntSize skillViewSize = mSkillView ? mSkillView->getSize() : MyGUI::IntSize(0, 0);
        if (!mSkillWidgets.empty() && skillViewSize == mLastSkillViewSize)
            return;

        if (!mMajorSkills.empty() || !mMinorSkills.empty())
        {
            updateSkillArea();
            return;
        }

        MWWorld::Ptr player = MWMechanics::getPlayer();
        if (player.isEmpty() || !player.getClass().isNpc())
            return;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Class* cls = store.get<ESM::Class>().search(player.get<ESM::NPC>()->mBase->mClass);
        if (!cls)
            return;

        const size_t size = cls->mData.mSkills.size();
        std::vector<ESM::RefId> majorSkills(size);
        std::vector<ESM::RefId> minorSkills(size);
        for (size_t i = 0; i < size; ++i)
        {
            minorSkills[i] = ESM::Skill::indexToRefId(cls->mData.mSkills[i][0]);
            majorSkills[i] = ESM::Skill::indexToRefId(cls->mData.mSkills[i][1]);
        }
        configureSkills(majorSkills, minorSkills);
    }

    void StatsWindow::onMouseWheel(MyGUI::Widget* /*sender*/, int rel)
    {
        if (mSkillView->getCanvasSize().height <= mSkillView->getHeight())
            return;

        const float scaledStep = rel * 0.3f;
        const int scrollDelta = scaledStep > 0.f ? std::max(1, static_cast<int>(std::floor(scaledStep)))
                                                 : std::min(-1, static_cast<int>(std::ceil(scaledStep)));
        const int minOffset = std::min(0, mSkillView->getHeight() - mSkillView->getCanvasSize().height);
        const int newOffset = std::clamp(mSkillView->getViewOffset().top + scrollDelta, minOffset, 0);
        mSkillView->setViewOffset(MyGUI::IntPoint(0, newOffset));
    }

    void StatsWindow::onWindowResize(MyGUI::Window* window)
    {
        const MyGUI::IntCoord clientCoord = window->getClientCoord();
        int windowWidth = clientCoord.width;
        int windowHeight = clientCoord.height;

        // initial values defined in openmw_stats_window.layout, if custom options are not present in .layout, a default
        // is loaded
        float leftPaneRatio = 0.44f;
        if (mLeftPane->isUserString("LeftPaneRatio"))
            leftPaneRatio = MyGUI::utility::parseFloat(mLeftPane->getUserString("LeftPaneRatio"));

        int leftOffsetWidth = 24;
        if (mLeftPane->isUserString("LeftOffsetWidth"))
            leftOffsetWidth = MyGUI::utility::parseInt(mLeftPane->getUserString("LeftOffsetWidth"));

        int minLeftWidth = static_cast<int>(mMinFullWidth * leftPaneRatio);
        int minLeftOffsetWidth = minLeftWidth + leftOffsetWidth;

        // if there's no space for right pane
        mRightPane->setVisible(windowWidth >= minLeftOffsetWidth);
        if (!mRightPane->getVisible())
        {
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, windowWidth - leftOffsetWidth, windowHeight));
        }
        // if there's some space for right pane
        else if (windowWidth < mMinFullWidth)
        {
            const int leftWidth = std::max(0, minLeftWidth);
            const int rightWidth = std::max(0, windowWidth - leftWidth);
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, leftWidth, windowHeight));
            mRightPane->setCoord(MyGUI::IntCoord(leftWidth, 0, rightWidth, windowHeight));
        }
        // if there's enough space for both panes
        else
        {
            const int leftWidth = std::max(0, static_cast<int>(leftPaneRatio * windowWidth));
            const int rightWidth = std::max(0, windowWidth - leftWidth);
            mLeftPane->setCoord(MyGUI::IntCoord(0, 0, leftWidth, windowHeight));
            mRightPane->setCoord(MyGUI::IntCoord(leftWidth, 0, rightWidth, windowHeight));
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mSkillView->setVisibleVScroll(false);
        mSkillView->setCanvasSize(mSkillView->getWidth(), mSkillView->getCanvasSize().height);
        mSkillView->setVisibleVScroll(true);

        updateStaticHighlights();
    }

    void StatsWindow::updateStaticHighlights()
    {
        if (mNameHighlight && mNameRow)
            mNameHighlight->setCoord(mNameRow->getCoord());
        if (mLevelHighlight && mLevelRow)
            mLevelHighlight->setCoord(mLevelRow->getCoord());
        if (mRaceHighlight && mRaceRow)
            mRaceHighlight->setCoord(mRaceRow->getCoord());
        if (mClassHighlight && mClassRow)
            mClassHighlight->setCoord(mClassRow->getCoord());
    }

    void StatsWindow::setBar(const std::string& name, const std::string& tname, int val, int max)
    {
        MyGUI::ProgressBar* pt;
        getWidget(pt, name);

        std::stringstream out;
        out << val << "/" << max;
        setText(tname, out.str());

        pt->setProgressRange(std::max(0, max));
        pt->setProgressPosition(std::max(0, val));
    }

    void StatsWindow::setPlayerName(const std::string& playerName)
    {
        MyGUI::TextBox* nameValue = nullptr;
        MyGUI::TextBox* nameLabel = nullptr;
        getWidget(nameValue, "NameText");
        getWidget(nameLabel, "Name_str");
        if (nameValue)
            nameValue->setCaption(playerName);
        if (nameLabel)
        {
            const std::string label = nameLabel->getCaption().asUTF8();
            if (nameValue)
            {
                nameValue->setUserString("CollapsedLabel", label);
                nameValue->setUserString("CollapsedValue", playerName);
            }
            nameLabel->setUserString("CollapsedLabel", label);
            nameLabel->setUserString("CollapsedValue", playerName);
        }
        mMainWidget->castType<MyGUI::Window>()->setCaption(playerName);
    }

    void StatsWindow::setAttribute(ESM::RefId id, const MWMechanics::AttributeValue& value)
    {
        auto it = mAttributeWidgets.find(id);
        if (it != mAttributeWidgets.end())
        {
            MyGUI::TextBox* box = it->second;
            box->setCaption(std::to_string(static_cast<int>(value.getModified())));
            std::string state = "normal";
            if (value.getModified() > value.getBase())
                state = "increased";
            else if (value.getModified() < value.getBase())
                state = "decreased";
            box->_setWidgetState(state);
            setControllerWidgetBaseState(box, state);

            if (MyGUI::Widget* tooltipWidget = box->getParent())
            {
                const std::string label = std::string(tooltipWidget->getUserString("Caption_AttributeName"));
                tooltipWidget->setUserString("CollapsedLabel", label);
                tooltipWidget->setUserString("CollapsedValue", std::to_string(static_cast<int>(value.getModified())));
            }
        }
    }

    void StatsWindow::setValue(std::string_view id, const MWMechanics::DynamicStat<float>& value)
    {
        int current = static_cast<int>(value.getCurrent());
        int modified = static_cast<int>(value.getModified(false));

        // Fatigue can be negative
        if (id != "FBar")
            current = std::max(0, current);

        setBar(std::string(id), std::string(id) + "T", current, modified);

        // health, magicka, fatigue tooltip
        MyGUI::Widget* w;
        std::string valStr = MyGUI::utility::toString(current) + " / " + MyGUI::utility::toString(modified);
        auto setCollapsedPair = [&](const char* labelWidgetName, MyGUI::Widget* tooltipWidget,
                                    const std::string& text) {
            MyGUI::TextBox* labelWidget = nullptr;
            getWidget(labelWidget, labelWidgetName);
            const std::string label = labelWidget ? labelWidget->getCaption().asUTF8() : std::string(labelWidgetName);
            tooltipWidget->setUserString("CollapsedLabel", label);
            tooltipWidget->setUserString("CollapsedValue", text);
        };
        if (id == "HBar")
        {
            getWidget(w, "Health");
            w->setUserString("Caption_HealthDescription", "#{sHealthDesc}\n" + valStr);
            setCollapsedPair("Health_str", w, valStr);
        }
        else if (id == "MBar")
        {
            getWidget(w, "Magicka");
            w->setUserString("Caption_HealthDescription", "#{sMagDesc}\n" + valStr);
            setCollapsedPair("Magicka_str", w, valStr);
        }
        else if (id == "FBar")
        {
            getWidget(w, "Fatigue");
            w->setUserString("Caption_HealthDescription", "#{sFatDesc}\n" + valStr);
            setCollapsedPair("Fatigue_str", w, valStr);
        }
    }

    void StatsWindow::setValue(std::string_view id, const std::string& value)
    {
        if (id == "name")
            setPlayerName(value);
        else if (id == "race")
        {
            setText("RaceText", value);
            auto setCollapsed = [&](const char* widgetName, const char* labelWidgetName) {
                MyGUI::Widget* w = nullptr;
                MyGUI::TextBox* label = nullptr;
                getWidget(w, widgetName);
                getWidget(label, labelWidgetName);
                if (w && label)
                {
                    w->setUserString("CollapsedLabel", label->getCaption().asUTF8());
                    w->setUserString("CollapsedValue", value);
                }
            };
            setCollapsed("RaceText", "Race_str");
            setCollapsed("Race_str", "Race_str");
        }
        else if (id == "class")
        {
            setText("ClassText", value);
            auto setCollapsed = [&](const char* widgetName, const char* labelWidgetName) {
                MyGUI::Widget* w = nullptr;
                MyGUI::TextBox* label = nullptr;
                getWidget(w, widgetName);
                getWidget(label, labelWidgetName);
                if (w && label)
                {
                    w->setUserString("CollapsedLabel", label->getCaption().asUTF8());
                    w->setUserString("CollapsedValue", value);
                }
            };
            setCollapsed("ClassText", "Class_str");
            setCollapsed("Class_str", "Class_str");
        }
    }

    void StatsWindow::setValue(std::string_view id, int value)
    {
        if (id == "level")
        {
            std::ostringstream text;
            text << value;
            setText("LevelText", text.str());
            auto setCollapsed = [&](const char* widgetName, const char* labelWidgetName) {
                MyGUI::Widget* w = nullptr;
                MyGUI::TextBox* label = nullptr;
                getWidget(w, widgetName);
                getWidget(label, labelWidgetName);
                if (w && label)
                {
                    w->setUserString("CollapsedLabel", label->getCaption().asUTF8());
                    w->setUserString("CollapsedValue", text.str());
                }
            };
            setCollapsed("LevelText", "Level_str");
            setCollapsed("Level_str", "Level_str");
        }
    }

    void setSkillProgress(MyGUI::Widget* w, float progress, ESM::RefId skillId)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        float progressRequirement = player.getClass().getNpcStats(player).getSkillProgressRequirement(
            skillId, *esmStore.get<ESM::Class>().find(player.get<ESM::NPC>()->mBase->mClass));

        // This is how vanilla MW displays the progress bar (I think). Note it's slightly inaccurate,
        // due to the int casting in the skill levelup logic. Also the progress label could in rare cases
        // reach 100% without the skill levelling up.
        // Leaving the original display logic for now, for consistency with ess-imported savegames.
        int progressPercent = int(float(progress) / float(progressRequirement) * 100.f + 0.5f);

        w->setUserString("Caption_SkillProgressText", MyGUI::utility::toString(progressPercent) + "/100");
        w->setUserString("RangePosition_SkillProgress", MyGUI::utility::toString(progressPercent));
    }

    void setCustomSkillProgress(MyGUI::Widget* w, float progress)
    {
        const int progressPercent = std::clamp(static_cast<int>(progress * 100.f + 0.5f), 0, 100);
        w->setUserString("Caption_SkillProgressText", MyGUI::utility::toString(progressPercent) + "/100");
        w->setUserString("RangePosition_SkillProgress", MyGUI::utility::toString(progressPercent));
    }

    void StatsWindow::setValue(ESM::RefId id, const MWMechanics::SkillValue& value)
    {
        mSkillValues[id] = value;
        std::pair<MyGUI::TextBox*, MyGUI::TextBox*> widgets = mSkillWidgetMap[id];
        MyGUI::TextBox* valueWidget = widgets.second;
        MyGUI::TextBox* nameWidget = widgets.first;
        if (valueWidget && nameWidget)
        {
            float modified = value.getModified(), base = value.getBase();
            std::string text = MyGUI::utility::toString(static_cast<int>(modified));
            std::string state = "normal";
            if (modified > base)
                state = "increased";
            else if (modified < base)
                state = "decreased";

            int widthBefore = valueWidget->getTextSize().width;

            valueWidget->setCaption(text);
            valueWidget->_setWidgetState(state);
            setControllerWidgetBaseState(valueWidget, state);

            int widthAfter = valueWidget->getTextSize().width;
            if (widthBefore != widthAfter)
            {
                valueWidget->setCoord(valueWidget->getLeft() - (widthAfter - widthBefore), valueWidget->getTop(),
                    valueWidget->getWidth() + (widthAfter - widthBefore), valueWidget->getHeight());
                nameWidget->setSize(nameWidget->getWidth() - (widthAfter - widthBefore), nameWidget->getHeight());
            }

            if (value.getBase() < 100)
            {
                nameWidget->setUserString("Visible_SkillMaxed", "false");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                nameWidget->setUserString("Visible_SkillProgressVBox", "true");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                valueWidget->setUserString("Visible_SkillMaxed", "false");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                valueWidget->setUserString("Visible_SkillProgressVBox", "true");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                setSkillProgress(nameWidget, value.getProgress(), id);
                setSkillProgress(valueWidget, value.getProgress(), id);
            }
            else
            {
                nameWidget->setUserString("Visible_SkillMaxed", "true");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                nameWidget->setUserString("Visible_SkillProgressVBox", "false");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");

                valueWidget->setUserString("Visible_SkillMaxed", "true");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                valueWidget->setUserString("Visible_SkillProgressVBox", "false");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");
            }

            const std::string label = nameWidget->isUserString("Caption_SkillName")
                ? std::string(nameWidget->getUserString("Caption_SkillName"))
                : nameWidget->getCaption().asUTF8();
            nameWidget->setUserString("CollapsedLabel", label);
            valueWidget->setUserString("CollapsedLabel", label);
            nameWidget->setUserString("CollapsedValue", text);
            valueWidget->setUserString("CollapsedValue", text);
        }
    }

    void StatsWindow::configureSkills(const std::vector<ESM::RefId>& major, const std::vector<ESM::RefId>& minor)
    {
        mMajorSkills = major;
        mMinorSkills = minor;

        // Update misc skills with the remaining skills not in major or minor
        std::set<ESM::RefId> skillSet;
        std::copy(major.begin(), major.end(), std::inserter(skillSet, skillSet.begin()));
        std::copy(minor.begin(), minor.end(), std::inserter(skillSet, skillSet.begin()));
        mMiscSkills.clear();
        const auto& store = MWBase::Environment::get().getWorld()->getStore().get<ESM::Skill>();
        for (const auto& skill : store)
        {
            if (!skillSet.contains(skill.mId))
                mMiscSkills.push_back(skill.mId);
        }

        updateSkillArea();
    }

    void StatsWindow::onFrame(float dt)
    {
        NoDrop::onFrame(dt);

        if (mPendingControllerFocusRefresh && mActiveControllerWindow && Settings::gui().mControllerMenus
            && !mControllerItems.empty())
        {
            mPendingControllerFocusRefresh = false;
            updateControllerFocus(mControllerItems.size(), mControllerFocus);
        }

        updateCustomSkillsFromLua();

        MWWorld::Ptr player = MWMechanics::getPlayer();
        const MWMechanics::NpcStats& playerStats = player.getClass().getNpcStats(player);
        const auto& store = MWBase::Environment::get().getESMStore();

        std::stringstream detail;
        bool first = true;
        for (const auto& attribute : store->get<ESM::Attribute>())
        {
            int mult = playerStats.getLevelupAttributeMultiplier(attribute.mId);
            mult = std::min(mult, static_cast<int>(100 - playerStats.getAttribute(attribute.mId).getBase()));
            if (mult > 1)
            {
                if (!first)
                    detail << '\n';
                detail << attribute.mName << " x" << MyGUI::utility::toString(mult);
                first = false;
            }
        }
        std::string detailText = detail.str();

        // level progress
        MyGUI::Widget* levelWidget;
        for (int i = 0; i < 2; ++i)
        {
            int max = store->get<ESM::GameSetting>().find("iLevelUpTotal")->mValue.getInteger();
            getWidget(levelWidget, i == 0 ? "Level_str" : "LevelText");

            levelWidget->setUserString(
                "RangePosition_LevelProgress", MyGUI::utility::toString(playerStats.getLevelProgress()));
            levelWidget->setUserString("Range_LevelProgress", MyGUI::utility::toString(max));
            levelWidget->setUserString("Caption_LevelProgressText",
                MyGUI::utility::toString(playerStats.getLevelProgress()) + "/" + MyGUI::utility::toString(max));
            levelWidget->setUserString("Caption_LevelDetailText", detailText);
        }

        setFactions(playerStats.getFactionRanks());
        setExpelled(playerStats.getExpelled());

        const auto& signId = MWBase::Environment::get().getWorld()->getPlayer().getBirthSign();

        setBirthSign(signId);
        setReputation(playerStats.getReputation());
        setBounty(playerStats.getBounty());

        if (mChanged)
            updateSkillArea();
    }

    void StatsWindow::updateCustomSkillsFromLua()
    {
        MWBase::LuaManager* luaManager = MWBase::Environment::get().getLuaManager();
        if (!luaManager)
            return;

        std::vector<MWBase::LuaManager::CustomSkillForStatsWindow> skills = luaManager->getCustomSkillsForStatsWindow();
        skills.erase(std::remove_if(skills.begin(), skills.end(),
                         [](const MWBase::LuaManager::CustomSkillForStatsWindow& s) {
                             return !s.mVisible || s.mId.empty() || s.mName.empty();
                         }),
            skills.end());

        if (skills.empty())
        {
            if (!mCustomSkillWidgetMap.empty())
                mChanged = true;
            mCustomSkillsUseSubsections = false;
            return;
        }

        const bool useSubsections = std::any_of(skills.begin(), skills.end(),
            [](const MWBase::LuaManager::CustomSkillForStatsWindow& s) { return !s.mSubsection.empty(); });
        if (useSubsections != mCustomSkillsUseSubsections)
        {
            mChanged = true;
            return;
        }

        if (skills.size() != mCustomSkillWidgetMap.size())
        {
            mChanged = true;
            return;
        }

        for (const MWBase::LuaManager::CustomSkillForStatsWindow& skill : skills)
        {
            if (mCustomSkillWidgetMap.find(skill.mId) == mCustomSkillWidgetMap.end())
            {
                mChanged = true;
                return;
            }
        }

        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        for (const MWBase::LuaManager::CustomSkillForStatsWindow& skill : skills)
        {
            const auto it = mCustomSkillWidgetMap.find(skill.mId);
            if (it == mCustomSkillWidgetMap.end())
                continue;

            MyGUI::TextBox* nameWidget = it->second.first;
            MyGUI::TextBox* valueWidget = it->second.second;
            if (!nameWidget || !valueWidget)
                continue;

            const std::string defaultSubsection = "Misc";
            const std::string& subsection = skill.mSubsection.empty() ? defaultSubsection : skill.mSubsection;

            const auto updateToolTipStrings = [&](MyGUI::Widget* widget) {
                widget->setUserString("ToolTipDynamic", "Stats");
                widget->setUserString("ToolTipType", "Layout");
                widget->setUserString("ToolTipLayout", "SkillToolTip");
                widget->setUserString("Caption_SkillName", MyGUI::TextIterator::toTagsString(skill.mName));
                widget->setUserString("Caption_SkillDescription", skill.mDescription);
                widget->setUserString("ImageTexture_SkillImage", skill.mIconPath);
                widget->setUserString("Range_SkillProgress", "100");

                std::string attributeText;
                if (!skill.mAttributeId.empty())
                {
                    const ESM::RefId attrId = ESM::RefId::deserializeText(skill.mAttributeId);
                    if (const ESM::Attribute* attr = esmStore.get<ESM::Attribute>().search(attrId))
                    {
                        attributeText = "#{sGoverningAttribute}: " + MyGUI::TextIterator::toTagsString(attr->mName);
                    }
                }
                widget->setUserString("Caption_SkillAttribute", attributeText);
            };

            updateToolTipStrings(nameWidget);
            updateToolTipStrings(valueWidget);

            if (nameWidget->isUserString("UserData^CustomSkillSubsection")
                && nameWidget->getUserString("UserData^CustomSkillSubsection") != subsection)
            {
                mChanged = true;
                return;
            }
            nameWidget->setUserString("UserData^CustomSkillSubsection", subsection);
            valueWidget->setUserString("UserData^CustomSkillSubsection", subsection);

            const int modified = skill.mModified;
            const int base = skill.mBase;
            std::string text = MyGUI::utility::toString(modified);
            std::string state = "normal";
            if (modified > base)
                state = "increased";
            else if (modified < base)
                state = "decreased";

            const int widthBefore = valueWidget->getTextSize().width;

            valueWidget->setCaption(text);
            valueWidget->_setWidgetState(state);
            setControllerWidgetBaseState(valueWidget, state);

            const int widthAfter = valueWidget->getTextSize().width;
            if (widthBefore != widthAfter)
            {
                valueWidget->setCoord(valueWidget->getLeft() - (widthAfter - widthBefore), valueWidget->getTop(),
                    valueWidget->getWidth() + (widthAfter - widthBefore), valueWidget->getHeight());
                nameWidget->setSize(nameWidget->getWidth() - (widthAfter - widthBefore), nameWidget->getHeight());
            }

            const bool hasMax = skill.mMaxLevel >= 0;
            const bool isMaxed = hasMax && base >= skill.mMaxLevel;
            if (!isMaxed)
            {
                nameWidget->setUserString("Visible_SkillMaxed", "false");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                nameWidget->setUserString("Visible_SkillProgressVBox", "true");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                valueWidget->setUserString("Visible_SkillMaxed", "false");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "true");
                valueWidget->setUserString("Visible_SkillProgressVBox", "true");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "false");

                setCustomSkillProgress(nameWidget, std::clamp(skill.mProgress, 0.f, 1.f));
                setCustomSkillProgress(valueWidget, std::clamp(skill.mProgress, 0.f, 1.f));
            }
            else
            {
                nameWidget->setUserString("Visible_SkillMaxed", "true");
                nameWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                nameWidget->setUserString("Visible_SkillProgressVBox", "false");
                nameWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");

                valueWidget->setUserString("Visible_SkillMaxed", "true");
                valueWidget->setUserString("UserData^Hidden_SkillMaxed", "false");
                valueWidget->setUserString("Visible_SkillProgressVBox", "false");
                valueWidget->setUserString("UserData^Hidden_SkillProgressVBox", "true");
            }

            const std::string label = nameWidget->isUserString("Caption_SkillName")
                ? std::string(nameWidget->getUserString("Caption_SkillName"))
                : nameWidget->getCaption().asUTF8();
            nameWidget->setUserString("CollapsedLabel", label);
            valueWidget->setUserString("CollapsedLabel", label);
            nameWidget->setUserString("CollapsedValue", text);
            valueWidget->setUserString("CollapsedValue", text);
        }
    }

    void StatsWindow::setFactions(const FactionList& factions)
    {
        if (mFactions != factions)
        {
            mFactions = factions;
            mChanged = true;
        }
    }

    void StatsWindow::setExpelled(const std::set<ESM::RefId>& expelled)
    {
        if (mExpelled != expelled)
        {
            mExpelled = expelled;
            mChanged = true;
        }
    }

    void StatsWindow::setBirthSign(const ESM::RefId& signId)
    {
        if (signId != mBirthSignId)
        {
            mBirthSignId = signId;
            mChanged = true;
        }
    }

    void StatsWindow::addSeparator(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::ImageBox* separator = mSkillView->createWidget<MyGUI::ImageBox>("MW_HLine",
            MyGUI::IntCoord(10, coord1.top, coord1.width + coord2.width - 4, 18),
            MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        separator->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);
        mSkillWidgets.push_back(separator);

        coord1.top += separator->getHeight();
        coord2.top += separator->getHeight();
    }

    MyGUI::Widget* StatsWindow::addGroup(std::string_view label, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        MyGUI::TextBox* groupWidget = mSkillView->createWidget<MyGUI::TextBox>("SandBrightText",
            MyGUI::IntCoord(0, coord1.top, coord1.width + coord2.width, coord1.height),
            MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        groupWidget->setCaption(MyGUI::UString(label));
        groupWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);
        mSkillWidgets.push_back(groupWidget);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;
        return groupWidget;
    }

    std::pair<MyGUI::TextBox*, MyGUI::TextBox*> StatsWindow::addValueItem(std::string_view text,
        const std::string& value, const std::string& state, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2,
        MyGUI::Widget* groupHeader)
    {
        MyGUI::TextBox *skillNameWidget, *skillValueWidget;

        skillNameWidget = mSkillView->createWidget<MyGUI::TextBox>(
            "SandTextController", coord1, MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
        skillNameWidget->setCaption(MyGUI::UString(text));
        skillNameWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        skillValueWidget = mSkillView->createWidget<MyGUI::TextBox>(
            "SandTextRightController", coord2, MyGUI::Align::Right | MyGUI::Align::Top);
        skillValueWidget->setCaption(value);
        skillValueWidget->_setWidgetState(state);
        skillValueWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        MyGUI::Widget* highlight = nullptr;
        if (useControllerSelectionHighlight())
        {
            highlight = mSkillView->createWidget<MyGUI::Widget>("ControllerHighlight",
                MyGUI::IntCoord(coord1.left, coord1.top, coord1.width + coord2.width, coord1.height),
                MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
            highlight->setNeedMouseFocus(false);
            highlight->setDepth(1);
            highlight->setVisible(false);
        }

        // resize dynamically according to text size
        int textWidthPlusMargin = skillValueWidget->getTextSize().width + 12;
        skillValueWidget->setCoord(
            coord2.left + coord2.width - textWidthPlusMargin, coord2.top, textWidthPlusMargin, coord2.height);
        skillNameWidget->setSize(skillNameWidget->getSize() + MyGUI::IntSize(coord2.width - textWidthPlusMargin, 0));

        mSkillWidgets.push_back(skillNameWidget);
        mSkillWidgets.push_back(skillValueWidget);
        addControllerItem(true, skillNameWidget, { skillNameWidget, skillValueWidget }, highlight, false, groupHeader);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;

        return std::make_pair(skillNameWidget, skillValueWidget);
    }

    MyGUI::Widget* StatsWindow::addItem(
        const std::string& text, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2, MyGUI::Widget* groupHeader)
    {
        MyGUI::TextBox* skillNameWidget;

        skillNameWidget = mSkillView->createWidget<MyGUI::TextBox>("SandTextController", coord1, MyGUI::Align::Default);

        skillNameWidget->setCaption(text);
        skillNameWidget->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);

        int textWidth = skillNameWidget->getTextSize().width;
        skillNameWidget->setSize(textWidth, skillNameWidget->getHeight());

        mSkillWidgets.push_back(skillNameWidget);
        MyGUI::Widget* highlight = nullptr;
        if (useControllerSelectionHighlight())
        {
            highlight = mSkillView->createWidget<MyGUI::Widget>("ControllerHighlight",
                MyGUI::IntCoord(coord1.left, coord1.top, coord1.width + coord2.width, coord1.height),
                MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
            highlight->setNeedMouseFocus(false);
            highlight->setDepth(1);
            highlight->setVisible(false);
        }

        addControllerItem(true, skillNameWidget, { skillNameWidget }, highlight, false, groupHeader);

        const int lineHeight = Settings::gui().mFontSize + 2;
        coord1.top += lineHeight;
        coord2.top += lineHeight;

        return skillNameWidget;
    }

    void StatsWindow::addSkills(const std::vector<ESM::RefId>& skills, const std::string& titleId,
        const std::string& titleDefault, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        // Add a line separator if there are items above
        if (!mSkillWidgets.empty())
        {
            addSeparator(coord1, coord2);
        }

        MyGUI::Widget* groupWidget = addGroup(
            MWBase::Environment::get().getWindowManager()->getGameSettingString(titleId, titleDefault), coord1, coord2);

        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();
        for (const ESM::RefId& skillId : skills)
        {
            const ESM::Skill* skill = esmStore.get<ESM::Skill>().search(skillId);
            if (!skill) // Skip unknown skills
                continue;

            auto skillValue = mSkillValues.find(skill->mId);
            if (skillValue == mSkillValues.end())
            {
                Log(Debug::Error) << "Failed to update stats window: can not find value for skill " << skill->mId;
                continue;
            }

            const ESM::Attribute* attr
                = esmStore.get<ESM::Attribute>().find(ESM::Attribute::indexToRefId(skill->mData.mAttribute));

            std::pair<MyGUI::TextBox*, MyGUI::TextBox*> widgets
                = addValueItem(skill->mName, {}, "normal", coord1, coord2, groupWidget);
            mSkillWidgetMap[skill->mId] = std::move(widgets);

            for (int i = 0; i < 2; ++i)
            {
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipDynamic", "Stats");
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "SkillToolTip");
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                    "Caption_SkillName", MyGUI::TextIterator::toTagsString(skill->mName));
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                    "Caption_SkillDescription", skill->mDescription);
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Caption_SkillAttribute",
                    "#{sGoverningAttribute}: " + MyGUI::TextIterator::toTagsString(attr->mName));
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ImageTexture_SkillImage", skill->mIcon);
                mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("Range_SkillProgress", "100");
            }

            setValue(skill->mId, skillValue->second);
        }
    }

    void StatsWindow::addCustomSkills(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2)
    {
        mCustomSkillsUseSubsections = false;

        MWBase::LuaManager* luaManager = MWBase::Environment::get().getLuaManager();
        if (!luaManager)
            return;

        std::vector<MWBase::LuaManager::CustomSkillForStatsWindow> skills = luaManager->getCustomSkillsForStatsWindow();
        skills.erase(std::remove_if(skills.begin(), skills.end(),
                         [](const MWBase::LuaManager::CustomSkillForStatsWindow& s) {
                             return !s.mVisible || s.mId.empty() || s.mName.empty();
                         }),
            skills.end());

        if (skills.empty())
            return;

        mCustomSkillsUseSubsections = std::any_of(skills.begin(), skills.end(),
            [](const MWBase::LuaManager::CustomSkillForStatsWindow& s) { return !s.mSubsection.empty(); });

        if (!mSkillWidgets.empty())
            addSeparator(coord1, coord2);

        MyGUI::Widget* groupWidget = addGroup(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sCustomSkills", "Other Skills"),
            coord1, coord2);

        const MWWorld::ESMStore& esmStore = *MWBase::Environment::get().getESMStore();

        constexpr int subsectionHeaderIndent = 10;
        constexpr int subsectionItemExtraIndent = 10;
        const std::string defaultSubsection = "Misc";

        const auto addIndentedGroup = [&](std::string_view label, int indent) -> MyGUI::Widget* {
            MyGUI::TextBox* groupHeader = mSkillView->createWidget<MyGUI::TextBox>("SandBrightText",
                MyGUI::IntCoord(indent, coord1.top, coord1.width + coord2.width - indent, coord1.height),
                MyGUI::Align::Left | MyGUI::Align::Top | MyGUI::Align::HStretch);
            groupHeader->setCaption(MyGUI::UString(label));
            groupHeader->eventMouseWheel += MyGUI::newDelegate(this, &StatsWindow::onMouseWheel);
            mSkillWidgets.push_back(groupHeader);

            const int lineHeight = Settings::gui().mFontSize + 2;
            coord1.top += lineHeight;
            coord2.top += lineHeight;
            return groupHeader;
        };

        std::sort(skills.begin(), skills.end(),
            [useSubsections = mCustomSkillsUseSubsections](const MWBase::LuaManager::CustomSkillForStatsWindow& a,
                const MWBase::LuaManager::CustomSkillForStatsWindow& b) {
                if (useSubsections)
                {
                    if (a.mSubsection != b.mSubsection)
                        return a.mSubsection < b.mSubsection;
                }
                if (a.mName != b.mName)
                    return a.mName < b.mName;
                return a.mId < b.mId;
            });

        MyGUI::Widget* currentGroupHeader = groupWidget;
        std::string currentSubsection;

        for (const MWBase::LuaManager::CustomSkillForStatsWindow& skill : skills)
        {
            const std::string& subsection = skill.mSubsection.empty() ? defaultSubsection : skill.mSubsection;
            if (mCustomSkillsUseSubsections && subsection != currentSubsection)
            {
                currentSubsection = subsection;
                currentGroupHeader = addIndentedGroup(currentSubsection, subsectionHeaderIndent);
            }

            const int originalLeft = coord1.left;
            const int originalWidth = coord1.width;
            if (mCustomSkillsUseSubsections)
            {
                coord1.left += subsectionItemExtraIndent;
                coord1.width -= subsectionItemExtraIndent;
            }

            std::pair<MyGUI::TextBox*, MyGUI::TextBox*> widgets
                = addValueItem(skill.mName, {}, "normal", coord1, coord2, currentGroupHeader);
            mCustomSkillWidgetMap[skill.mId] = widgets;

            coord1.left = originalLeft;
            coord1.width = originalWidth;

            const auto updateToolTipStrings = [&](MyGUI::Widget* widget) {
                widget->setUserString("ToolTipDynamic", "Stats");
                widget->setUserString("ToolTipType", "Layout");
                widget->setUserString("ToolTipLayout", "SkillToolTip");
                widget->setUserString("Caption_SkillName", MyGUI::TextIterator::toTagsString(skill.mName));
                widget->setUserString("Caption_SkillDescription", skill.mDescription);
                widget->setUserString("ImageTexture_SkillImage", skill.mIconPath);
                widget->setUserString("Range_SkillProgress", "100");

                std::string attributeText;
                if (!skill.mAttributeId.empty())
                {
                    const ESM::RefId attrId = ESM::RefId::deserializeText(skill.mAttributeId);
                    if (const ESM::Attribute* attr = esmStore.get<ESM::Attribute>().search(attrId))
                    {
                        attributeText = "#{sGoverningAttribute}: " + MyGUI::TextIterator::toTagsString(attr->mName);
                    }
                }
                widget->setUserString("Caption_SkillAttribute", attributeText);
            };

            updateToolTipStrings(widgets.first);
            updateToolTipStrings(widgets.second);

            widgets.first->setUserString("UserData^CustomSkillSubsection", subsection);
            widgets.second->setUserString("UserData^CustomSkillSubsection", subsection);
        }

        updateCustomSkillsFromLua();
    }

    void StatsWindow::updateSkillArea()
    {
        mChanged = false;

        resetControllerItems();

        for (MyGUI::Widget* widget : mSkillWidgets)
        {
            MyGUI::Gui::getInstance().destroyWidget(widget);
        }
        mSkillWidgets.clear();
        mCustomSkillWidgetMap.clear();

        const int valueSize = 40;
        MyGUI::IntCoord coord1(10, 0, mSkillView->getWidth() - (10 + valueSize) - 24, 18);
        MyGUI::IntCoord coord2(coord1.left + coord1.width, coord1.top, valueSize, coord1.height);

        if (!mMajorSkills.empty())
            addSkills(mMajorSkills, "sSkillClassMajor", "Major Skills", coord1, coord2);

        if (!mMinorSkills.empty())
            addSkills(mMinorSkills, "sSkillClassMinor", "Minor Skills", coord1, coord2);

        if (!mMiscSkills.empty())
            addSkills(mMiscSkills, "sSkillClassMisc", "Misc Skills", coord1, coord2);

        addCustomSkills(coord1, coord2);

        MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore& store = world->getStore();
        const ESM::NPC* player = world->getPlayerPtr().get<ESM::NPC>()->mBase;

        // race tooltip
        const ESM::Race* playerRace = store.get<ESM::Race>().find(player->mRace);

        MyGUI::Widget* raceWidget;
        getWidget(raceWidget, "RaceText");
        ToolTips::createRaceToolTip(raceWidget, playerRace);
        getWidget(raceWidget, "Race_str");
        ToolTips::createRaceToolTip(raceWidget, playerRace);

        // class tooltip
        MyGUI::Widget* classWidget;

        const ESM::Class* playerClass = store.get<ESM::Class>().find(player->mClass);

        getWidget(classWidget, "ClassText");
        ToolTips::createClassToolTip(classWidget, *playerClass);
        getWidget(classWidget, "Class_str");
        ToolTips::createClassToolTip(classWidget, *playerClass);

        if (!mFactions.empty())
        {
            MWWorld::Ptr playerPtr = MWMechanics::getPlayer();
            const MWMechanics::NpcStats& playerStats = playerPtr.getClass().getNpcStats(playerPtr);
            const std::set<ESM::RefId>& expelled = playerStats.getExpelled();

            bool firstFaction = true;
            for (const auto& [factionId, factionRank] : mFactions)
            {
                const ESM::Faction* faction = store.get<ESM::Faction>().find(factionId);
                if (faction->mData.mIsHidden == 1)
                    continue;

                if (firstFaction)
                {
                    // Add a line separator if there are items above
                    if (!mSkillWidgets.empty())
                        addSeparator(coord1, coord2);

                    addGroup(MWBase::Environment::get().getWindowManager()->getGameSettingString("sFaction", "Faction"),
                        coord1, coord2);

                    firstFaction = false;
                }

                MyGUI::Widget* w = addItem(faction->mName, coord1, coord2);

                std::string text;

                text += std::string("#{fontcolourhtml=header}") + faction->mName;

                if (expelled.find(factionId) != expelled.end())
                    text += "\n#{fontcolourhtml=normal}#{sExpelled}";
                else
                {
                    const auto rank = static_cast<size_t>(std::max(0, factionRank));
                    if (rank < faction->mRanks.size())
                        text += std::string("\n#{fontcolourhtml=normal}") + faction->mRanks[rank];
                    if (rank + 1 < faction->mRanks.size() && !faction->mRanks[rank + 1].empty())
                    {
                        // player doesn't have max rank yet
                        text += std::string("\n\n#{fontcolourhtml=header}#{sNextRank} ") + faction->mRanks[rank + 1];

                        const ESM::RankData& rankData = faction->mData.mRankData[rank + 1];
                        const ESM::Attribute* attr1 = store.get<ESM::Attribute>().find(
                            ESM::Attribute::indexToRefId(faction->mData.mAttribute[0]));
                        const ESM::Attribute* attr2 = store.get<ESM::Attribute>().find(
                            ESM::Attribute::indexToRefId(faction->mData.mAttribute[1]));

                        text += "\n#{fontcolourhtml=normal}" + MyGUI::TextIterator::toTagsString(attr1->mName) + ": "
                            + MyGUI::utility::toString(rankData.mAttribute1) + ", "
                            + MyGUI::TextIterator::toTagsString(attr2->mName) + ": "
                            + MyGUI::utility::toString(rankData.mAttribute2);

                        text += "\n\n#{fontcolourhtml=header}#{sFavoriteSkills}";
                        text += "\n#{fontcolourhtml=normal}";
                        bool firstSkill = true;
                        for (int id : faction->mData.mSkills)
                        {
                            const ESM::Skill* skill = store.get<ESM::Skill>().search(ESM::Skill::indexToRefId(id));
                            if (skill)
                            {
                                if (!firstSkill)
                                    text += ", ";

                                firstSkill = false;
                                text += MyGUI::TextIterator::toTagsString(skill->mName);
                            }
                        }

                        text += "\n";

                        if (rankData.mPrimarySkill > 0)
                            text += "\n#{sNeedOneSkill} " + MyGUI::utility::toString(rankData.mPrimarySkill);
                        if (rankData.mFavouredSkill > 0)
                            text += " #{sand} #{sNeedTwoSkills} " + MyGUI::utility::toString(rankData.mFavouredSkill);
                    }
                }

                w->setUserString("ToolTipType", "Layout");
                w->setUserString("ToolTipLayout", "FactionToolTip");
                w->setUserString("Caption_FactionText", text);
                w->setUserString("CollapsedLabel", faction->mName);
                w->setUserString("CollapsedValue", "");
            }
        }

        if (!mBirthSignId.empty())
        {
            // Add a line separator if there are items above
            if (!mSkillWidgets.empty())
                addSeparator(coord1, coord2);

            addGroup(MWBase::Environment::get().getWindowManager()->getGameSettingString("sBirthSign", "Sign"), coord1,
                coord2);
            const ESM::BirthSign* sign = store.get<ESM::BirthSign>().find(mBirthSignId);
            MyGUI::Widget* w = addItem(sign->mName, coord1, coord2);

            ToolTips::createBirthsignToolTip(w, mBirthSignId);
            w->setUserString("ToolTipDynamic", "Stats");
            w->setUserString("CollapsedLabel", sign->mName);
            w->setUserString("CollapsedValue", "");
        }

        // Add a line separator if there are items above
        if (!mSkillWidgets.empty())
            addSeparator(coord1, coord2);

        const std::string reputationLabel = std::string(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sReputation", "Reputation"));
        const std::string reputationValue = MyGUI::utility::toString(static_cast<int>(mReputation));
        const auto reputationWidgets = addValueItem(reputationLabel, reputationValue, "normal", coord1, coord2);
        const std::string reputationCollapsedLabel
            = "#{fontcolourhtml=header}" + reputationLabel + "#{fontcolourhtml=normal}";
        for (MyGUI::TextBox* widget : { reputationWidgets.first, reputationWidgets.second })
        {
            if (!widget)
                continue;
            widget->setUserString("CollapsedLabel", reputationCollapsedLabel);
            widget->setUserString("CollapsedValue", reputationValue);
        }

        for (int i = 0; i < 2; ++i)
        {
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "TextToolTipOneLine");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                "Caption_TextOneLine", "Your fame rating in the world of Morrowind.");
        }

        const std::string bountyLabel
            = std::string(MWBase::Environment::get().getWindowManager()->getGameSettingString("sBounty", "Bounty"));
        const std::string bountyValue = MyGUI::utility::toString(static_cast<int>(mBounty));
        const auto bountyWidgets = addValueItem(bountyLabel, bountyValue, "normal", coord1, coord2);
        const std::string bountyCollapsedLabel = "#{fontcolourhtml=header}" + bountyLabel + "#{fontcolourhtml=normal}";
        for (MyGUI::TextBox* widget : { bountyWidgets.first, bountyWidgets.second })
        {
            if (!widget)
                continue;
            widget->setUserString("CollapsedLabel", bountyCollapsedLabel);
            widget->setUserString("CollapsedValue", bountyValue);
        }

        for (int i = 0; i < 2; ++i)
        {
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipType", "Layout");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString("ToolTipLayout", "TextToolTipOneLine");
            mSkillWidgets[mSkillWidgets.size() - 1 - i]->setUserString(
                "Caption_TextOneLine", "The penalty you must pay for your crimes, if caught.");
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mSkillView->setVisibleVScroll(false);
        mSkillView->setCanvasSize(mSkillView->getWidth(), std::max(mSkillView->getHeight(), coord1.top));
        mSkillView->setVisibleVScroll(true);
        mLastSkillViewSize = mSkillView->getSize();

        if (Settings::gui().mControllerMenus && mActiveControllerWindow && !mControllerItems.empty())
        {
            mControllerFocus = std::min(mControllerFocus, mControllerItems.size() - 1);
            updateControllerFocus(mControllerItems.size(), mControllerFocus);
        }
    }

    void StatsWindow::resetFixedWindowGeometry()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            window->setCoord(getFixedWindowCoord(viewSize));
        }
    }

    void StatsWindow::onPinToggled()
    {
        Settings::windows().mStatsPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setHMSVisibility(!mPinned);
    }

    void StatsWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
        {
            MWBase::Environment::get().getWindowManager()->toggleMaximized(this);
            MyGUI::Window* t = mMainWidget->castType<MyGUI::Window>();
            onWindowResize(t);
        }
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Stats);
    }

    bool StatsWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (mControllerItems.empty())
                return true;

            const bool rightPane = mControllerItems[mControllerFocus].mRightPane;
            MWBase::Environment::get().getWindowManager()->restoreControllerTooltips();
            const std::vector<size_t>& list = rightPane ? mControllerRightItems : mControllerLeftItems;
            if (list.empty())
                return true;

            const auto it = std::find(list.begin(), list.end(), mControllerFocus);
            size_t index = 0;
            if (it != list.end())
                index = static_cast<size_t>(std::distance(list.begin(), it));

            const int delta = (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP) ? -1 : 1;
            index = wrap(index, list.size(), delta);
            setControllerFocus(list[index]);
            if (rightPane)
                mLastLeftFocus = mControllerItems.size();
            else
                mLastRightFocus = mControllerItems.size();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (mControllerItems.empty())
                return true;

            const bool targetRightPane = (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
            MWBase::Environment::get().getWindowManager()->restoreControllerTooltips();
            if (targetRightPane == mControllerItems[mControllerFocus].mRightPane)
                return true;

            const std::vector<size_t>& list = targetRightPane ? mControllerRightItems : mControllerLeftItems;
            size_t newFocus = mControllerItems.size();
            if (!list.empty())
            {
                const size_t remembered = targetRightPane ? mLastRightFocus : mLastLeftFocus;
                if (remembered < mControllerItems.size() && mControllerItems[remembered].mRightPane == targetRightPane
                    && std::find(list.begin(), list.end(), remembered) != list.end())
                {
                    newFocus = remembered;
                }
                else
                {
                    newFocus
                        = findClosestControllerItem(targetRightPane, mControllerItems[mControllerFocus].mTooltipWidget);
                }
            }
            if (newFocus < mControllerItems.size())
                setControllerFocus(newFocus);
        }

        return true;
    }

    void StatsWindow::setActiveControllerWindow(bool active)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            const MyGUI::IntCoord coord = getFixedWindowCoord(viewSize);

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(coord.left, active ? coord.top : viewSize.height + 1, coord.width, coord.height);

            if (active)
                onWindowResize(window);
        }

        WindowBase::setActiveControllerWindow(active);

        if (Settings::gui().mControllerMenus)
        {
            winMgr->setControllerTooltipVisible(active && Settings::gui().mControllerTooltips);
            winMgr->setCursorVisible(false);

            if (active && !mControllerItems.empty())
            {
                mControllerFocus = std::min(mControllerFocus, mControllerItems.size() - 1);
                updateControllerFocus(mControllerItems.size(), mControllerFocus);
                mPendingControllerFocusRefresh = true;
            }
            else if (!active && !mControllerItems.empty())
            {
                updateControllerFocus(mControllerFocus, mControllerItems.size());
            }
        }
    }

    void StatsWindow::resetControllerItems()
    {
        if (mActiveControllerWindow && !mControllerItems.empty())
            updateControllerFocus(mControllerFocus, mControllerItems.size());

        const auto previousStates = mControllerBaseStates;

        mControllerItems.clear();
        mControllerLeftItems.clear();
        mControllerRightItems.clear();
        mControllerBaseStates.clear();

        for (const StaticControllerEntry& entry : mStaticControllerEntries)
            addControllerItem(
                false, entry.mTooltipWidget, entry.mTextWidgets, entry.mHighlight, entry.mHighlightFollowsTooltip);

        for (const AttributeControllerEntry& entry : mAttributeControllerEntries)
        {
            if (auto it = previousStates.find(entry.mNameWidget); it != previousStates.end())
                mControllerBaseStates[entry.mNameWidget] = it->second;
            if (auto it = previousStates.find(entry.mValueWidget); it != previousStates.end())
                mControllerBaseStates[entry.mValueWidget] = it->second;

            addControllerItem(false, entry.mTooltipWidget, { entry.mNameWidget, entry.mValueWidget });
        }
    }

    void StatsWindow::addStaticControllerEntry(MyGUI::Widget* tooltipWidget,
        const std::initializer_list<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight,
        bool highlightFollowsTooltip)
    {
        StaticControllerEntry entry;
        entry.mTooltipWidget = tooltipWidget;
        entry.mHighlight = highlight;
        entry.mHighlightFollowsTooltip = highlightFollowsTooltip;
        entry.mTextWidgets.assign(textWidgets.begin(), textWidgets.end());
        mStaticControllerEntries.push_back(std::move(entry));
    }

    void StatsWindow::addControllerItem(bool rightPane, MyGUI::Widget* tooltipWidget,
        const std::vector<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight, bool highlightFollowsTooltip,
        MyGUI::Widget* groupHeader)
    {
        ControllerItem item;
        item.mTooltipWidget = tooltipWidget;
        item.mHighlight = highlight;
        item.mHighlightFollowsTooltip = highlight == nullptr ? true : highlightFollowsTooltip;
        item.mGroupHeader = groupHeader;
        item.mRightPane = rightPane;
        item.mTextWidgets = textWidgets;

        const size_t index = mControllerItems.size();
        mControllerItems.push_back(item);
        (rightPane ? mControllerRightItems : mControllerLeftItems).push_back(index);

        for (MyGUI::TextBox* widget : item.mTextWidgets)
        {
            if (widget == nullptr)
                continue;
            if (mControllerBaseStates.find(widget) == mControllerBaseStates.end())
                mControllerBaseStates.emplace(widget, "normal");
        }

        if (item.mHighlight == nullptr && useControllerSelectionHighlight() && item.mTooltipWidget != nullptr)
        {
            if (MyGUI::Widget* parent = item.mTooltipWidget->getParent())
            {
                item.mHighlight = parent->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", item.mTooltipWidget->getCoord(), MyGUI::Align::Default);
                item.mHighlight->setNeedMouseFocus(false);
                item.mHighlight->setDepth(1);
                item.mHighlight->setVisible(false);
                item.mHighlightFollowsTooltip = true;
                mControllerItems[index].mHighlight = item.mHighlight;
                mControllerItems[index].mHighlightFollowsTooltip = item.mHighlightFollowsTooltip;
            }
        }

        if (item.mTooltipWidget != nullptr)
            item.mTooltipWidget->setNeedMouseFocus(true);
    }

    void StatsWindow::addControllerItem(bool rightPane, MyGUI::Widget* tooltipWidget,
        const std::initializer_list<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight,
        bool highlightFollowsTooltip, MyGUI::Widget* groupHeader)
    {
        ControllerItem item;
        item.mTooltipWidget = tooltipWidget;
        item.mHighlight = highlight;
        item.mHighlightFollowsTooltip = highlight == nullptr ? true : highlightFollowsTooltip;
        item.mGroupHeader = groupHeader;
        item.mRightPane = rightPane;
        item.mTextWidgets.assign(textWidgets.begin(), textWidgets.end());

        const size_t index = mControllerItems.size();
        mControllerItems.push_back(item);
        (rightPane ? mControllerRightItems : mControllerLeftItems).push_back(index);

        for (MyGUI::TextBox* widget : item.mTextWidgets)
        {
            if (widget == nullptr)
                continue;
            if (mControllerBaseStates.find(widget) == mControllerBaseStates.end())
                mControllerBaseStates.emplace(widget, "normal");
        }

        if (item.mHighlight == nullptr && useControllerSelectionHighlight() && item.mTooltipWidget != nullptr)
        {
            if (MyGUI::Widget* parent = item.mTooltipWidget->getParent())
            {
                item.mHighlight = parent->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", item.mTooltipWidget->getCoord(), MyGUI::Align::Default);
                item.mHighlight->setNeedMouseFocus(false);
                item.mHighlight->setDepth(1);
                item.mHighlight->setVisible(false);
                item.mHighlightFollowsTooltip = true;
                mControllerItems[index].mHighlight = item.mHighlight;
                mControllerItems[index].mHighlightFollowsTooltip = item.mHighlightFollowsTooltip;
            }
        }

        if (item.mTooltipWidget != nullptr)
            item.mTooltipWidget->setNeedMouseFocus(true);
    }

    void StatsWindow::setControllerWidgetBaseState(MyGUI::TextBox* widget, std::string_view state)
    {
        if (widget == nullptr)
            return;

        auto it = mControllerBaseStates.find(widget);
        if (it != mControllerBaseStates.end())
            it->second = std::string(state);
        else
            mControllerBaseStates.emplace(widget, std::string(state));

        if (useControllerSelectionHighlight() && isControllerWidgetFocused(widget))
            widget->_setWidgetState("highlighted");
    }

    MyGUI::IntCoord StatsWindow::getFixedWindowCoord(const MyGUI::IntSize& viewSize) const
    {
        const float scale = 0.85f;
        float width = viewSize.width * scale;
        float height = width * 10.f / 16.f;
        const float maxHeight = viewSize.height * scale;
        if (height > maxHeight)
        {
            height = maxHeight;
            width = height * 16.f / 10.f;
        }

        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        const int x = (viewSize.width - w) / 2;
        const int y = (viewSize.height - h) / 2;
        return { x, y, w, h };
    }

    MyGUI::Widget* StatsWindow::getControllerFocusTooltipWidget() const
    {
        if (!mActiveControllerWindow || mControllerItems.empty())
            return nullptr;
        if (mControllerFocus >= mControllerItems.size())
            return nullptr;
        return mControllerItems[mControllerFocus].mTooltipWidget;
    }

    void StatsWindow::updateControllerFocus(size_t prevFocus, size_t newFocus)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (Settings::gui().mControllerMenus)
            winMgr->setCursorVisible(false);

        if (prevFocus < mControllerItems.size())
        {
            for (MyGUI::TextBox* widget : mControllerItems[prevFocus].mTextWidgets)
            {
                if (widget == nullptr)
                    continue;
                const auto it = mControllerBaseStates.find(widget);
                widget->_setWidgetState(it != mControllerBaseStates.end() ? it->second : "normal");
            }

            if (MyGUI::Widget* highlight = mControllerItems[prevFocus].mHighlight)
                highlight->setVisible(false);
        }

        if (!mActiveControllerWindow || newFocus >= mControllerItems.size())
            return;

        const ControllerItem& item = mControllerItems[newFocus];

        if (useControllerSelectionHighlight())
        {
            for (MyGUI::TextBox* widget : item.mTextWidgets)
            {
                if (widget != nullptr)
                    widget->_setWidgetState("highlighted");
            }

            if (item.mHighlight != nullptr)
            {
                if (item.mHighlightFollowsTooltip && item.mTooltipWidget != nullptr)
                    item.mHighlight->setCoord(item.mTooltipWidget->getCoord());
                item.mHighlight->setVisible(true);
            }
        }

        if (item.mRightPane && item.mTooltipWidget != nullptr)
            scrollSkillViewToWidget(item.mTooltipWidget, item.mGroupHeader);

        winMgr->restoreControllerTooltips();
    }

    bool StatsWindow::isControllerWidgetFocused(MyGUI::TextBox* widget) const
    {
        if (widget == nullptr || mControllerItems.empty())
            return false;
        if (mControllerFocus >= mControllerItems.size())
            return false;
        const ControllerItem& item = mControllerItems[mControllerFocus];
        return std::find(item.mTextWidgets.begin(), item.mTextWidgets.end(), widget) != item.mTextWidgets.end();
    }

    void StatsWindow::scrollSkillViewToWidget(MyGUI::Widget* widget, MyGUI::Widget* headerWidget)
    {
        if (widget == nullptr || mSkillView == nullptr)
            return;

        const MyGUI::IntCoord itemCoord = widget->getAbsoluteCoord();
        const MyGUI::IntCoord viewCoord = mSkillView->getAbsoluteCoord();
        const MyGUI::IntCoord headerCoord = headerWidget ? headerWidget->getAbsoluteCoord() : itemCoord;

        const bool itemAboveView = itemCoord.top < viewCoord.top;
        const bool itemBelowView = itemCoord.top + itemCoord.height > viewCoord.top + viewCoord.height;

        int newOffset = mSkillView->getViewOffset().top;
        if (itemAboveView && headerWidget != nullptr && headerCoord.top < viewCoord.top)
        {
            newOffset += viewCoord.top - headerCoord.top;
        }
        else if (itemAboveView)
        {
            newOffset += viewCoord.top - itemCoord.top;
        }
        else if (itemBelowView)
        {
            newOffset -= (itemCoord.top + itemCoord.height) - (viewCoord.top + viewCoord.height);
        }
        else
            return;

        const int minOffset = std::min(0, mSkillView->getHeight() - mSkillView->getCanvasSize().height);
        newOffset = std::clamp(newOffset, minOffset, 0);
        mSkillView->setViewOffset(MyGUI::IntPoint(0, newOffset));
    }

    size_t StatsWindow::findClosestControllerItem(bool rightPane, MyGUI::Widget* widget) const
    {
        const std::vector<size_t>& list = rightPane ? mControllerRightItems : mControllerLeftItems;
        if (list.empty() || widget == nullptr)
            return mControllerFocus;

        const int targetTop = widget->getAbsoluteCoord().top;
        size_t bestIndex = list.front();
        int bestDistance = std::numeric_limits<int>::max();

        for (size_t index : list)
        {
            const ControllerItem& item = mControllerItems[index];
            if (item.mTooltipWidget == nullptr)
                continue;

            const int itemTop = item.mTooltipWidget->getAbsoluteCoord().top;
            const int distance = std::abs(itemTop - targetTop);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = index;
            }
        }

        return bestIndex;
    }

    void StatsWindow::setControllerFocus(size_t newFocus)
    {
        if (newFocus >= mControllerItems.size())
            return;

        const size_t prevFocus = mControllerFocus;
        mControllerFocus = newFocus;
        updateControllerFocus(prevFocus, mControllerFocus);
        if (mControllerItems[mControllerFocus].mRightPane)
            mLastRightFocus = mControllerFocus;
        else
            mLastLeftFocus = mControllerFocus;
    }
}
