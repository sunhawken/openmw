#ifndef MWGUI_STATS_WINDOW_H
#define MWGUI_STATS_WINDOW_H

#include "statswatcher.hpp"
#include "windowpinnablebase.hpp"
#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <unordered_map>

namespace MWGui
{
    class StatsWindow : public WindowPinnableBase, public NoDrop, public StatsListener
    {
    public:
        typedef std::map<ESM::RefId, int> FactionList;

        /// It would be nice to measure these, but for now they're hardcoded.
        static int getIdealHeight() { return 750; }
        static int getIdealWidth() { return 600; }

        StatsWindow(DragAndDrop* drag);

        /// automatically updates all the data in the stats window, but only if it has changed.
        void onFrame(float dt) override;

        void setBar(const std::string& name, const std::string& tname, int val, int max);
        void setPlayerName(const std::string& playerName);

        /// Set value for the given ID.
        void setAttribute(ESM::RefId id, const MWMechanics::AttributeValue& value) override;
        void setValue(std::string_view id, const MWMechanics::DynamicStat<float>& value) override;
        void setValue(std::string_view id, const std::string& value) override;
        void setValue(std::string_view id, int value) override;
        void setValue(ESM::RefId id, const MWMechanics::SkillValue& value) override;
        void configureSkills(const std::vector<ESM::RefId>& major, const std::vector<ESM::RefId>& minor) override;

        void setReputation(int reputation)
        {
            if (reputation != mReputation)
                mChanged = true;
            this->mReputation = reputation;
        }
        void setBounty(int bounty)
        {
            if (bounty != mBounty)
                mChanged = true;
            this->mBounty = bounty;
        }
        void updateSkillArea();

        void onOpen() override;

        std::string_view getWindowIdForLua() const override { return "Stats"; }
        MyGUI::Widget* getControllerFocusTooltipWidget() const;
        ControllerButtons* getControllerButtons() override;

    protected:
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        void setActiveControllerWindow(bool active) override;

    private:
        struct ControllerItem
        {
            MyGUI::Widget* mTooltipWidget = nullptr;
            MyGUI::Widget* mHighlight = nullptr;
            bool mHighlightFollowsTooltip = true;
            MyGUI::Widget* mGroupHeader = nullptr;
            std::vector<MyGUI::TextBox*> mTextWidgets;
            bool mRightPane = false;
        };

        struct AttributeControllerEntry
        {
            MyGUI::Widget* mTooltipWidget = nullptr;
            MyGUI::TextBox* mNameWidget = nullptr;
            MyGUI::TextBox* mValueWidget = nullptr;
        };

        struct StaticControllerEntry
        {
            MyGUI::Widget* mTooltipWidget = nullptr;
            MyGUI::Widget* mHighlight = nullptr;
            bool mHighlightFollowsTooltip = true;
            std::vector<MyGUI::TextBox*> mTextWidgets;
        };

        void addSkills(const std::vector<ESM::RefId>& skills, const std::string& titleId,
            const std::string& titleDefault, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        void addCustomSkills(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        void addSeparator(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        MyGUI::Widget* addGroup(std::string_view label, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        std::pair<MyGUI::TextBox*, MyGUI::TextBox*> addValueItem(std::string_view text, const std::string& value,
            const std::string& state, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2,
            MyGUI::Widget* groupHeader = nullptr);
        MyGUI::Widget* addItem(const std::string& text, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2,
            MyGUI::Widget* groupHeader = nullptr);

        void updateCustomSkillsFromLua();

        void setFactions(const FactionList& factions);
        void setExpelled(const std::set<ESM::RefId>& expelled);
        void setBirthSign(const ESM::RefId& signId);

        void onWindowResize(MyGUI::Window* window);
        void onMouseWheel(MyGUI::Widget* sender, int rel);
        void resetControllerItems();
        void addStaticControllerEntry(MyGUI::Widget* tooltipWidget,
            const std::initializer_list<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight = nullptr,
            bool highlightFollowsTooltip = true);
        void addControllerItem(bool rightPane, MyGUI::Widget* tooltipWidget,
            const std::initializer_list<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight = nullptr,
            bool highlightFollowsTooltip = true, MyGUI::Widget* groupHeader = nullptr);
        void addControllerItem(bool rightPane, MyGUI::Widget* tooltipWidget,
            const std::vector<MyGUI::TextBox*>& textWidgets, MyGUI::Widget* highlight = nullptr,
            bool highlightFollowsTooltip = true, MyGUI::Widget* groupHeader = nullptr);
        void setControllerWidgetBaseState(MyGUI::TextBox* widget, std::string_view state);
        void updateControllerFocus(size_t prevFocus, size_t newFocus);
        bool isControllerWidgetFocused(MyGUI::TextBox* widget) const;
        void scrollSkillViewToWidget(MyGUI::Widget* widget, MyGUI::Widget* headerWidget);
        size_t findClosestControllerItem(bool rightPane, MyGUI::Widget* widget) const;
        void setControllerFocus(size_t newFocus);
        void updateStaticHighlights();

        MyGUI::Widget* mLeftPane;
        MyGUI::Widget* mRightPane;

        MyGUI::ScrollView* mSkillView;

        std::vector<ESM::RefId> mMajorSkills, mMinorSkills, mMiscSkills;
        std::map<ESM::RefId, MWMechanics::SkillValue> mSkillValues;
        std::map<ESM::RefId, MyGUI::TextBox*> mAttributeWidgets;
        std::map<ESM::RefId, std::pair<MyGUI::TextBox*, MyGUI::TextBox*>> mSkillWidgetMap;
        std::map<std::string, std::pair<MyGUI::TextBox*, MyGUI::TextBox*>> mCustomSkillWidgetMap;
        std::map<std::string, MyGUI::Widget*> mFactionWidgetMap;
        FactionList mFactions; ///< Stores a list of factions and the current rank
        ESM::RefId mBirthSignId;
        int mReputation, mBounty;
        std::vector<MyGUI::Widget*> mSkillWidgets; //< Skills and other information
        std::set<ESM::RefId> mExpelled;

        bool mChanged;
        bool mCustomSkillsUseSubsections = false;
        const int mMinFullWidth;
        size_t mControllerFocus = 0;
        std::vector<ControllerItem> mControllerItems;
        std::vector<size_t> mControllerLeftItems;
        std::vector<size_t> mControllerRightItems;
        size_t mLastLeftFocus = 0;
        size_t mLastRightFocus = 0;
        std::unordered_map<MyGUI::TextBox*, std::string> mControllerBaseStates;
        std::vector<AttributeControllerEntry> mAttributeControllerEntries;
        std::vector<StaticControllerEntry> mStaticControllerEntries;
        bool mPendingControllerFocusRefresh = false;
        MyGUI::Widget* mLevelHighlight = nullptr;
        MyGUI::Widget* mRaceHighlight = nullptr;
        MyGUI::Widget* mClassHighlight = nullptr;
        MyGUI::Widget* mNameHighlight = nullptr;
        MyGUI::Widget* mLevelRow = nullptr;
        MyGUI::Widget* mRaceRow = nullptr;
        MyGUI::Widget* mClassRow = nullptr;
        MyGUI::Widget* mNameRow = nullptr;
        MyGUI::IntSize mLastSkillViewSize = MyGUI::IntSize(0, 0);

    protected:
        void onPinToggled() override;
        void onTitleDoubleClicked() override;
        void resetFixedWindowGeometry();
        MyGUI::IntCoord getFixedWindowCoord(const MyGUI::IntSize& viewSize) const;
    };
}
#endif
