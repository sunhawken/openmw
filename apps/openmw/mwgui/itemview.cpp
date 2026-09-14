#include "itemview.hpp"

#include <algorithm>
#include <cmath>

#include <MyGUI_FactoryManager.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_ScrollView.h>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "itemmodel.hpp"
#include "itemwidget.hpp"

namespace MWGui
{

    ItemView::ItemView()
        : mScrollView(nullptr)
        , mControllerActiveWindow(false)
    {
    }

    void ItemView::setModel(std::unique_ptr<ItemModel> model)
    {
        mModel = std::move(model);

        update();
    }

    void ItemView::initialiseOverride()
    {
        Base::initialiseOverride();

        assignWidget(mScrollView, "ScrollView");
        if (mScrollView == nullptr)
            throw std::runtime_error("Item view needs a scroll view");

        mScrollView->setCanvasAlign(MyGUI::Align::Left | MyGUI::Align::Top);
    }

    void ItemView::layoutWidgets()
    {
        if (!mScrollView->getChildCount())
            return;

        int x = 0;
        int y = 0;
        MyGUI::Widget* dragArea = mScrollView->getChildAt(0);
        int maxHeight = mScrollView->getHeight();

        mRows = std::max(maxHeight / 42, 1);
        mItemCount = static_cast<int>(dragArea->getChildCount());
        bool showScrollbar = static_cast<int>(std::ceil(mItemCount / float(mRows))) > mScrollView->getWidth() / 42;
        if (showScrollbar)
        {
            maxHeight -= 18;
            mRows = std::max(maxHeight / 42, 1);
        }

        for (int i = 0; i < mItemCount; ++i)
        {
            MyGUI::Widget* w = dragArea->getChildAt(i);

            w->setPosition(x, y);

            y += 42;

            if (y > maxHeight - 42 && i < mItemCount - 1)
            {
                x += 42;
                y = 0;
            }
        }
        x += 42;

        MyGUI::IntSize size = MyGUI::IntSize(std::max(mScrollView->getSize().width, x), mScrollView->getSize().height);

        if (Settings::gui().mControllerMenus && mControllerActiveWindow)
        {
            if (mItemCount > 0)
                mControllerFocus = std::clamp(mControllerFocus, 0, mItemCount - 1);
            else
                mControllerFocus = -1;
            updateControllerFocus(-1, mControllerFocus);
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden
        mScrollView->setVisibleVScroll(false);
        mScrollView->setVisibleHScroll(false);
        mScrollView->setCanvasSize(size);
        mScrollView->setVisibleVScroll(true);
        mScrollView->setVisibleHScroll(true);
        dragArea->setSize(size);
    }

    void ItemView::update()
    {
        while (mScrollView->getChildCount())
            MyGUI::Gui::getInstance().destroyWidget(mScrollView->getChildAt(0));

        mItemCount = 0;

        if (!mModel)
        {
            if (mControllerFocus >= 0)
                mControllerFocus = -1;
            return;
        }

        mModel->update();

        MyGUI::Widget* dragArea = mScrollView->createWidget<MyGUI::Widget>(
            {}, 0, 0, mScrollView->getWidth(), mScrollView->getHeight(), MyGUI::Align::Stretch);
        dragArea->setNeedMouseFocus(true);
        dragArea->eventMouseButtonClick += MyGUI::newDelegate(this, &ItemView::onSelectedBackground);
        dragArea->eventMouseWheel += MyGUI::newDelegate(this, &ItemView::onMouseWheelMoved);

        for (ItemModel::ModelIndex i = 0; i < static_cast<int>(mModel->getItemCount()); ++i)
        {
            const ItemStack& item = mModel->getItem(i);

            ItemWidget* itemWidget = dragArea->createWidget<ItemWidget>(
                "MW_ItemIcon", MyGUI::IntCoord(0, 0, 42, 42), MyGUI::Align::Default);
            itemWidget->setUserString("ToolTipType", "ItemModelIndex");
            itemWidget->setUserData(std::make_pair(i, mModel.get()));
            ItemWidget::ItemState state = ItemWidget::None;
            if (item.mType == ItemStack::Type_Barter)
                state = ItemWidget::Barter;
            if (item.mType == ItemStack::Type_Equipped)
                state = ItemWidget::Equip;
            itemWidget->setItem(item.mBase, state);
            itemWidget->setCount(static_cast<int>(item.mCount));

            itemWidget->eventMouseButtonClick += MyGUI::newDelegate(this, &ItemView::onSelectedItem);
            itemWidget->eventMouseWheel += MyGUI::newDelegate(this, &ItemView::onMouseWheelMoved);
        }

        layoutWidgets();
    }

    void ItemView::resetScrollBars()
    {
        mScrollView->setViewOffset(MyGUI::IntPoint(0, 0));
        if (Settings::gui().mControllerMenus)
        {
            const int newFocus = (mItemCount > 0) ? 0 : -1;
            updateControllerFocus(mControllerFocus, newFocus);
            mControllerFocus = newFocus;
        }
    }

    void ItemView::onSelectedItem(MyGUI::Widget* sender)
    {
        ItemModel::ModelIndex index = (*sender->getUserData<std::pair<ItemModel::ModelIndex, ItemModel*>>()).first;
        eventItemClicked(index);
    }

    void ItemView::onSelectedBackground(MyGUI::Widget* /*sender*/)
    {
        eventBackgroundClicked();
    }

    void ItemView::onMouseWheelMoved(MyGUI::Widget* /*sender*/, int rel)
    {
        if (mScrollView->getViewOffset().left + rel * 0.3f > 0)
            mScrollView->setViewOffset(MyGUI::IntPoint(0, 0));
        else
            mScrollView->setViewOffset(
                MyGUI::IntPoint(static_cast<int>(mScrollView->getViewOffset().left + rel * 0.3f), 0));
    }

    void ItemView::setSize(const MyGUI::IntSize& value)
    {
        bool changed = (value.width != getWidth() || value.height != getHeight());
        Base::setSize(value);
        if (changed)
            layoutWidgets();
    }

    void ItemView::setCoord(const MyGUI::IntCoord& value)
    {
        bool changed = (value.width != getWidth() || value.height != getHeight());
        Base::setCoord(value);
        if (changed)
            layoutWidgets();
    }

    void ItemView::registerComponents()
    {
        MyGUI::FactoryManager::getInstance().registerFactory<MWGui::ItemView>("Widget");
    }

    void ItemView::setActiveControllerWindow(bool active)
    {
        mControllerActiveWindow = active;

        MWBase::Environment::get().getWindowManager()->setControllerTooltipVisible(
            active && Settings::gui().mControllerTooltips);

        if (active)
        {
            if (mItemCount > 0)
                mControllerFocus = std::clamp(mControllerFocus, 0, mItemCount - 1);
            else
                mControllerFocus = -1;
            updateControllerFocus(-1, mControllerFocus);
        }
        else
            updateControllerFocus(mControllerFocus, -1);
    }

    void ItemView::setControllerFocusIndex(int index)
    {
        const int prevFocus = mControllerFocus;

        if (index < 0 || mItemCount <= 0)
            mControllerFocus = -1;
        else
            mControllerFocus = std::clamp(index, 0, mItemCount - 1);

        if (Settings::gui().mControllerMenus && mControllerActiveWindow)
        {
            if (prevFocus != mControllerFocus)
                updateControllerFocus(prevFocus, mControllerFocus);
            else
                updateControllerFocus(-1, mControllerFocus);
        }
    }

    void ItemView::setControllerFocusToItem(const MWWorld::Ptr& item)
    {
        if (!mModel)
            return;

        const int itemCount = static_cast<int>(mModel->getItemCount());
        if (itemCount <= 0)
            return;

        int newFocus = -1;
        for (int i = 0; i < itemCount; ++i)
        {
            if (mModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase == item)
            {
                newFocus = i;
                break;
            }
        }

        if (newFocus < 0)
            return;

        const int prevFocus = mControllerFocus;
        mControllerFocus = newFocus;

        if (Settings::gui().mControllerMenus && mControllerActiveWindow)
            updateControllerFocus(prevFocus, mControllerFocus);
    }

    bool ItemView::isControllerFocusTopRow() const
    {
        if (mControllerFocus < 0 || mRows <= 0)
            return false;
        return (mControllerFocus % mRows) == 0;
    }

    bool ItemView::isControllerFocusBottomRow() const
    {
        if (mControllerFocus < 0 || mRows <= 0)
            return false;
        if (mControllerFocus >= mItemCount - 1)
            return true;
        return (mControllerFocus % mRows) == mRows - 1;
    }

    void ItemView::refreshControllerFocus()
    {
        updateControllerFocus(-1, mControllerFocus);
    }

    void ItemView::onControllerButton(const unsigned char button)
    {
        if (!mItemCount || !mScrollView->getChildCount())
            return;

        int prevFocus = mControllerFocus;
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        switch (button)
        {
            case SDL_CONTROLLER_BUTTON_A:
                // Select the focused item, if any.
                if (mControllerFocus >= 0 && mControllerFocus < mItemCount)
                {
                    if (!mScrollView->getChildCount())
                        return;
                    MyGUI::Widget* dragArea = mScrollView->getChildAt(0);
                    if (!dragArea || dragArea->getChildCount() <= static_cast<size_t>(mControllerFocus))
                        return;
                    onSelectedItem(dragArea->getChildAt(mControllerFocus));
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                winMgr->restoreControllerTooltips();
                if (mControllerFocus % mRows == 0)
                    mControllerFocus = std::min(mControllerFocus + mRows - 1, mItemCount - 1);
                else
                    mControllerFocus--;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                winMgr->restoreControllerTooltips();
                if (mControllerFocus % mRows == mRows - 1 || mControllerFocus == mItemCount - 1)
                    mControllerFocus -= mControllerFocus % mRows;
                else
                    mControllerFocus++;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                winMgr->restoreControllerTooltips();
                if (mControllerFocus >= mRows)
                    mControllerFocus -= mRows;
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                winMgr->restoreControllerTooltips();
                if (mControllerFocus + mRows < mItemCount)
                    mControllerFocus += mRows;
                else if (mControllerFocus / mRows != (mItemCount - 1) / mRows)
                    mControllerFocus = mItemCount - 1;
                break;
            default:
                return;
        }

        if (prevFocus != mControllerFocus)
            updateControllerFocus(prevFocus, mControllerFocus);
        else
            updateControllerFocus(-1, mControllerFocus);
    }

    void ItemView::updateControllerFocus(int prevFocus, int newFocus)
    {
        if (!mItemCount || !mScrollView->getChildCount())
            return;

        MyGUI::Widget* dragArea = mScrollView->getChildAt(0);
        if (!dragArea)
            return;

        const int safeCount = std::min(mItemCount, static_cast<int>(dragArea->getChildCount()));
        if (safeCount <= 0)
            return;

        if (prevFocus >= 0 && prevFocus < safeCount)
        {
            ItemWidget* prev = static_cast<ItemWidget*>(dragArea->getChildAt(prevFocus));
            if (prev)
                prev->setControllerFocus(false);
        }

        if (mControllerActiveWindow && newFocus >= 0 && newFocus < safeCount)
        {
            ItemWidget* focused = static_cast<ItemWidget*>(dragArea->getChildAt(newFocus));
            if (focused)
            {
                focused->setControllerFocus(true);

                // Scroll the list to keep the active item in view
                int column = newFocus / mRows;
                if (column <= 3)
                    mScrollView->setViewOffset(MyGUI::IntPoint(0, 0));
                else
                    mScrollView->setViewOffset(MyGUI::IntPoint(-42 * (column - 3), 0));

                MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
                winMgr->restoreControllerTooltips();
                if (winMgr->getControllerTooltipVisible())
                    MWBase::Environment::get().getInputManager()->warpMouseToWidget(focused);
            }
        }
    }
}
