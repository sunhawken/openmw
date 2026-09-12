#ifndef MWGUI_CLASS_H
#define MWGUI_CLASS_H

#include <array>
#include <memory>

#include <MyGUI_EditBox.h>

#include <components/esm/attr.hpp>
#include <components/esm/refid.hpp>
#include <components/esm3/loadclas.hpp>

#include "widgets.hpp"
#include "windowbase.hpp"

namespace MWGui
{
    void setClassImage(MyGUI::ImageBox* imageBox, const ESM::RefId& classId);

    class InfoBoxDialog : public WindowModal
    {
    public:
        InfoBoxDialog();

        typedef std::vector<std::string> ButtonList;

        void setText(const std::string& str);
        std::string getText() const;
        void setButtons(ButtonList& buttons);

        void onOpen() override;

        bool exit() override { return false; }

        // Events
        typedef MyGUI::delegates::MultiDelegate<int> EventHandle_Int;

        /** Event : Button was clicked.\n
            signature : void method(int index)\n
        */
        EventHandle_Int eventButtonSelected;

    protected:
        void onButtonClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
        void fitToText(MyGUI::TextBox* widget);
        void layoutVertically(MyGUI::Widget* widget, int margin);
        void updateControllerHighlight();
        MyGUI::Widget* mTextBox;
        MyGUI::TextBox* mText;
        MyGUI::Widget* mButtonBar;
        std::vector<MyGUI::Button*> mButtons;
        size_t mControllerFocus = 0;
        MyGUI::Widget* mControllerHighlight = nullptr;
    };

    // Lets the player choose between 3 ways of creating a class
    class ClassChoiceDialog : public InfoBoxDialog
    {
    public:
        // Corresponds to the buttons that can be clicked
        enum ClassChoice
        {
            Class_Generate = 0,
            Class_Pick = 1,
            Class_Create = 2,
            Class_Back = 3
        };
        ClassChoiceDialog();
    };

    class GenerateClassResultDialog : public WindowModal
    {
    public:
        GenerateClassResultDialog();

        void setClassId(const ESM::RefId& classId);

        bool exit() override { return false; }

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Back button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventBack;

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

    protected:
        void onOkClicked(MyGUI::Widget* sender);
        void onBackClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        bool mOkButtonFocus = true;

    private:
        void updateControllerHighlight();
        MyGUI::ImageBox* mClassImage;
        MyGUI::TextBox* mClassName;
        MyGUI::Button* mBackButton;
        MyGUI::Button* mOkButton;
        MyGUI::Widget* mControllerHighlight = nullptr;

        ESM::RefId mCurrentClassId;
    };

    class PickClassDialog : public WindowModal
    {
    public:
        PickClassDialog();

        const ESM::RefId& getClassId() const { return mCurrentClassId; }
        void setClassId(const ESM::RefId& classId);

        void setNextButtonShow(bool shown);
        void onOpen() override;

        bool exit() override { return false; }

        MyGUI::Widget* getControllerFocusTooltipWidget() const;

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Back button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventBack;

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

    protected:
        void onSelectClass(MyGUI::ListBox* sender, size_t index);
        void onAccept(MyGUI::ListBox* sender, size_t index);
        void onListScroll(MyGUI::ListBox* sender, size_t position);

        void onOkClicked(MyGUI::Widget* sender);
        void onBackClicked(MyGUI::Widget* sender);

    private:
        void updateClasses();
        void updateStats();

        MyGUI::ImageBox* mClassImage;
        MyGUI::ListBox* mClassList;
        MyGUI::TextBox* mSpecializationName;
        MyGUI::Button* mBackButton;
        MyGUI::Button* mOkButton;
        MyGUI::Widget* mControllerHighlight = nullptr;
        Widgets::MWAttributePtr mFavoriteAttribute[2];
        Widgets::MWSkillPtr mMajorSkill[5];
        Widgets::MWSkillPtr mMinorSkill[5];

        ESM::RefId mCurrentClassId;

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
    };

    class SelectSpecializationDialog : public WindowModal
    {
    public:
        SelectSpecializationDialog();
        ~SelectSpecializationDialog();

        bool exit() override;

        ESM::Class::Specialization getSpecializationId() const { return mSpecializationId; }

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Cancel button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventCancel;

        /** Event : Dialog finished, specialization selected.\n
            signature : void method()\n
        */
        EventHandle_Void eventItemSelected;

        MyGUI::Widget* getControllerFocusTooltipWidget() const;

    protected:
        void onSpecializationClicked(MyGUI::Widget* sender);
        void onCancelClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
        void updateControllerHighlight();

        MyGUI::TextBox *mSpecialization0, *mSpecialization1, *mSpecialization2;

        ESM::Class::Specialization mSpecializationId;
        size_t mControllerFocus = 0;
        MyGUI::Widget* mControllerHighlight = nullptr;
    };

    class SelectAttributeDialog : public WindowModal
    {
    public:
        SelectAttributeDialog();
        ~SelectAttributeDialog() override = default;

        bool exit() override;

        ESM::RefId getAttributeId() const { return mAttributeId; }

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Cancel button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventCancel;

        /** Event : Dialog finished, attribute selected.\n
            signature : void method()\n
        */
        EventHandle_Void eventItemSelected;

        MyGUI::Widget* getControllerFocusTooltipWidget() const;

    protected:
        void onAttributeClicked(Widgets::MWAttributePtr sender);
        void onCancelClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        size_t mControllerFocus = 0;
        std::vector<Widgets::MWAttribute*> mAttributeButtons;

    private:
        void updateControllerHighlight();

        ESM::RefId mAttributeId;
        MyGUI::Widget* mControllerHighlight = nullptr;
    };

    class SelectSkillDialog : public WindowModal
    {
    public:
        SelectSkillDialog();
        ~SelectSkillDialog();

        bool exit() override;

        ESM::RefId getSkillId() const { return mSkillId; }

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Cancel button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventCancel;

        /** Event : Dialog finished, skill selected.\n
            signature : void method()\n
        */
        EventHandle_Void eventItemSelected;

        MyGUI::Widget* getControllerFocusTooltipWidget() const;

    protected:
        void onSkillClicked(Widgets::MWSkillPtr sender);
        void onCancelClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        size_t mControllerFocus = 0;
        std::vector<Widgets::MWSkill*> mSkillButtons;

    private:
        void updateControllerHighlight();

        ESM::RefId mSkillId;
        std::array<size_t, 3> mNumSkillsPerSpecialization{};
        MyGUI::Widget* mControllerHighlight = nullptr;

        void selectNextColumn(int direction);
    };

    class DescriptionDialog : public WindowModal
    {
    public:
        DescriptionDialog();
        ~DescriptionDialog();

        std::string getTextInput() const { return mTextEdit->getCaption(); }
        void setTextInput(const std::string& text) { mTextEdit->setCaption(text); }
        MyGUI::EditBox* getTextEdit() const { return mTextEdit; }

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

    protected:
        void onOkClicked(MyGUI::Widget* sender);
        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;

    private:
        MyGUI::EditBox* mTextEdit;
        void openVirtualKeyboard();
    };

    class CreateClassDialog : public WindowModal
    {
    public:
        CreateClassDialog();
        virtual ~CreateClassDialog();

        bool exit() override { return false; }

        std::string getName() const;
        std::string getDescription() const;
        ESM::Class::Specialization getSpecializationId() const;
        std::vector<ESM::RefId> getFavoriteAttributes() const;
        std::vector<ESM::RefId> getMajorSkills() const;
        std::vector<ESM::RefId> getMinorSkills() const;
        MyGUI::EditBox* getEditName() const { return mEditName; }
        MyGUI::Widget* getControllerFocusTooltipWidget() const;

        void setNextButtonShow(bool shown);

        // Events
        typedef MyGUI::delegates::MultiDelegate<> EventHandle_Void;

        /** Event : Back button clicked.\n
            signature : void method()\n
        */
        EventHandle_Void eventBack;

        /** Event : Dialog finished, OK button clicked.\n
            signature : void method()\n
        */
        EventHandle_WindowBase eventDone;

    protected:
        void onOkClicked(MyGUI::Widget* sender);
        void onBackClicked(MyGUI::Widget* sender);

        void onSpecializationClicked(MyGUI::Widget* sender);
        void onSpecializationSelected();
        void onAttributeClicked(Widgets::MWAttributePtr sender);
        void onAttributeSelected();
        void onSkillClicked(Widgets::MWSkillPtr sender);
        void onSkillSelected();
        void onDescriptionClicked(MyGUI::Widget* sender);
        void onDescriptionEntered(WindowBase* parWindow);
        void onDialogCancel();
        void onNameEdited(MyGUI::EditBox* sender);

        void setSpecialization(int id);

        void update();

        void buildControllerItems();
        void setControllerFocusIndex(size_t newIndex);
        void moveControllerFocusVertical(int delta);
        void moveControllerFocusHorizontal(int delta);
        void updateControllerFocusHighlight();
        void setControllerItemSelected(MyGUI::Widget* widget, bool selected);
        void openVirtualKeyboard(MyGUI::EditBox* edit);

    private:
        struct ControllerItem
        {
            MyGUI::Widget* mWidget = nullptr;
            int mColumn = 0;
            int mRow = 0;
        };

        MyGUI::EditBox* mEditName;
        MyGUI::TextBox* mSpecializationName;
        MyGUI::Button* mDescriptionButton = nullptr;
        MyGUI::Button* mBackButton = nullptr;
        MyGUI::Button* mOkButton = nullptr;
        std::vector<MyGUI::Button*> mButtons;
        Widgets::MWAttributePtr mFavoriteAttribute0, mFavoriteAttribute1;
        std::array<Widgets::MWSkillPtr, 5> mMajorSkill;
        std::array<Widgets::MWSkillPtr, 5> mMinorSkill;
        std::vector<Widgets::MWSkillPtr> mSkills;
        std::string mDescription;

        std::unique_ptr<SelectSpecializationDialog> mSpecDialog;
        std::unique_ptr<SelectAttributeDialog> mAttribDialog;
        std::unique_ptr<SelectSkillDialog> mSkillDialog;
        std::unique_ptr<DescriptionDialog> mDescDialog;

        ESM::Class::Specialization mSpecializationId;

        Widgets::MWAttributePtr mAffectedAttribute;
        Widgets::MWSkillPtr mAffectedSkill;

        bool onControllerButtonEvent(const SDL_ControllerButtonEvent& arg) override;
        std::vector<ControllerItem> mControllerItems;
        size_t mControllerFocus = 0;
        size_t mControllerNameIndex = 0;
        int mControllerNameReturnColumn = 0;
        MyGUI::Widget* mControllerFocusHighlight = nullptr;
        bool mNameHitLimit = false;
        bool mSuppressNameLimitMessage = false;
    };
}
#endif
