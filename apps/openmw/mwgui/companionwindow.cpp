#include "companionwindow.hpp"

#include <cmath>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"

#include "companionitemmodel.hpp"
#include "countdialog.hpp"
#include "draganddrop.hpp"
#include "itemtransfer.hpp"
#include "itemview.hpp"
#include "messagebox.hpp"
#include "sortfilteritemmodel.hpp"
#include "tooltips.hpp"
#include "widgets.hpp"

namespace
{

    int getProfit(const MWWorld::Ptr& actor)
    {
        const ESM::RefId& script = actor.getClass().getScript(actor);
        if (!script.empty())
        {
            return actor.getRefData().getLocals().getIntVar(script, "minimumprofit");
        }
        return 0;
    }

}

namespace MWGui
{

    CompanionWindow::CompanionWindow(DragAndDrop& dragAndDrop, ItemTransfer& itemTransfer, MessageBoxManager* manager)
        : WindowBase("openmw_companion_window.layout")
        , mSortModel(nullptr)
        , mModel(nullptr)
        , mSelectedItem(-1)
        , mUpdateNextFrame(false)
        , mDragAndDrop(&dragAndDrop)
        , mItemTransfer(&itemTransfer)
        , mMessageBoxManager(manager)
    {
        getWidget(mCloseButton, "CloseButton");
        getWidget(mProfitLabel, "ProfitLabel");
        getWidget(mEncumbranceBar, "EncumbranceBar");
        mFilterEdit = nullptr;
        getWidget(mItemView, "ItemView");
        mItemView->eventBackgroundClicked += MyGUI::newDelegate(this, &CompanionWindow::onBackgroundSelected);
        mItemView->eventItemClicked += MyGUI::newDelegate(this, &CompanionWindow::onItemSelected);

        mCloseButton->eventMouseButtonClick += MyGUI::newDelegate(this, &CompanionWindow::onCloseButtonClicked);

        setCoord(200, 0, 600, 300);

        mControllerButtons = {};
        mControllerButtons.mA = "#{Interface:Take}";
        mControllerButtons.mB = "#{Interface:Close}";
        mControllerButtons.mY = "#{Interface:Info}";
        mControllerButtons.mR2 = "#{Interface:Inventory}";
    }

    void CompanionWindow::onItemSelected(int index)
    {
        if (!mSortModel || !mModel)
            return;
        if (index < 0 || static_cast<size_t>(index) >= mSortModel->getItemCount())
            return;
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            mDragAndDrop->drop(mModel, mItemView);
            updateEncumbranceBar();
            return;
        }

        const ItemStack& item = mSortModel->getItem(index);

        // We can't take conjured items from a companion actor
        if (item.mFlags & ItemStack::Flag_Bound)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sBarterDialog12}");
            return;
        }

        MWWorld::Ptr object = item.mBase;
        size_t count = item.mCount;
        bool shift = MyGUI::InputManager::getInstance().isShiftPressed();
        if (MyGUI::InputManager::getInstance().isControlPressed())
            count = 1;

        mSelectedItem = mSortModel->mapToSource(index);
        if (mSelectedItem < 0)
            return;

        if (count > 1 && !shift)
        {
            CountDialog* dialog = MWBase::Environment::get().getWindowManager()->getCountDialog();
            std::string name{ object.getClass().getName(object) };
            name += MWGui::ToolTips::getSoulString(object.getCellRef());
            dialog->openCountDialog(name, "#{sTake}", static_cast<int>(count), this);
            dialog->eventOkClicked.clear();
            if (Settings::gui().mControllerMenus || MyGUI::InputManager::getInstance().isAltPressed())
                dialog->eventOkClicked += MyGUI::newDelegate(this, &CompanionWindow::transferItem);
            else
                dialog->eventOkClicked += MyGUI::newDelegate(this, &CompanionWindow::dragItem);
        }
        else if (Settings::gui().mControllerMenus || MyGUI::InputManager::getInstance().isAltPressed())
            transferItem(nullptr, count);
        else
            dragItem(nullptr, count);
    }

    void CompanionWindow::onNameFilterChanged(MyGUI::EditBox* sender)
    {
        if (mSortModel)
            mSortModel->setNameFilter(sender->getCaption());
        mItemView->update();
    }

    void CompanionWindow::dragItem(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        if (!mModel || !mSortModel)
            return;
        if (mSelectedItem < 0 || static_cast<size_t>(mSelectedItem) >= mModel->getItemCount())
            return;
        mDragAndDrop->startDrag(mSelectedItem, mSortModel, mModel, mItemView, count);
    }

    void CompanionWindow::transferItem(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        if (!mModel)
            return;
        if (mSelectedItem < 0 || static_cast<size_t>(mSelectedItem) >= mModel->getItemCount())
            return;
        mItemTransfer->apply(mModel->getItem(mSelectedItem), count, *mItemView);
    }

    void CompanionWindow::onBackgroundSelected()
    {
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            mDragAndDrop->drop(mModel, mItemView);
            updateEncumbranceBar();
        }
    }

    void CompanionWindow::setPtr(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
            throw std::runtime_error("Invalid argument in CompanionWindow::setPtr");
        mPtr = actor;
        mItemTransfer->addTarget(*mItemView);
        if (auto* inventory = MWBase::Environment::get().getWindowManager()->getInventoryWindow())
            mItemTransfer->addTarget(*inventory->getItemView());
        auto model = std::make_unique<CompanionItemModel>(actor);
        mModel = model.get();
        auto sortModel = std::make_unique<SortFilterItemModel>(std::move(model));
        mSortModel = sortModel.get();
        if (mFilterEdit)
            mFilterEdit->setCaption({});
        mItemView->setModel(std::move(sortModel));
        mItemView->resetScrollBars();

        setTitle(actor.getClass().getName(actor));
        updateEncumbranceBar();
    }

    void CompanionWindow::onFrame(float dt)
    {
        checkReferenceAvailable();

        if (mUpdateNextFrame)
        {
            updateEncumbranceBar();
            mItemView->update();
            mUpdateNextFrame = false;
        }
    }

    void CompanionWindow::updateEncumbranceBar()
    {
        if (mPtr.isEmpty())
            return;
        int capacity = static_cast<int>(mPtr.getClass().getCapacity(mPtr));
        float encumbrance = std::ceil(mPtr.getClass().getEncumbrance(mPtr));
        mEncumbranceBar->setValue(static_cast<int>(encumbrance), capacity);

        if (mModel && mModel->hasProfit(mPtr))
        {
            mProfitLabel->setCaptionWithReplacing("#{sProfitValue} " + MyGUI::utility::toString(getProfit(mPtr)));
        }
        else
            mProfitLabel->setCaption({});
    }

    void CompanionWindow::onCloseButtonClicked(MyGUI::Widget* /*sender*/)
    {
        if (exit())
            MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Companion);
    }

    bool CompanionWindow::exit()
    {
        if (mModel && mModel->hasProfit(mPtr) && getProfit(mPtr) < 0)
        {
            std::vector<std::string> buttons;
            buttons.emplace_back("#{sCompanionWarningButtonOne}");
            buttons.emplace_back("#{sCompanionWarningButtonTwo}");
            mMessageBoxManager->createInteractiveMessageBox("#{sCompanionWarningMessage}", buttons);
            mMessageBoxManager->eventButtonPressed
                += MyGUI::newDelegate(this, &CompanionWindow::onMessageBoxButtonClicked);
            return false;
        }
        return true;
    }

    void CompanionWindow::onMessageBoxButtonClicked(int button)
    {
        if (button == 0)
        {
            MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Companion);
            // Important for Calvus' contract script to work properly
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        }
    }

    void CompanionWindow::onReferenceUnavailable()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Companion);
    }

    void CompanionWindow::resetReference()
    {
        ReferenceInterface::resetReference();
        mItemView->setModel(nullptr);
        mModel = nullptr;
        mSortModel = nullptr;
    }

    void CompanionWindow::onInventoryUpdate(const MWWorld::Ptr& ptr)
    {
        if (ptr == mPtr)
            mUpdateNextFrame = true;
    }

    void CompanionWindow::onOpen()
    {
        resetFixedWindowGeometry();
        mItemTransfer->addTarget(*mItemView);
        if (auto* inventory = MWBase::Environment::get().getWindowManager()->getInventoryWindow())
            mItemTransfer->addTarget(*inventory->getItemView());
    }

    void CompanionWindow::onClose()
    {
        // Only remove transfer targets when leaving companion mode entirely.
        if (MWBase::Environment::get().getWindowManager()->containsMode(GM_Companion))
            return;
        mItemTransfer->removeTarget(*mItemView);
    }

    void CompanionWindow::resetFixedWindowGeometry()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            const float scale = 0.85f;
            float targetWidth = viewSize.width * scale;
            float targetHeight = targetWidth * 10.f / 16.f;
            const float maxHeight = viewSize.height * scale;
            int width = static_cast<int>(targetWidth);
            int height = static_cast<int>(targetHeight);
            if (height > maxHeight)
            {
                height = static_cast<int>(maxHeight);
                width = static_cast<int>(height * 16.f / 10.f);
            }
            const int x = (viewSize.width - width) / 2;
            const int y = (viewSize.height - height) / 2;
            window->setCoord(x, y, width, height);
        }
    }

    bool CompanionWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (!mSortModel)
                return true;
            int index = mItemView->getControllerFocus();
            const size_t modelCount = mSortModel->getItemCount();
            if (index >= 0 && static_cast<size_t>(index) < modelCount)
                onItemSelected(index);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCloseButtonClicked(mCloseButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK || arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP
            || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN || arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT
            || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            mItemView->onControllerButton(arg.button);
        }

        return true;
    }

    void CompanionWindow::setActiveControllerWindow(bool active)
    {
        mItemView->setActiveControllerWindow(active);
        if (active)
            mItemView->refreshControllerFocus();
        WindowBase::setActiveControllerWindow(active);
    }
}
