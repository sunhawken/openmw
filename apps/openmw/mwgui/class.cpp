#include "class.hpp"

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_UString.h>

#include <limits>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/esmstore.hpp"

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include "tooltips.hpp"

namespace
{
    constexpr size_t kClassNameLimit = 48;

    bool sortClasses(const std::pair<ESM::RefId, std::string>& left, const std::pair<ESM::RefId, std::string>& right)
    {
        return left.second.compare(right.second) < 0;
    }

}

namespace MWGui
{

    /* GenerateClassResultDialog */

    GenerateClassResultDialog::GenerateClassResultDialog()
        : WindowModal("openmw_chargen_generate_class_result.layout")
    {
        setText("ReflectT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sMessageQuestionAnswer1", {}));

        getWidget(mClassImage, "ClassImage");
        getWidget(mClassName, "ClassName");

        getWidget(mBackButton, "BackButton");
        mBackButton->setCaptionWithReplacing("#{sMessageQuestionAnswer3}");
        mBackButton->eventMouseButtonClick += MyGUI::newDelegate(this, &GenerateClassResultDialog::onBackClicked);

        getWidget(mOkButton, "OKButton");
        mOkButton->setCaptionWithReplacing("#{sMessageQuestionAnswer2}");
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &GenerateClassResultDialog::onOkClicked);

        if (Settings::gui().mControllerMenus)
        {
            mOkButton->setStateSelected(true);
            mDisableGamepadCursor = true;
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Back}";

            if (useControllerSelectionHighlight())
            {
                MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
                if (!highlightParent)
                    highlightParent = mMainWidget;
                mControllerHighlight = highlightParent->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
                mControllerHighlight->setNeedMouseFocus(false);
                mControllerHighlight->setDepth(1);
                mControllerHighlight->setVisible(false);
                mControllerHighlight->setUserString("Hidden", "true");
            }
        }

        center();
        updateControllerHighlight();
    }

    void GenerateClassResultDialog::setClassId(const ESM::RefId& classId)
    {
        mCurrentClassId = classId;

        setClassImage(mClassImage, mCurrentClassId);

        mClassName->setCaption(
            MWBase::Environment::get().getESMStore()->get<ESM::Class>().find(mCurrentClassId)->mName);

        center();
        updateControllerHighlight();
    }

    bool GenerateClassResultDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mOkButtonFocus)
                onOkClicked(mOkButton);
            else
                onBackClicked(mBackButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onBackClicked(mBackButton);
        }
        else if ((arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT && mOkButtonFocus)
            || (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !mOkButtonFocus))
        {
            mOkButtonFocus = !mOkButtonFocus;
            mOkButton->setStateSelected(mOkButtonFocus);
            mBackButton->setStateSelected(!mOkButtonFocus);
            updateControllerHighlight();
        }

        return true;
    }

    void GenerateClassResultDialog::updateControllerHighlight()
    {
        if (!mControllerHighlight || !useControllerSelectionHighlight())
        {
            if (mControllerHighlight)
                mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focusWidget = mOkButtonFocus ? static_cast<MyGUI::Widget*>(mOkButton) : mBackButton;
        if (!focusWidget)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord parentRect
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusRect = focusWidget->getAbsoluteCoord();
        mControllerHighlight->setCoord(MyGUI::IntCoord(
            focusRect.left - parentRect.left, focusRect.top - parentRect.top, focusRect.width, focusRect.height));
        mControllerHighlight->setVisible(true);
    }

    // widget controls

    void GenerateClassResultDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        eventDone(this);
    }

    void GenerateClassResultDialog::onBackClicked(MyGUI::Widget* /*sender*/)
    {
        eventBack();
    }

    /* PickClassDialog */

    PickClassDialog::PickClassDialog()
        : WindowModal("openmw_chargen_class.layout")
    {
        // Centre dialog
        center();

        getWidget(mSpecializationName, "SpecializationName");
        mSpecializationName->setUserString("ForceCollapsedTooltip", "true");

        getWidget(mFavoriteAttribute[0], "FavoriteAttribute0");
        getWidget(mFavoriteAttribute[1], "FavoriteAttribute1");

        for (int i = 0; i < 5; i++)
        {
            char theIndex = '0' + static_cast<char>(i);
            getWidget(mMajorSkill[i], std::string("MajorSkill").append(1, theIndex));
            getWidget(mMinorSkill[i], std::string("MinorSkill").append(1, theIndex));
        }

        getWidget(mClassList, "ClassList");
        mClassList->setScrollVisible(true);
        mClassList->eventListSelectAccept += MyGUI::newDelegate(this, &PickClassDialog::onAccept);
        mClassList->eventListChangePosition += MyGUI::newDelegate(this, &PickClassDialog::onSelectClass);
        mClassList->eventListChangeScroll += MyGUI::newDelegate(this, &PickClassDialog::onListScroll);

        if (MyGUI::Widget* client = mClassList->getClientWidget())
        {
            mControllerHighlight = client->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setVisible(false);
        }

        getWidget(mClassImage, "ClassImage");

        getWidget(mBackButton, "BackButton");
        mBackButton->eventMouseButtonClick += MyGUI::newDelegate(this, &PickClassDialog::onBackClicked);

        getWidget(mOkButton, "OKButton");
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &PickClassDialog::onOkClicked);

        if (Settings::gui().mControllerMenus)
        {
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mY = "#{Interface:Info}";
        }

        updateClasses();
        updateStats();
    }

    void PickClassDialog::setNextButtonShow(bool shown)
    {
        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");

        if (shown)
        {
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sNext", {})));
            mControllerButtons.mX = "#{Interface:Next}";
            mControllerButtons.mXAfterB = true;
        }
        else if (Settings::gui().mControllerMenus)
        {
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sDone", {})));
            mControllerButtons.mX = "#{Interface:Done}";
            mControllerButtons.mXAfterB = true;
        }
        else
            okButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
    }

    void PickClassDialog::onOpen()
    {
        WindowModal::onOpen();
        updateClasses();
        updateStats();
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mClassList);
        const ESM::RefId acrobatId = ESM::RefId::stringRefId("Acrobat");
        setClassId(acrobatId);
        if (mClassList->getIndexSelected() == MyGUI::ITEM_NONE)
        {
            // Fallback to the player's current class if Acrobat is unavailable.
            MWWorld::Ptr player = MWMechanics::getPlayer();
            const ESM::RefId& classId = player.get<ESM::NPC>()->mBase->mClass;
            if (!classId.empty())
                setClassId(classId);
        }

        updateControllerListHighlight(mClassList, mControllerHighlight);
    }

    void PickClassDialog::setClassId(const ESM::RefId& classId)
    {
        mCurrentClassId = classId;
        mClassList->setIndexSelected(MyGUI::ITEM_NONE);
        size_t count = mClassList->getItemCount();
        for (size_t i = 0; i < count; ++i)
        {
            if (*mClassList->getItemDataAt<ESM::RefId>(i) == classId)
            {
                mClassList->setIndexSelected(i);
                break;
            }
        }

        updateStats();
        updateControllerListHighlight(mClassList, mControllerHighlight);
    }

    MyGUI::Widget* PickClassDialog::getControllerFocusTooltipWidget() const
    {
        return mClassList;
    }

    // widget controls

    void PickClassDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        if (mClassList->getIndexSelected() == MyGUI::ITEM_NONE)
            return;
        eventDone(this);
    }

    void PickClassDialog::onBackClicked(MyGUI::Widget* /*sender*/)
    {
        eventBack();
    }

    void PickClassDialog::onAccept(MyGUI::ListBox* sender, size_t index)
    {
        onSelectClass(sender, index);
        if (mClassList->getIndexSelected() == MyGUI::ITEM_NONE)
            return;
        eventDone(this);
    }

    void PickClassDialog::onSelectClass(MyGUI::ListBox* sender, size_t index)
    {
        if (index == MyGUI::ITEM_NONE)
        {
            updateControllerListHighlight(sender, mControllerHighlight);
            return;
        }

        const ESM::RefId& classId = *mClassList->getItemDataAt<ESM::RefId>(index);
        if (mCurrentClassId == classId)
        {
            updateControllerListHighlight(sender, mControllerHighlight);
            return;
        }

        mCurrentClassId = classId;
        updateStats();
        updateControllerListHighlight(mClassList, mControllerHighlight);
    }

    void PickClassDialog::onListScroll(MyGUI::ListBox* sender, size_t /*position*/)
    {
        updateControllerListHighlight(sender, mControllerHighlight);
    }

    // update widget content

    void PickClassDialog::updateClasses()
    {
        mClassList->removeAllItems();

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

        std::vector<std::pair<ESM::RefId, std::string>> items; // class id, class name
        for (const ESM::Class& classInfo : store.get<ESM::Class>())
        {
            bool playable = (classInfo.mData.mIsPlayable != 0);
            if (!playable) // Only display playable classes
                continue;

            if (store.get<ESM::Class>().isDynamic(classInfo.mId))
                continue; // custom-made class not relevant for this dialog

            items.emplace_back(classInfo.mId, classInfo.mName);
        }
        std::sort(items.begin(), items.end(), sortClasses);

        int index = 0;
        for (auto& itemPair : items)
        {
            const ESM::RefId& id = itemPair.first;
            mClassList->addItem(itemPair.second, id);
            if (mCurrentClassId.empty())
            {
                mCurrentClassId = id;
                mClassList->setIndexSelected(index);
            }
            else if (id == mCurrentClassId)
            {
                mClassList->setIndexSelected(index);
            }
            ++index;
        }

        updateControllerListHighlight(mClassList, mControllerHighlight);
    }

    void PickClassDialog::updateStats()
    {
        if (mCurrentClassId.empty())
            return;
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Class* currentClass = store.get<ESM::Class>().search(mCurrentClassId);
        if (!currentClass)
            return;

        ToolTips::createClassToolTip(mClassList, *currentClass);
        mClassList->setUserString("CollapsedLabel", "#{sClass}");
        mClassList->setUserString("CollapsedValue", currentClass->mName);

        ESM::Class::Specialization specialization
            = static_cast<ESM::Class::Specialization>(currentClass->mData.mSpecialization);

        std::string_view specName = MWBase::Environment::get().getWindowManager()->getGameSettingString(
            ESM::Class::sGmstSpecializationIds[specialization], ESM::Class::sGmstSpecializationIds[specialization]);
        mSpecializationName->setCaption(MyGUI::UString(specName));
        ToolTips::createSpecializationToolTip(mSpecializationName, specName, specialization);
        mSpecializationName->setUserString("CollapsedValue", std::string(specName));

        mFavoriteAttribute[0]->setAttributeId(ESM::Attribute::indexToRefId(currentClass->mData.mAttribute[0]));
        mFavoriteAttribute[1]->setAttributeId(ESM::Attribute::indexToRefId(currentClass->mData.mAttribute[1]));
        ToolTips::createAttributeToolTip(mFavoriteAttribute[0], mFavoriteAttribute[0]->getAttributeId());
        ToolTips::createAttributeToolTip(mFavoriteAttribute[1], mFavoriteAttribute[1]->getAttributeId());
        mFavoriteAttribute[0]->setUserString("ToolTipDynamic", "Stats");
        mFavoriteAttribute[1]->setUserString("ToolTipDynamic", "Stats");

        for (size_t i = 0; i < currentClass->mData.mSkills.size(); ++i)
        {
            ESM::RefId minor = ESM::Skill::indexToRefId(currentClass->mData.mSkills[i][0]);
            ESM::RefId major = ESM::Skill::indexToRefId(currentClass->mData.mSkills[i][1]);
            mMinorSkill[i]->setSkillId(minor);
            mMajorSkill[i]->setSkillId(major);
            ToolTips::createSkillToolTip(mMinorSkill[i], minor);
            ToolTips::createSkillToolTip(mMajorSkill[i], major);
            mMinorSkill[i]->setUserString("ToolTipDynamic", "Stats");
            mMajorSkill[i]->setUserString("ToolTipDynamic", "Stats");
        }

        setClassImage(mClassImage, mCurrentClassId);
    }

    bool PickClassDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onBackClicked(mBackButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onOkClicked(mOkButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            winMgr->setKeyFocusWidget(mClassList);
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowUp, 0, false);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            winMgr->setKeyFocusWidget(mClassList);
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowDown, 0, false);
        }

        return true;
    }

    /* InfoBoxDialog */

    void InfoBoxDialog::fitToText(MyGUI::TextBox* widget)
    {
        MyGUI::IntCoord inner = widget->getTextRegion();
        MyGUI::IntCoord outer = widget->getCoord();
        MyGUI::IntSize size = widget->getTextSize();
        size.width += outer.width - inner.width;
        size.height += outer.height - inner.height;
        widget->setSize(size);
    }

    void InfoBoxDialog::layoutVertically(MyGUI::Widget* widget, int margin)
    {
        size_t count = widget->getChildCount();
        int pos = 0;
        pos += margin;
        int width = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            MyGUI::Widget* child = widget->getChildAt(i);
            if (!child->getVisible())
                continue;
            if (child->getUserString("Hidden") == "true")
                continue;
            if (child == mControllerHighlight)
                continue;

            child->setPosition(child->getLeft(), pos);
            width = std::max(width, child->getWidth());
            pos += child->getHeight() + margin;
        }
        width += margin * 2;
        widget->setSize(width, pos);
    }

    InfoBoxDialog::InfoBoxDialog()
        : WindowModal("openmw_infobox.layout")
    {
        getWidget(mTextBox, "TextBox");
        getWidget(mText, "Text");
        mText->getSubWidgetText()->setWordWrap(true);
        getWidget(mButtonBar, "ButtonBar");

        center();

        mDisableGamepadCursor = Settings::gui().mControllerMenus;
        mControllerButtons.mA = "#{Interface:Select}";

        if (useControllerSelectionHighlight())
        {
            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setDepth(1);
            mControllerHighlight->setVisible(false);
            mControllerHighlight->setUserString("Hidden", "true");
        }
    }

    void InfoBoxDialog::setText(const std::string& str)
    {
        mText->setCaption(str);
        mTextBox->setVisible(!str.empty());
        fitToText(mText);
    }

    std::string InfoBoxDialog::getText() const
    {
        return mText->getCaption();
    }

    void InfoBoxDialog::setButtons(ButtonList& buttons)
    {
        for (MyGUI::Button* button : this->mButtons)
        {
            MyGUI::Gui::getInstance().destroyWidget(button);
        }
        this->mButtons.clear();

        // TODO: The buttons should be generated from a template in the layout file, ie. cloning an existing widget
        MyGUI::Button* button;
        MyGUI::IntCoord coord = MyGUI::IntCoord(0, 0, mButtonBar->getWidth(), 10);
        for (const std::string& text : buttons)
        {
            button = mButtonBar->createWidget<MyGUI::Button>(
                "MW_Button", coord, MyGUI::Align::Top | MyGUI::Align::HCenter, {});
            button->getSubWidgetText()->setWordWrap(true);
            button->setCaption(text);
            fitToText(button);
            button->eventMouseButtonClick += MyGUI::newDelegate(this, &InfoBoxDialog::onButtonClicked);
            coord.top += button->getHeight();

            if (Settings::gui().mControllerMenus && buttons.size() > 1 && this->mButtons.empty())
            {
                // First button is selected by default
                button->setStateSelected(true);
            }

            this->mButtons.push_back(button);
        }
        updateControllerHighlight();
    }

    void InfoBoxDialog::onOpen()
    {
        WindowModal::onOpen();
        // Fix layout
        layoutVertically(mTextBox, 4);
        layoutVertically(mButtonBar, 6);
        layoutVertically(mMainWidget, 4 + 6);

        center();
        updateControllerHighlight();
    }

    void InfoBoxDialog::onButtonClicked(MyGUI::Widget* sender)
    {
        int i = 0;
        for (MyGUI::Button* button : mButtons)
        {
            if (button == sender)
            {
                eventButtonSelected(i);
                return;
            }
            ++i;
        }
    }

    bool InfoBoxDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mControllerFocus < mButtons.size())
                onButtonClicked(mButtons[mControllerFocus]);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            if (mButtons.size() == 1)
                onButtonClicked(mButtons[0]);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (mButtons.size() <= 1)
                return true;
            if (mButtons.size() == 2 && mControllerFocus == 0)
                return true;

            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus, mButtons.size(), -1);
            setControllerFocus(mButtons, mControllerFocus, true);
            updateControllerHighlight();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (mButtons.size() <= 1)
                return true;
            if (mButtons.size() == 2 && mControllerFocus == 1)
                return true;

            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus, mButtons.size(), 1);
            setControllerFocus(mButtons, mControllerFocus, true);
            updateControllerHighlight();
        }

        return true;
    }

    void InfoBoxDialog::updateControllerHighlight()
    {
        if (!mControllerHighlight || !useControllerSelectionHighlight() || mButtons.empty())
        {
            if (mControllerHighlight)
                mControllerHighlight->setVisible(false);
            return;
        }

        const size_t clamped = std::min(mControllerFocus, mButtons.size() - 1);
        MyGUI::Button* focusButton = mButtons[clamped];
        if (!focusButton)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord parentRect
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusRect = focusButton->getAbsoluteCoord();
        mControllerHighlight->setCoord(MyGUI::IntCoord(
            focusRect.left - parentRect.left, focusRect.top - parentRect.top, focusRect.width, focusRect.height));
        mControllerHighlight->setVisible(true);
    }

    /* ClassChoiceDialog */

    ClassChoiceDialog::ClassChoiceDialog()
        : InfoBoxDialog()
    {
        setText({});
        ButtonList buttons;
        buttons.emplace_back(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sClassChoiceMenu1", {}));
        buttons.emplace_back(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sClassChoiceMenu2", {}));
        buttons.emplace_back(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sClassChoiceMenu3", {}));
        buttons.emplace_back(MWBase::Environment::get().getWindowManager()->getGameSettingString("sBack", {}));
        setButtons(buttons);
    }

    /* CreateClassDialog */

    CreateClassDialog::CreateClassDialog()
        : WindowModal("openmw_chargen_create_class.layout")
        , mAffectedAttribute(nullptr)
        , mAffectedSkill(nullptr)
    {
        // Centre dialog
        center();

        setText("SpecializationT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sChooseClassMenu1", "Specialization"));
        getWidget(mSpecializationName, "SpecializationName");
        mSpecializationName->setUserString("ForceCollapsedTooltip", "true");
        mSpecializationName->eventMouseButtonClick
            += MyGUI::newDelegate(this, &CreateClassDialog::onSpecializationClicked);

        setText("FavoriteAttributesT",
            MWBase::Environment::get().getWindowManager()->getGameSettingString(
                "sChooseClassMenu2", "Favorite Attributes:"));
        getWidget(mFavoriteAttribute0, "FavoriteAttribute0");
        getWidget(mFavoriteAttribute1, "FavoriteAttribute1");
        mFavoriteAttribute0->eventClicked += MyGUI::newDelegate(this, &CreateClassDialog::onAttributeClicked);
        mFavoriteAttribute1->eventClicked += MyGUI::newDelegate(this, &CreateClassDialog::onAttributeClicked);

        setText(
            "MajorSkillT", MWBase::Environment::get().getWindowManager()->getGameSettingString("sSkillClassMajor", {}));
        setText(
            "MinorSkillT", MWBase::Environment::get().getWindowManager()->getGameSettingString("sSkillClassMinor", {}));
        for (char i = 0; i < 5; i++)
        {
            char theIndex = '0' + i;
            getWidget(mMajorSkill[i], std::string("MajorSkill").append(1, theIndex));
            getWidget(mMinorSkill[i], std::string("MinorSkill").append(1, theIndex));
            mSkills.push_back(mMajorSkill[i]);
            mSkills.push_back(mMinorSkill[i]);
        }

        for (Widgets::MWSkillPtr& skill : mSkills)
        {
            skill->eventClicked += MyGUI::newDelegate(this, &CreateClassDialog::onSkillClicked);
        }

        setText("LabelT", MWBase::Environment::get().getWindowManager()->getGameSettingString("sName", {}));
        getWidget(mEditName, "EditName");
        mEditName->setUserString("ForceEmptyTooltipBar", "true");
        mEditName->setMaxTextLength(kClassNameLimit);
        mEditName->eventEditTextChange += MyGUI::newDelegate(this, &CreateClassDialog::onNameEdited);

        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mEditName);

        getWidget(mDescriptionButton, "DescriptionButton");
        mDescriptionButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CreateClassDialog::onDescriptionClicked);

        getWidget(mBackButton, "BackButton");
        mBackButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CreateClassDialog::onBackClicked);

        getWidget(mOkButton, "OKButton");
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CreateClassDialog::onOkClicked);

        if (Settings::gui().mControllerMenus)
        {
            mOkButton->setStateSelected(true);
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mY = "#{Interface:Info}";
            mControllerButtons.mR1 = "#{Interface:ClassDescription}";
            mControllerButtons.mLStick.clear();

            mDescriptionButton->setVisible(false);
            mDescriptionButton->setEnabled(false);
            mBackButton->setVisible(false);
            mBackButton->setEnabled(false);
            mOkButton->setVisible(false);
            mOkButton->setEnabled(false);
        }

        if (Settings::gui().mControllerMenus)
        {
            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);
        }

        // Set default skills, attributes

        mFavoriteAttribute0->setAttributeId(ESM::Attribute::Strength);
        mFavoriteAttribute1->setAttributeId(ESM::Attribute::Agility);

        mMajorSkill[0]->setSkillId(ESM::Skill::Block);
        mMajorSkill[1]->setSkillId(ESM::Skill::Armorer);
        mMajorSkill[2]->setSkillId(ESM::Skill::MediumArmor);
        mMajorSkill[3]->setSkillId(ESM::Skill::HeavyArmor);
        mMajorSkill[4]->setSkillId(ESM::Skill::BluntWeapon);

        mMinorSkill[0]->setSkillId(ESM::Skill::LongBlade);
        mMinorSkill[1]->setSkillId(ESM::Skill::Axe);
        mMinorSkill[2]->setSkillId(ESM::Skill::Spear);
        mMinorSkill[3]->setSkillId(ESM::Skill::Athletics);
        mMinorSkill[4]->setSkillId(ESM::Skill::Enchant);

        setSpecialization(0);
        update();
        buildControllerItems();
        if (Settings::gui().mControllerMenus && !mControllerItems.empty())
        {
            size_t specIndex = 0;
            for (size_t i = 0; i < mControllerItems.size(); ++i)
            {
                if (mControllerItems[i].mWidget == mSpecializationName)
                {
                    specIndex = i;
                    break;
                }
            }
            mControllerFocus = std::min(specIndex, mControllerItems.size() - 1);
            setControllerItemSelected(mControllerItems[mControllerFocus].mWidget, true);
            updateControllerFocusHighlight();
            MWBase::Environment::get().getWindowManager()->restoreControllerTooltips();
        }
    }

    CreateClassDialog::~CreateClassDialog() = default;

    void CreateClassDialog::update()
    {
        for (int i = 0; i < 5; ++i)
        {
            ToolTips::createSkillToolTip(mMajorSkill[i], mMajorSkill[i]->getSkillId());
            ToolTips::createSkillToolTip(mMinorSkill[i], mMinorSkill[i]->getSkillId());
            if (mMajorSkill[i])
                mMajorSkill[i]->setUserString("ToolTipDynamic", "Stats");
            if (mMinorSkill[i])
                mMinorSkill[i]->setUserString("ToolTipDynamic", "Stats");
            if (mMajorSkill[i] != nullptr)
            {
                const auto skillId = mMajorSkill[i]->getSkillId();
                const auto* skill = MWBase::Environment::get().getESMStore()->get<ESM::Skill>().search(skillId);
                if (skill)
                    mMajorSkill[i]->setUserString("CollapsedLabel", skill->mName);
                mMajorSkill[i]->setUserString("CollapsedValue", {});
            }
            if (mMinorSkill[i] != nullptr)
            {
                const auto skillId = mMinorSkill[i]->getSkillId();
                const auto* skill = MWBase::Environment::get().getESMStore()->get<ESM::Skill>().search(skillId);
                if (skill)
                    mMinorSkill[i]->setUserString("CollapsedLabel", skill->mName);
                mMinorSkill[i]->setUserString("CollapsedValue", {});
            }
        }

        ToolTips::createAttributeToolTip(mFavoriteAttribute0, mFavoriteAttribute0->getAttributeId());
        ToolTips::createAttributeToolTip(mFavoriteAttribute1, mFavoriteAttribute1->getAttributeId());
        if (mFavoriteAttribute0)
            mFavoriteAttribute0->setUserString("ToolTipDynamic", "Stats");
        if (mFavoriteAttribute1)
            mFavoriteAttribute1->setUserString("ToolTipDynamic", "Stats");
        if (mFavoriteAttribute0 != nullptr)
        {
            const auto attrId = mFavoriteAttribute0->getAttributeId();
            const auto* attr = MWBase::Environment::get().getESMStore()->get<ESM::Attribute>().search(attrId);
            if (attr)
                mFavoriteAttribute0->setUserString("CollapsedLabel", attr->mName);
            mFavoriteAttribute0->setUserString("CollapsedValue", {});
        }
        if (mFavoriteAttribute1 != nullptr)
        {
            const auto attrId = mFavoriteAttribute1->getAttributeId();
            const auto* attr = MWBase::Environment::get().getESMStore()->get<ESM::Attribute>().search(attrId);
            if (attr)
                mFavoriteAttribute1->setUserString("CollapsedLabel", attr->mName);
            mFavoriteAttribute1->setUserString("CollapsedValue", {});
        }
    }

    void CreateClassDialog::onNameEdited(MyGUI::EditBox* sender)
    {
        const size_t length = sender->getCaption().size();
        if (mSuppressNameLimitMessage)
        {
            mNameHitLimit = length >= kClassNameLimit;
            return;
        }

        if (length < kClassNameLimit)
        {
            mNameHitLimit = false;
            return;
        }

        if (!mNameHitLimit)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("You've hit the character limit");
            mNameHitLimit = true;
        }
    }

    std::string CreateClassDialog::getName() const
    {
        return mEditName->getCaption();
    }

    std::string CreateClassDialog::getDescription() const
    {
        return mDescription;
    }

    ESM::Class::Specialization CreateClassDialog::getSpecializationId() const
    {
        return mSpecializationId;
    }

    std::vector<ESM::RefId> CreateClassDialog::getFavoriteAttributes() const
    {
        std::vector<ESM::RefId> v;
        v.push_back(mFavoriteAttribute0->getAttributeId());
        v.push_back(mFavoriteAttribute1->getAttributeId());
        return v;
    }

    std::vector<ESM::RefId> CreateClassDialog::getMajorSkills() const
    {
        std::vector<ESM::RefId> v;
        v.reserve(mMajorSkill.size());
        for (const auto& widget : mMajorSkill)
        {
            v.push_back(widget->getSkillId());
        }
        return v;
    }

    std::vector<ESM::RefId> CreateClassDialog::getMinorSkills() const
    {
        std::vector<ESM::RefId> v;
        v.reserve(mMinorSkill.size());
        for (const auto& widget : mMinorSkill)
        {
            v.push_back(widget->getSkillId());
        }
        return v;
    }

    void CreateClassDialog::setNextButtonShow(bool shown)
    {
        if (!mOkButton)
            return;

        if (shown)
        {
            mOkButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sNext", {})));
            mControllerButtons.mX = "#{Interface:Next}";
            mControllerButtons.mXAfterB = true;
        }
        else if (Settings::gui().mControllerMenus)
        {
            mOkButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sDone", {})));
            mControllerButtons.mX = "#{Interface:Done}";
            mControllerButtons.mXAfterB = true;
        }
        else
            mOkButton->setCaption(
                MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sOK", {})));
    }

    bool CreateClassDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (mControllerItems.empty())
            return true;

        const ControllerItem& current = mControllerItems[mControllerFocus];
        MyGUI::Widget* focusWidget = current.mWidget;

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (focusWidget == mEditName)
            {
                openVirtualKeyboard(mEditName);
                return true;
            }
            if (focusWidget == mSpecializationName)
            {
                onSpecializationClicked(mSpecializationName);
                return true;
            }
            if (focusWidget == mFavoriteAttribute0 || focusWidget == mFavoriteAttribute1)
            {
                onAttributeClicked(focusWidget->castType<Widgets::MWAttribute>(false));
                return true;
            }
            for (const Widgets::MWSkillPtr& skill : mSkills)
            {
                if (focusWidget == skill)
                {
                    onSkillClicked(skill);
                    return true;
                }
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onBackClicked(mBackButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_Y)
        {
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            onDescriptionClicked(mDescriptionButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onOkClicked(mOkButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            moveControllerFocusHorizontal(-1);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            moveControllerFocusHorizontal(1);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            moveControllerFocusVertical(-1);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            moveControllerFocusVertical(1);
        }
        return true;
    }

    MyGUI::Widget* CreateClassDialog::getControllerFocusTooltipWidget() const
    {
        if (!Settings::gui().mControllerMenus || mControllerItems.empty())
            return nullptr;

        MyGUI::Widget* widget = mControllerItems[mControllerFocus].mWidget;
        if (widget && widget->isUserString("ToolTipType"))
            return widget;
        return nullptr;
    }

    void CreateClassDialog::buildControllerItems()
    {
        mControllerItems.clear();
        auto addItem = [&](MyGUI::Widget* widget, int column, int row) {
            if (widget)
                mControllerItems.push_back({ widget, column, row });
        };

        addItem(mEditName, 0, -1);
        if (!mControllerItems.empty())
            mControllerNameIndex = mControllerItems.size() - 1;
        addItem(mSpecializationName, 0, 0);
        addItem(mFavoriteAttribute0, 0, 1);
        addItem(mFavoriteAttribute1, 0, 2);

        for (int i = 0; i < 5; ++i)
            addItem(mMajorSkill[i], 1, i);
        for (int i = 0; i < 5; ++i)
            addItem(mMinorSkill[i], 2, i);
    }

    void CreateClassDialog::setControllerFocusIndex(size_t newIndex)
    {
        if (mControllerItems.empty())
            return;
        newIndex = std::min(newIndex, mControllerItems.size() - 1);
        if (newIndex == mControllerFocus)
            return;

        setControllerItemSelected(mControllerItems[mControllerFocus].mWidget, false);
        mControllerFocus = newIndex;
        setControllerItemSelected(mControllerItems[mControllerFocus].mWidget, true);
        updateControllerFocusHighlight();
        MWBase::Environment::get().getWindowManager()->restoreControllerTooltips();

        if (auto* edit = mControllerItems[mControllerFocus].mWidget->castType<MyGUI::EditBox>(false))
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(edit);
    }

    void CreateClassDialog::moveControllerFocusVertical(int delta)
    {
        if (mControllerItems.empty())
            return;

        const ControllerItem& current = mControllerItems[mControllerFocus];
        if (current.mWidget == mEditName)
        {
            if (delta > 0)
            {
                const int targetColumn = std::clamp(mControllerNameReturnColumn, 0, 2);
                size_t bestIndex = mControllerFocus;
                bool found = false;
                int bestRow = std::numeric_limits<int>::max();
                for (size_t i = 0; i < mControllerItems.size(); ++i)
                {
                    const ControllerItem& item = mControllerItems[i];
                    if (item.mColumn != targetColumn || item.mRow < 0)
                        continue;
                    if (item.mRow < bestRow)
                    {
                        bestRow = item.mRow;
                        bestIndex = i;
                        found = true;
                    }
                }
                if (found)
                    setControllerFocusIndex(bestIndex);
            }
            return;
        }

        size_t bestIndex = mControllerFocus;
        bool found = false;
        int bestRow = current.mRow;

        for (size_t i = 0; i < mControllerItems.size(); ++i)
        {
            const ControllerItem& item = mControllerItems[i];
            if (item.mColumn != current.mColumn)
                continue;
            if (delta < 0 && item.mRow < current.mRow)
            {
                if (!found || item.mRow > bestRow)
                {
                    bestRow = item.mRow;
                    bestIndex = i;
                    found = true;
                }
            }
            else if (delta > 0 && item.mRow > current.mRow)
            {
                if (!found || item.mRow < bestRow)
                {
                    bestRow = item.mRow;
                    bestIndex = i;
                    found = true;
                }
            }
        }

        if (found)
        {
            if (mControllerItems[bestIndex].mWidget == mEditName)
                mControllerNameReturnColumn = current.mColumn;
            setControllerFocusIndex(bestIndex);
        }
        else if (delta < 0 && mEditName)
        {
            mControllerNameReturnColumn = current.mColumn;
            setControllerFocusIndex(mControllerNameIndex);
        }
    }

    void CreateClassDialog::moveControllerFocusHorizontal(int delta)
    {
        if (mControllerItems.empty())
            return;

        const ControllerItem& current = mControllerItems[mControllerFocus];
        if (delta < 0)
        {
            if (auto* skill = current.mWidget->castType<Widgets::MWSkill>(false))
            {
                if (skill->getSkillId() == ESM::Skill::Armorer && mSpecializationName)
                {
                    for (size_t i = 0; i < mControllerItems.size(); ++i)
                    {
                        if (mControllerItems[i].mWidget == mSpecializationName)
                        {
                            setControllerFocusIndex(i);
                            return;
                        }
                    }
                }
                if (skill->getSkillId() == ESM::Skill::MediumArmor && mFavoriteAttribute0)
                {
                    for (size_t i = 0; i < mControllerItems.size(); ++i)
                    {
                        if (mControllerItems[i].mWidget == mFavoriteAttribute0)
                        {
                            setControllerFocusIndex(i);
                            return;
                        }
                    }
                }
            }
        }
        if (delta > 0)
        {
            bool hasTarget = false;
            ESM::RefId targetSkill;
            if (current.mWidget == mFavoriteAttribute0)
            {
                hasTarget = true;
                targetSkill = ESM::Skill::MediumArmor;
            }
            else if (current.mWidget == mFavoriteAttribute1)
            {
                hasTarget = true;
                targetSkill = ESM::Skill::HeavyArmor;
            }

            if (hasTarget)
            {
                for (size_t i = 0; i < mControllerItems.size(); ++i)
                {
                    if (auto* skill = mControllerItems[i].mWidget->castType<Widgets::MWSkill>(false))
                    {
                        if (skill->getSkillId() == targetSkill)
                        {
                            setControllerFocusIndex(i);
                            return;
                        }
                    }
                }
            }
        }
        const int targetColumn = current.mColumn + delta;

        size_t bestIndex = mControllerFocus;
        bool found = false;
        int bestDistance = 0;
        int bestRow = 0;

        for (size_t i = 0; i < mControllerItems.size(); ++i)
        {
            const ControllerItem& item = mControllerItems[i];
            if (item.mColumn != targetColumn)
                continue;
            const int distance = std::abs(item.mRow - current.mRow);
            if (!found || distance < bestDistance || (distance == bestDistance && item.mRow < bestRow))
            {
                bestDistance = distance;
                bestRow = item.mRow;
                bestIndex = i;
                found = true;
            }
        }

        if (found)
            setControllerFocusIndex(bestIndex);
    }

    void CreateClassDialog::updateControllerFocusHighlight()
    {
        if (!mControllerFocusHighlight || !useControllerSelectionHighlight() || mControllerItems.empty())
        {
            if (mControllerFocusHighlight)
                mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focusWidget = mControllerItems[mControllerFocus].mWidget;
        if (!focusWidget)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        const MyGUI::IntCoord focusCoord = focusWidget->getAbsoluteCoord();
        MyGUI::Widget* highlightParent = mControllerFocusHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        mControllerFocusHighlight->setCoord(
            focusCoord.left - baseCoord.left, focusCoord.top - baseCoord.top, focusCoord.width, focusCoord.height);
        mControllerFocusHighlight->setVisible(true);
    }

    void CreateClassDialog::setControllerItemSelected(MyGUI::Widget* widget, bool selected)
    {
        if (!widget)
            return;
        if (auto* skill = widget->castType<Widgets::MWSkill>(false))
            skill->setStateSelected(selected);
        else if (auto* attr = widget->castType<Widgets::MWAttribute>(false))
            attr->setStateSelected(selected);
        else if (auto* button = widget->castType<MyGUI::Button>(false))
            button->setStateSelected(selected);

        if (selected)
            updateControllerFocusHighlight();
    }

    void CreateClassDialog::openVirtualKeyboard(MyGUI::EditBox* edit)
    {
        if (!edit)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->isVirtualKeyboardVisible())
            return;

        edit->setEditStatic(false);
        winMgr->setKeyFocusWidget(edit);
        winMgr->toggleVirtualKeyboard();
    }

    // widget controls

    void CreateClassDialog::onDialogCancel()
    {
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSpecDialog));
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mAttribDialog));
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSkillDialog));
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mDescDialog));
    }

    void CreateClassDialog::onSpecializationClicked(MyGUI::Widget* /*sender*/)
    {
        mSpecDialog = std::make_unique<SelectSpecializationDialog>();
        mSpecDialog->eventCancel += MyGUI::newDelegate(this, &CreateClassDialog::onDialogCancel);
        mSpecDialog->eventItemSelected += MyGUI::newDelegate(this, &CreateClassDialog::onSpecializationSelected);
        mSpecDialog->setVisible(true);
    }

    void CreateClassDialog::onSpecializationSelected()
    {
        mSpecializationId = mSpecDialog->getSpecializationId();
        setSpecialization(mSpecializationId);

        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSpecDialog));
    }

    void CreateClassDialog::setSpecialization(int id)
    {
        mSpecializationId = ESM::Class::Specialization(id);
        const std::string specName{ MWBase::Environment::get().getWindowManager()->getGameSettingString(
            ESM::Class::sGmstSpecializationIds[mSpecializationId],
            ESM::Class::sGmstSpecializationIds[mSpecializationId]) };
        mSpecializationName->setCaption(specName);
        ToolTips::createSpecializationToolTip(mSpecializationName, specName, mSpecializationId);
        const std::string label = std::string(
            MWBase::Environment::get().getWindowManager()->getGameSettingString("sChooseClassMenu1", "Specialization"));
        mSpecializationName->setUserString("CollapsedLabel", label);
        mSpecializationName->setUserString("CollapsedValue", specName);
    }

    void CreateClassDialog::onAttributeClicked(Widgets::MWAttributePtr sender)
    {
        mAttribDialog = std::make_unique<SelectAttributeDialog>();
        mAffectedAttribute = sender;
        mAttribDialog->eventCancel += MyGUI::newDelegate(this, &CreateClassDialog::onDialogCancel);
        mAttribDialog->eventItemSelected += MyGUI::newDelegate(this, &CreateClassDialog::onAttributeSelected);
        mAttribDialog->setVisible(true);
    }

    void CreateClassDialog::onAttributeSelected()
    {
        ESM::RefId id = mAttribDialog->getAttributeId();
        if (mAffectedAttribute == mFavoriteAttribute0)
        {
            if (mFavoriteAttribute1->getAttributeId() == id)
                mFavoriteAttribute1->setAttributeId(mFavoriteAttribute0->getAttributeId());
        }
        else if (mAffectedAttribute == mFavoriteAttribute1)
        {
            if (mFavoriteAttribute0->getAttributeId() == id)
                mFavoriteAttribute0->setAttributeId(mFavoriteAttribute1->getAttributeId());
        }
        mAffectedAttribute->setAttributeId(id);
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mAttribDialog));

        update();
    }

    void CreateClassDialog::onSkillClicked(Widgets::MWSkillPtr sender)
    {
        mSkillDialog = std::make_unique<SelectSkillDialog>();
        mAffectedSkill = sender;
        mSkillDialog->eventCancel += MyGUI::newDelegate(this, &CreateClassDialog::onDialogCancel);
        mSkillDialog->eventItemSelected += MyGUI::newDelegate(this, &CreateClassDialog::onSkillSelected);
        mSkillDialog->setVisible(true);
    }

    void CreateClassDialog::onSkillSelected()
    {
        ESM::RefId id = mSkillDialog->getSkillId();

        // Avoid duplicate skills by swapping any skill field that matches the selected one
        for (Widgets::MWSkillPtr& skill : mSkills)
        {
            if (skill == mAffectedSkill)
                continue;
            if (skill->getSkillId() == id)
            {
                skill->setSkillId(mAffectedSkill->getSkillId());
                break;
            }
        }

        mAffectedSkill->setSkillId(mSkillDialog->getSkillId());
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSkillDialog));
        update();
    }

    void CreateClassDialog::onDescriptionClicked(MyGUI::Widget* /*sender*/)
    {
        mDescDialog = std::make_unique<DescriptionDialog>();
        mDescDialog->setTextInput(mDescription);
        mDescDialog->eventDone += MyGUI::newDelegate(this, &CreateClassDialog::onDescriptionEntered);
        mDescDialog->setVisible(true);
    }

    void CreateClassDialog::onDescriptionEntered(WindowBase* parWindow)
    {
        mDescription = mDescDialog->getTextInput();
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mDescDialog));
    }

    void CreateClassDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        if (getName().size() <= 0)
            return;
        eventDone(this);
    }

    void CreateClassDialog::onBackClicked(MyGUI::Widget* /*sender*/)
    {
        eventBack();
    }

    /* SelectSpecializationDialog */

    SelectSpecializationDialog::SelectSpecializationDialog()
        : WindowModal("openmw_chargen_select_specialization.layout")
    {
        // Centre dialog
        center();

        getWidget(mSpecialization0, "Specialization0");
        getWidget(mSpecialization1, "Specialization1");
        getWidget(mSpecialization2, "Specialization2");
        std::string combat{ MWBase::Environment::get().getWindowManager()->getGameSettingString(
            ESM::Class::sGmstSpecializationIds[ESM::Class::Combat], {}) };
        std::string magic{ MWBase::Environment::get().getWindowManager()->getGameSettingString(
            ESM::Class::sGmstSpecializationIds[ESM::Class::Magic], {}) };
        std::string stealth{ MWBase::Environment::get().getWindowManager()->getGameSettingString(
            ESM::Class::sGmstSpecializationIds[ESM::Class::Stealth], {}) };

        mSpecialization0->setCaption(combat);
        mSpecialization0->setUserString("ForceCollapsedTooltip", "true");
        mSpecialization0->setUserString("CollapsedLabel", combat);
        mSpecialization0->setUserString("CollapsedValue", combat);
        mSpecialization0->eventMouseButtonClick
            += MyGUI::newDelegate(this, &SelectSpecializationDialog::onSpecializationClicked);
        mSpecialization1->setCaption(magic);
        mSpecialization1->setUserString("ForceCollapsedTooltip", "true");
        mSpecialization1->setUserString("CollapsedLabel", magic);
        mSpecialization1->setUserString("CollapsedValue", magic);
        mSpecialization1->eventMouseButtonClick
            += MyGUI::newDelegate(this, &SelectSpecializationDialog::onSpecializationClicked);
        mSpecialization2->setCaption(stealth);
        mSpecialization2->setUserString("ForceCollapsedTooltip", "true");
        mSpecialization2->setUserString("CollapsedLabel", stealth);
        mSpecialization2->setUserString("CollapsedValue", stealth);
        mSpecialization2->eventMouseButtonClick
            += MyGUI::newDelegate(this, &SelectSpecializationDialog::onSpecializationClicked);
        mSpecializationId = ESM::Class::Combat;

        ToolTips::createSpecializationToolTip(mSpecialization0, combat, ESM::Class::Combat);
        ToolTips::createSpecializationToolTip(mSpecialization1, magic, ESM::Class::Magic);
        ToolTips::createSpecializationToolTip(mSpecialization2, stealth, ESM::Class::Stealth);

        MyGUI::Button* cancelButton;
        getWidget(cancelButton, "CancelButton");
        cancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SelectSpecializationDialog::onCancelClicked);

        mControllerButtons.mA = "#{Interface:Select}";
        mControllerButtons.mB = "#{Interface:Cancel}";

        if (Settings::gui().mControllerMenus)
        {
            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setDepth(1);
            mControllerHighlight->setVisible(false);
            updateControllerHighlight();
        }
    }

    SelectSpecializationDialog::~SelectSpecializationDialog() {}

    // widget controls

    void SelectSpecializationDialog::onSpecializationClicked(MyGUI::Widget* sender)
    {
        if (sender == mSpecialization0)
            mSpecializationId = ESM::Class::Combat;
        else if (sender == mSpecialization1)
            mSpecializationId = ESM::Class::Magic;
        else if (sender == mSpecialization2)
            mSpecializationId = ESM::Class::Stealth;
        else
            return;

        eventItemSelected();
    }

    void SelectSpecializationDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
    {
        exit();
    }

    void SelectSpecializationDialog::updateControllerHighlight()
    {
        if (!mControllerHighlight || !useControllerSelectionHighlight())
        {
            if (mControllerHighlight)
                mControllerHighlight->setVisible(false);
            return;
        }

        const std::array<MyGUI::TextBox*, 3> widgets{ mSpecialization0, mSpecialization1, mSpecialization2 };
        const size_t clamped = std::min(mControllerFocus, widgets.size() - 1);
        MyGUI::Widget* focusWidget = widgets[clamped];
        if (!focusWidget)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord parentRect
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusRect = focusWidget->getAbsoluteCoord();
        const int width = highlightParent ? highlightParent->getWidth() : focusRect.width;
        const int left = highlightParent ? 0 : (focusRect.left - parentRect.left);
        mControllerHighlight->setCoord(MyGUI::IntCoord(left, focusRect.top - parentRect.top, width, focusRect.height));
        mControllerHighlight->setVisible(true);
    }

    MyGUI::Widget* SelectSpecializationDialog::getControllerFocusTooltipWidget() const
    {
        if (!Settings::gui().mControllerMenus)
            return nullptr;
        const std::array<MyGUI::TextBox*, 3> widgets{ mSpecialization0, mSpecialization1, mSpecialization2 };
        const size_t clamped = std::min(mControllerFocus, widgets.size() - 1);
        MyGUI::Widget* widget = widgets[clamped];
        if (widget && widget->isUserString("ToolTipType"))
            return widget;
        return nullptr;
    }

    bool SelectSpecializationDialog::exit()
    {
        eventCancel();
        return true;
    }

    bool SelectSpecializationDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        const std::array<MyGUI::TextBox*, 3> widgets{ mSpecialization0, mSpecialization1, mSpecialization2 };

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            const size_t clamped = std::min(mControllerFocus, widgets.size() - 1);
            onSpecializationClicked(widgets[clamped]);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelClicked(nullptr);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            mControllerFocus = wrap(mControllerFocus, widgets.size(), -1);
            updateControllerHighlight();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            mControllerFocus = wrap(mControllerFocus, widgets.size(), 1);
            updateControllerHighlight();
            return true;
        }
        return false;
    }

    /* SelectAttributeDialog */

    SelectAttributeDialog::SelectAttributeDialog()
        : WindowModal("openmw_chargen_select_attribute.layout")
        , mAttributeId(ESM::Attribute::Strength)
    {
        // Centre dialog
        center();

        const auto& store = MWBase::Environment::get().getWorld()->getStore().get<ESM::Attribute>();
        MyGUI::ScrollView* attributes;
        getWidget(attributes, "Attributes");
        MyGUI::IntCoord coord{ 0, 0, attributes->getWidth(), 18 };
        for (const ESM::Attribute& attribute : store)
        {
            auto* widget
                = attributes->createWidget<Widgets::MWAttribute>("MW_StatNameButtonC", coord, MyGUI::Align::Default);
            coord.top += coord.height;
            widget->setAttributeId(attribute.mId);
            widget->eventClicked += MyGUI::newDelegate(this, &SelectAttributeDialog::onAttributeClicked);
            ToolTips::createAttributeToolTip(widget, attribute.mId);
            widget->setUserString("ToolTipDynamic", "Stats");
            widget->setUserString("CollapsedLabel", attribute.mName);
            widget->setUserString("CollapsedValue", {});
            mAttributeButtons.emplace_back(widget);
        }

        attributes->setVisibleVScroll(false);
        attributes->setCanvasSize(MyGUI::IntSize(attributes->getWidth(), std::max(attributes->getHeight(), coord.top)));
        attributes->setVisibleVScroll(true);
        attributes->setViewOffset(MyGUI::IntPoint());

        MyGUI::Button* cancelButton;
        getWidget(cancelButton, "CancelButton");
        cancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SelectAttributeDialog::onCancelClicked);

        if (Settings::gui().mControllerMenus)
        {
            if (mAttributeButtons.size() > 0)
                mAttributeButtons[0]->setStateSelected(true);

            mControllerButtons.mB = "#{Interface:Cancel}";
            mControllerButtons.mY = "#{Interface:Info}";

            if (useControllerSelectionHighlight())
            {
                MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
                if (!highlightParent)
                    highlightParent = mMainWidget;
                mControllerHighlight = highlightParent->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
                mControllerHighlight->setNeedMouseFocus(false);
                mControllerHighlight->setDepth(1);
                mControllerHighlight->setVisible(false);
                updateControllerHighlight();
            }
        }
    }

    // widget controls

    void SelectAttributeDialog::onAttributeClicked(Widgets::MWAttributePtr sender)
    {
        mAttributeId = sender->getAttributeId();
        eventItemSelected();
    }

    void SelectAttributeDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
    {
        exit();
    }

    bool SelectAttributeDialog::exit()
    {
        eventCancel();
        return true;
    }

    bool SelectAttributeDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mControllerFocus < mAttributeButtons.size())
                onAttributeClicked(mAttributeButtons[mControllerFocus]);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelClicked(nullptr);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            mAttributeButtons[mControllerFocus]->setStateSelected(false);
            mControllerFocus = wrap(mControllerFocus, mAttributeButtons.size(), -1);
            mAttributeButtons[mControllerFocus]->setStateSelected(true);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            mAttributeButtons[mControllerFocus]->setStateSelected(false);
            mControllerFocus = wrap(mControllerFocus, mAttributeButtons.size(), 1);
            mAttributeButtons[mControllerFocus]->setStateSelected(true);
        }

        updateControllerHighlight();
        return true;
    }

    MyGUI::Widget* SelectAttributeDialog::getControllerFocusTooltipWidget() const
    {
        if (!Settings::gui().mControllerMenus || mAttributeButtons.empty())
            return nullptr;
        if (mControllerFocus >= mAttributeButtons.size())
            return nullptr;
        MyGUI::Widget* widget = mAttributeButtons[mControllerFocus];
        if (widget && widget->isUserString("ToolTipType"))
            return widget;
        return nullptr;
    }

    void SelectAttributeDialog::updateControllerHighlight()
    {
        if (!mControllerHighlight || !useControllerSelectionHighlight() || mAttributeButtons.empty())
        {
            if (mControllerHighlight)
                mControllerHighlight->setVisible(false);
            return;
        }

        if (mControllerFocus >= mAttributeButtons.size())
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focusWidget = mAttributeButtons[mControllerFocus];
        if (!focusWidget)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord parentRect
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusRect = focusWidget->getAbsoluteCoord();
        const int width = highlightParent ? highlightParent->getWidth() : focusRect.width;
        const int left = highlightParent ? 0 : (focusRect.left - parentRect.left);
        mControllerHighlight->setCoord(MyGUI::IntCoord(left, focusRect.top - parentRect.top, width, focusRect.height));
        mControllerHighlight->setVisible(true);
    }

    /* SelectSkillDialog */

    SelectSkillDialog::SelectSkillDialog()
        : WindowModal("openmw_chargen_select_skill.layout")
        , mSkillId(ESM::Skill::Block)
    {
        // Centre dialog
        center();

        std::array<std::pair<MyGUI::ScrollView*, MyGUI::IntCoord>, 3> specializations;
        getWidget(specializations[ESM::Class::Combat].first, "CombatSkills");
        getWidget(specializations[ESM::Class::Magic].first, "MagicSkills");
        getWidget(specializations[ESM::Class::Stealth].first, "StealthSkills");
        for (auto& [widget, coord] : specializations)
        {
            coord.width = widget->getCoord().width;
            coord.height = 18;
            while (widget->getChildCount() > 0)
                MyGUI::Gui::getInstance().destroyWidget(widget->getChildAt(0));
        }
        for (const ESM::Skill& skill : MWBase::Environment::get().getESMStore()->get<ESM::Skill>())
        {
            auto& [widget, coord] = specializations[skill.mData.mSpecialization];
            auto* skillWidget
                = widget->createWidget<Widgets::MWSkill>("MW_StatNameButton", coord, MyGUI::Align::Default);
            coord.top += coord.height;
            skillWidget->setSkillId(skill.mId);
            skillWidget->eventClicked += MyGUI::newDelegate(this, &SelectSkillDialog::onSkillClicked);
            ToolTips::createSkillToolTip(skillWidget, skill.mId);
            skillWidget->setUserString("ToolTipDynamic", "Stats");
            skillWidget->setUserString("CollapsedLabel", skill.mName);
            skillWidget->setUserString("CollapsedValue", {});
            mSkillButtons.emplace_back(skillWidget);
            mNumSkillsPerSpecialization[skill.mData.mSpecialization]++;
        }
        for (const auto& [widget, coord] : specializations)
        {
            widget->setVisibleVScroll(false);
            widget->setCanvasSize(MyGUI::IntSize(widget->getWidth(), std::max(widget->getHeight(), coord.top)));
            widget->setVisibleVScroll(true);
            widget->setViewOffset(MyGUI::IntPoint());
        }

        MyGUI::Button* cancelButton;
        getWidget(cancelButton, "CancelButton");
        cancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SelectSkillDialog::onCancelClicked);

        if (Settings::gui().mControllerMenus)
        {
            if (mSkillButtons.size() > 0)
                mSkillButtons[0]->setStateSelected(true);

            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Cancel}";
            mControllerButtons.mY = "#{Interface:Info}";

            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setDepth(1);
            mControllerHighlight->setVisible(false);
            updateControllerHighlight();
        }
    }

    SelectSkillDialog::~SelectSkillDialog() {}

    // widget controls

    void SelectSkillDialog::onSkillClicked(Widgets::MWSkillPtr sender)
    {
        mSkillId = sender->getSkillId();
        eventItemSelected();
    }

    void SelectSkillDialog::onCancelClicked(MyGUI::Widget* /*sender*/)
    {
        exit();
    }

    bool SelectSkillDialog::exit()
    {
        eventCancel();
        return true;
    }

    bool SelectSkillDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mControllerFocus < mSkillButtons.size())
                onSkillClicked(mSkillButtons[mControllerFocus]);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelClicked(nullptr);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            mSkillButtons[mControllerFocus]->setStateSelected(false);
            mControllerFocus = wrap(mControllerFocus, mSkillButtons.size(), -1);
            mSkillButtons[mControllerFocus]->setStateSelected(true);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            mSkillButtons[mControllerFocus]->setStateSelected(false);
            mControllerFocus = wrap(mControllerFocus, mSkillButtons.size(), 1);
            mSkillButtons[mControllerFocus]->setStateSelected(true);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            mSkillButtons[mControllerFocus]->setStateSelected(false);
            selectNextColumn(arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT ? -1 : 1);
            mSkillButtons[mControllerFocus]->setStateSelected(true);
        }

        updateControllerHighlight();
        return true;
    }

    MyGUI::Widget* SelectSkillDialog::getControllerFocusTooltipWidget() const
    {
        if (!Settings::gui().mControllerMenus || mSkillButtons.empty())
            return nullptr;
        if (mControllerFocus >= mSkillButtons.size())
            return nullptr;
        MyGUI::Widget* widget = mSkillButtons[mControllerFocus];
        if (widget && widget->isUserString("ToolTipType"))
            return widget;
        return nullptr;
    }

    void SelectSkillDialog::updateControllerHighlight()
    {
        if (!mControllerHighlight || !useControllerSelectionHighlight() || mSkillButtons.empty())
        {
            if (mControllerHighlight)
                mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focusWidget = mSkillButtons[mControllerFocus];
        if (!focusWidget)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord parentRect
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        const MyGUI::IntCoord focusRect = focusWidget->getAbsoluteCoord();
        mControllerHighlight->setCoord(MyGUI::IntCoord(
            focusRect.left - parentRect.left, focusRect.top - parentRect.top, focusRect.width, focusRect.height));
        mControllerHighlight->setVisible(true);
    }

    void SelectSkillDialog::selectNextColumn(int direction)
    {
        // Find which column (specialization) the current index is in.
        size_t specialization = 0;
        size_t nextSpecializationIndex = 0;
        for (; specialization < mNumSkillsPerSpecialization.size(); ++specialization)
        {
            nextSpecializationIndex += mNumSkillsPerSpecialization[specialization];
            if (mControllerFocus < nextSpecializationIndex)
                break;
        }

        if (direction < 0)
        {
            if (mControllerFocus < mNumSkillsPerSpecialization[0])
            {
                // Wrap around to the right column
                for (size_t i = 0; i < mNumSkillsPerSpecialization.size() - 1; ++i)
                    mControllerFocus += mNumSkillsPerSpecialization[i];
            }
            else
                mControllerFocus -= mNumSkillsPerSpecialization[specialization];
        }
        else
        {
            if (mControllerFocus + mNumSkillsPerSpecialization.back() >= mSkillButtons.size())
            {
                // Wrap around to the left column
                for (size_t i = 0; i < mNumSkillsPerSpecialization.size() - 1; ++i)
                    mControllerFocus -= mNumSkillsPerSpecialization[i];
            }
            else
                mControllerFocus += mNumSkillsPerSpecialization[specialization];
        }
    }

    /* DescriptionDialog */

    DescriptionDialog::DescriptionDialog()
        : WindowModal("openmw_chargen_class_description.layout")
    {
        // Centre dialog
        center();

        getWidget(mTextEdit, "TextEdit");
        mTextEdit->setUserString("ForceEmptyTooltipBar", "true");

        MyGUI::Button* okButton;
        getWidget(okButton, "OKButton");
        okButton->eventMouseButtonClick += MyGUI::newDelegate(this, &DescriptionDialog::onOkClicked);
        okButton->setCaption(
            MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString("sInputMenu1", {})));

        // Make sure the edit box has focus
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mTextEdit);

        mControllerButtons.mA = "#{Interface:ShowKeyboard}";
        mControllerButtons.mB = "#{Interface:Back}";
    }

    DescriptionDialog::~DescriptionDialog() {}

    // widget controls

    void DescriptionDialog::onOkClicked(MyGUI::Widget* /*sender*/)
    {
        eventDone(this);
    }

    void DescriptionDialog::openVirtualKeyboard()
    {
        if (!mTextEdit)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->isVirtualKeyboardVisible())
            return;

        mTextEdit->setEditStatic(false);
        winMgr->setKeyFocusWidget(mTextEdit);
        winMgr->toggleVirtualKeyboard();
    }

    void setClassImage(MyGUI::ImageBox* imageBox, const ESM::RefId& classId)
    {
        std::string_view fallback = "textures\\levelup\\warrior.dds";
        std::string classImage;
        if (const auto* id = classId.getIf<ESM::StringRefId>())
        {
            const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
            classImage = Misc::ResourceHelpers::correctTexturePath(
                VFS::Path::toNormalized("textures\\levelup\\" + id->getValue() + ".dds"), *vfs);
            if (!vfs->exists(classImage))
            {
                Log(Debug::Warning) << "No class image for " << classId << ", falling back to default";
                classImage = fallback;
            }
        }
        else
            classImage = fallback;
        imageBox->setImageTexture(classImage);
    }

    bool DescriptionDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            openVirtualKeyboard();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onOkClicked(nullptr);
            return true;
        }
        return false;
    }
}
