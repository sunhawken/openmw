#ifndef MWGUI_REVIEW_H
#define MWGUI_REVIEW_H

#include "widgets.hpp"
#include "windowbase.hpp"
#include <components/esm/refid.hpp>
#include <components/esm3/loadclas.hpp>
#include <unordered_map>
#include <utility>

namespace MyGUI
{
    class ScrollView;
    class TextBox;
    class Widget;
}

namespace ESM
{
    struct Spell;
}

namespace MWGui
{
    class ReviewDialog : public WindowModal
    {
    public:
        enum Dialogs
        {
            NAME_DIALOG,
            RACE_DIALOG,
            CLASS_DIALOG,
            BIRTHSIGN_DIALOG
        };

        ReviewDialog();

        bool exit() override { return false; }

        void setPlayerName(const std::string& name);
        void setRace(const ESM::RefId& raceId);
        void setClass(const ESM::Class& playerClass);
        void setBirthSign(const ESM::RefId& signId);

        void setHealth(const MWMechanics::DynamicStat<float>& value);
        void setMagicka(const MWMechanics::DynamicStat<float>& value);
        void setFatigue(const MWMechanics::DynamicStat<float>& value);

        void setAttribute(ESM::RefId attributeId, const MWMechanics::AttributeValue& value);

        void configureSkills(const std::vector<ESM::RefId>& major, const std::vector<ESM::RefId>& minor);
        void setSkillValue(ESM::RefId id, const MWMechanics::SkillValue& value);

        void onOpen() override;
        void onClose() override;

        void onFrame(float duration) override;
        void setActiveControllerWindow(bool active) override;
        MyGUI::Widget* getControllerFocusTooltipWidget() const;

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;
        typedef MyGUI::delegates::MultiDelegate<int> EventHandle_Int;

        /** Event : Back button clicked.\n
        signature : void method()\n
        */
        EventHandle_Void eventBack;

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

        EventHandle_Int eventActivateDialog;

    protected:
        void onOkClicked(MyGUI::Widget* sender);
        void onBackClicked(MyGUI::Widget* sender);

        void onNameClicked(MyGUI::Widget* sender);
        void onRaceClicked(MyGUI::Widget* sender);
        void onClassClicked(MyGUI::Widget* sender);
        void onBirthSignClicked(MyGUI::Widget* sender);

        void onMouseWheel(MyGUI::Widget* sender, int rel);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        bool onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg) override;

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

        void addSkills(const std::vector<ESM::RefId>& skills, const std::string& titleId,
            const std::string& titleDefault, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        void addSeparator(MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        MyGUI::Widget* addGroup(std::string_view label, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        std::pair<MyGUI::TextBox*, MyGUI::TextBox*> addValueItem(std::string_view text, const std::string& value,
            const std::string& state, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        MyGUI::TextBox* addItem(const std::string& text, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        MyGUI::Widget* addItem(const ESM::Spell* spell, MyGUI::IntCoord& coord1, MyGUI::IntCoord& coord2);
        void updateSkillArea();
        void resetControllerItems();
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

        MyGUI::TextBox *mNameLabel, *mRaceLabel, *mClassLabel, *mBirthSignLabel;
        MyGUI::TextBox *mNameWidget, *mRaceWidget, *mClassWidget, *mBirthSignWidget;
        MyGUI::ScrollView* mSkillView;

        Widgets::MWDynamicStatPtr mHealth, mMagicka, mFatigue;

        std::map<ESM::RefId, Widgets::MWAttributePtr> mAttributeWidgets;
        std::vector<Widgets::MWAttribute*> mAttributeList;
        std::vector<MyGUI::Widget*> mAttributeHighlights;

        std::vector<ESM::RefId> mMajorSkills, mMinorSkills, mMiscSkills;
        std::map<ESM::RefId, MWMechanics::SkillValue> mSkillValues;
        std::map<ESM::RefId, std::pair<MyGUI::TextBox*, MyGUI::TextBox*>> mSkillWidgetMap;
        ESM::RefId mRaceId, mBirthSignId;
        std::string mName;
        ESM::Class mClass;
        std::vector<MyGUI::Widget*> mSkillWidgets; //< Skills and other information

        bool mUpdateSkillArea;
        size_t mControllerFocus = 0;
        std::vector<ControllerItem> mControllerItems;
        std::vector<size_t> mControllerLeftItems;
        std::vector<size_t> mControllerRightItems;
        size_t mLastLeftFocus = 0;
        size_t mLastRightFocus = 0;
        std::unordered_map<MyGUI::TextBox*, std::string> mControllerBaseStates;
        MyGUI::Widget* mNameHighlight = nullptr;
        MyGUI::Widget* mRaceHighlight = nullptr;
        MyGUI::Widget* mClassHighlight = nullptr;
        MyGUI::Widget* mBirthSignHighlight = nullptr;
        bool mLeftTriggerHeld = false;
        bool mRightTriggerHeld = false;
    };
}
#endif
