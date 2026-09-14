#include "scrollwindow.hpp"

#include <MyGUI_ScrollView.h>

#include <components/esm3/loadbook.hpp>
#include <components/esm4/loadbook.hpp>
#include <components/widgets/imagebutton.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "../mwworld/actiontake.hpp"
#include "../mwworld/class.hpp"

#include "formatting.hpp"

namespace MWGui
{

    ScrollWindow::ScrollWindow()
        : BookWindowBase("openmw_scroll.layout")
        , mTakeButtonShow(true)
        , mTakeButtonAllowed(true)
    {
        getWidget(mTextView, "TextView");

        getWidget(mCloseButton, "CloseButton");
        mCloseButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onCloseButtonClicked);

        getWidget(mTakeButton, "TakeButton");
        mTakeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &ScrollWindow::onTakeButtonClicked);
        mTakeButton->setVisible(false);

        adjustButton("CloseButton");
        adjustButton("TakeButton");

        mCloseButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);
        mTakeButton->eventKeyButtonPressed += MyGUI::newDelegate(this, &ScrollWindow::onKeyButtonPressed);

        mControllerScrollWidget = mTextView;
        mControllerButtons.mB = "#{Interface:Close}";

        center();
    }

    void ScrollWindow::setPtr(const MWWorld::Ptr& scroll)
    {
        if (scroll.isEmpty() || (scroll.getType() != ESM::REC_BOOK && scroll.getType() != ESM::REC_BOOK4))
            throw std::runtime_error("Invalid argument in ScrollWindow::setPtr");
        mScroll = scroll;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        bool showTakeButton = scroll.getContainerStore() != &player.getClass().getContainerStore(player);

        static const std::string kEmptyText;
        const std::string* text = &kEmptyText;
        const bool shrinkTextAtLastTag = scroll.getType() == ESM::REC_BOOK;

        bool isMagicScroll = false;
        if (scroll.getType() == ESM::REC_BOOK)
        {
            const auto* ref = scroll.get<ESM::Book>();
            const ESM::Book* book = ref ? ref->mBase : nullptr;
            if (book)
            {
                text = &book->mText;
                isMagicScroll = book->mData.mIsScroll && !book->mEnchant.empty();
            }
        }
        else if (scroll.getType() == ESM::REC_BOOK4)
        {
            const auto* ref = scroll.get<ESM4::Book>();
            const ESM4::Book* book = ref ? ref->mBase : nullptr;
            if (book)
            {
                text = &book->mText;
                isMagicScroll = (book->mData.flags & ESM4::Book::Flag_Scroll) && !book->mEnchantment.isZeroOrUnset();
            }
        }

        Formatting::BookFormatter formatter(/*useDialogueBoldFont=*/!isMagicScroll);
        formatter.markupToWidget(mTextView, *text, 390, mTextView->getHeight(), shrinkTextAtLastTag);
        MyGUI::IntSize size;
        if (mTextView->getChildCount() > 0)
            size = mTextView->getChildAt(0)->getSize();
        else
            size = MyGUI::IntSize(0, 0);

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mTextView->setVisibleVScroll(false);
        if (size.height > mTextView->getSize().height)
            mTextView->setCanvasSize(mTextView->getWidth(), size.height);
        else
            mTextView->setCanvasSize(mTextView->getWidth(), mTextView->getSize().height);
        mTextView->setVisibleVScroll(true);

        mTextView->setViewOffset(MyGUI::IntPoint(0, 0));

        setTakeButtonShow(showTakeButton);

        updateControllerScrollButtons();
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mCloseButton);
    }

    void ScrollWindow::onKeyButtonPressed(MyGUI::Widget* /*sender*/, MyGUI::KeyCode key, MyGUI::Char character)
    {
        int scroll = 0;
        if (key == MyGUI::KeyCode::ArrowUp)
            scroll = 40;
        else if (key == MyGUI::KeyCode::ArrowDown)
            scroll = -40;

        if (scroll != 0)
            mTextView->setViewOffset(mTextView->getViewOffset() + MyGUI::IntPoint(0, scroll));
    }

    void ScrollWindow::setTakeButtonShow(bool show)
    {
        mTakeButtonShow = show;
        mTakeButton->setVisible(false);
    }

    void ScrollWindow::setInventoryAllowed(bool allowed)
    {
        mTakeButtonAllowed = allowed;
        mTakeButton->setVisible(false);
    }

    void ScrollWindow::onCloseButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onTakeButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Item Book Up"));

        MWWorld::ActionTake take(mScroll);
        take.execute(MWMechanics::getPlayer());

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Scroll);
    }

    void ScrollWindow::onClose()
    {
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getInputManager()->setGamepadGuiCursorEnabled(true);
        BookWindowBase::onClose();
    }

    ControllerButtons* ScrollWindow::getControllerButtons()
    {
        updateControllerScrollButtons();
        if (canTake())
            mControllerButtons.mA = "#{Interface:Take}";
        else
            mControllerButtons.mA.clear();
        return &mControllerButtons;
    }

    bool ScrollWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (canTake())
                onTakeButtonClicked(mTakeButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCloseButtonClicked(mCloseButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            return false; // Fall through to keyboard

        return true;
    }

    bool ScrollWindow::onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg)
    {
        const bool isTrigger
            = arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        if (!isTrigger)
            return false;

        if (!isScrollable())
            return true;

        const int direction = (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? 1 : -1;
        constexpr int kScrollStep = 1;
        mTextView->setViewOffset(mTextView->getViewOffset() + MyGUI::IntPoint(0, direction * kScrollStep));
        return true;
    }

    bool ScrollWindow::canTake() const
    {
        return mTakeButtonShow && mTakeButtonAllowed;
    }

    void ScrollWindow::updateControllerScrollButtons()
    {
        const bool scrollable = isScrollable();
        mControllerButtons.mL2 = scrollable ? "#{Interface:ScrollUp}" : "";
        mControllerButtons.mR2 = scrollable ? "#{Interface:ScrollDown}" : "";
        mControllerButtons.mDpad.clear();
    }

    bool ScrollWindow::isScrollable() const
    {
        if (!mTextView)
            return false;
        return mTextView->getCanvasSize().height > mTextView->getHeight();
    }
}
