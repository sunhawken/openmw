#include "inventorywindow.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include <osg/Texture2D>

#include <components/misc/strings/algorithm.hpp>

#include <components/myguiplatform/myguitexture.hpp>

#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/action.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "companionwindow.hpp"
#include "container.hpp"
#include "countdialog.hpp"
#include "draganddrop.hpp"
#include "hud.hpp"
#include "inventoryitemmodel.hpp"
#include "itemtransfer.hpp"
#include "itemview.hpp"
#include "settings.hpp"
#include "sortfilteritemmodel.hpp"
#include "statswindow.hpp"
#include "tooltips.hpp"
#include "tradeitemmodel.hpp"
#include "tradewindow.hpp"

namespace
{

    bool isRightHandWeapon(const MWWorld::Ptr& item)
    {
        if (item.getClass().getType() != ESM::Weapon::sRecordId)
            return false;
        std::vector<int> equipmentSlots = item.getClass().getEquipmentSlots(item).first;
        return (!equipmentSlots.empty() && equipmentSlots.front() == MWWorld::InventoryStore::Slot_CarriedRight);
    }

}

namespace MWGui
{
    namespace
    {
        WindowSettingValues getModeSettings(GuiMode mode)
        {
            switch (mode)
            {
                case GM_Container:
                    return makeInventoryContainerWindowSettingValues();
                case GM_Companion:
                    return makeInventoryCompanionWindowSettingValues();
                case GM_Barter:
                    return makeInventoryBarterWindowSettingValues();
                default:
                    return makeInventoryWindowSettingValues();
            }
        }
    }

    InventoryWindow::InventoryWindow(DragAndDrop& dragAndDrop, ItemTransfer& itemTransfer, osg::Group* parent,
        Resource::ResourceSystem* resourceSystem)
        : WindowPinnableBase("openmw_inventory_window.layout")
        , mDragAndDrop(&dragAndDrop)
        , mItemTransfer(&itemTransfer)
        , mSelectedItem(-1)
        , mSortModel(nullptr)
        , mTradeModel(nullptr)
        , mGuiMode(GM_Inventory)
        , mLastXSize(0)
        , mLastYSize(0)
        , mPreview(std::make_unique<MWRender::InventoryPreview>(parent, resourceSystem, MWMechanics::getPlayer()))
        , mTrading(false)
        , mUpdateNextFrame(false)
        , mPendingControllerAction(ControllerAction::None)
    {
        mPreviewTexture
            = std::make_unique<MyGUIPlatform::OSGTexture>(mPreview->getTexture(), mPreview->getTextureStateSet());
        mPreview->rebuild();

        mMainWidget->castType<MyGUI::Window>()->eventWindowChangeCoord
            += MyGUI::newDelegate(this, &InventoryWindow::onWindowResize);

        getWidget(mAvatar, "Avatar");
        getWidget(mAvatarImage, "AvatarImage");
        getWidget(mEncumbranceBar, "EncumbranceBar");
        getWidget(mFilterAll, "AllButton");
        getWidget(mFilterWeapon, "WeaponButton");
        getWidget(mFilterApparel, "ApparelButton");
        getWidget(mFilterMagic, "MagicButton");
        getWidget(mFilterMisc, "MiscButton");
        getWidget(mLeftPane, "LeftPane");
        getWidget(mRightPane, "RightPane");
        getWidget(mCategories, "Categories");
        getWidget(mTradeInfoPane, "TradeInfoPane");
        getWidget(mTradeTotalBalanceLabel, "TradeTotalBalanceLabel");
        getWidget(mTradeTotalBalanceValue, "TradeTotalBalanceValue");
        getWidget(mTradePlayerGold, "TradePlayerGold");
        getWidget(mTradeMerchantGold, "TradeMerchantGold");
        getWidget(mArmorRating, "ArmorRating");
        mFilterEdit = nullptr;
        mTradeInfoPane->setVisible(false);

        mAvatarImage->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onAvatarClicked);
        mAvatarImage->setRenderItemTexture(mPreviewTexture.get());
        // The widget is Y-down, the RTT image is Y-up, so this UV is inverted
        mAvatarImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 1.f, 1.f, 0.f));

        getWidget(mItemView, "ItemView");
        mItemView->eventItemClicked += MyGUI::newDelegate(this, &InventoryWindow::onItemSelected);
        mItemView->eventBackgroundClicked += MyGUI::newDelegate(this, &InventoryWindow::onBackgroundSelected);

        mFilterAll->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onFilterChanged);
        mFilterWeapon->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onFilterChanged);
        mFilterApparel->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onFilterChanged);
        mFilterMagic->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onFilterChanged);
        mFilterMisc->eventMouseButtonClick += MyGUI::newDelegate(this, &InventoryWindow::onFilterChanged);
        mFilterAll->setStateSelected(true);

        setGuiMode(mGuiMode);

        mControllerTabWidgets = { mFilterAll, mFilterWeapon, mFilterApparel, mFilterMagic, mFilterMisc };
        if (mCategories)
        {
            mControllerTabHighlight = mCategories->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerTabHighlight->setNeedMouseFocus(false);
            mControllerTabHighlight->setVisible(false);
        }

        if (Settings::gui().mControllerMenus)
            mControllerButtons = {};

        mDefaultWindowSize = MyGUI::IntSize(600, 300);
        adjustPanes();
    }

    void InventoryWindow::adjustPanes()
    {
        const float aspect = 0.5f; // fixed aspect ratio for the avatar image
        const int leftGap = mEncumbranceBar ? mEncumbranceBar->getLeft() : 8;
        const int bottomGap = leftGap;
        const int rightGap = leftGap;
        const int paneGap = 4;
        const int tradeInfoGap = 8;
        const MyGUI::IntCoord client = mMainWidget->getClientCoord();
        const bool showTradeInfo = mTradeInfoPane->getVisible();
        const int tradeInfoHeight = showTradeInfo ? mTradeInfoPane->getHeight() : 0;
        int targetPaneHeight = client.height - bottomGap;
        if (showTradeInfo)
        {
            MWGui::TradeWindow* tradeWindow = MWBase::Environment::get().getWindowManager()->getTradeWindow();
            if (tradeWindow)
            {
                const MyGUI::IntCoord tradeClient = tradeWindow->mMainWidget->getClientCoord();
                const int tradeClientAbsTop = tradeWindow->mMainWidget->getAbsoluteCoord().top + tradeClient.top;
                if (tradeWindow->mBottomPane)
                {
                    const int tradeBottomAbsTop = tradeWindow->mBottomPane->getAbsoluteCoord().top;
                    targetPaneHeight = tradeBottomAbsTop - tradeClientAbsTop;
                }
                else if (tradeWindow->getItemView())
                {
                    const MyGUI::IntCoord tradeItemViewAbs = tradeWindow->getItemView()->getAbsoluteCoord();
                    targetPaneHeight = tradeItemViewAbs.top + tradeItemViewAbs.height - tradeClientAbsTop;
                }
            }
            if (mItemView && targetPaneHeight <= 0)
                targetPaneHeight = mItemView->getTop() + mItemView->getHeight();
        }
        const int maxPaneHeight
            = std::max(0, client.height - bottomGap - (showTradeInfo ? tradeInfoHeight + tradeInfoGap : 0));
        const int paneHeight = std::min(targetPaneHeight, maxPaneHeight);
        const int leftPaneWidth = std::max(0, static_cast<int>((paneHeight - mArmorRating->getHeight()) * aspect));
        const int rightPaneWidth = std::max(0, client.width - rightGap - leftPaneWidth - paneGap);

        mLeftPane->setCoord(0, 0, leftPaneWidth, paneHeight);
        mRightPane->setCoord(leftPaneWidth + paneGap, 0, rightPaneWidth, paneHeight);
        if (showTradeInfo)
        {
            const int tradeInfoWidth = std::min(mTradeInfoPane->getWidth(), client.width - leftGap - rightGap);
            const int tradeInfoTop = paneHeight + tradeInfoGap;
            mTradeInfoPane->setCoord(leftGap, tradeInfoTop, tradeInfoWidth, tradeInfoHeight);
        }
    }

    void InventoryWindow::resetFixedWindowGeometry()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            MyGUI::IntCoord coord = getFixedWindowCoord(viewSize);
            window->setCoord(coord);
        }
    }

    void InventoryWindow::updatePlayer()
    {
        mPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
        auto tradeModel = std::make_unique<TradeItemModel>(std::make_unique<InventoryItemModel>(mPtr), MWWorld::Ptr());
        mTradeModel = tradeModel.get();

        if (mSortModel) // reuse existing SortModel when possible to keep previous category/filter settings
            mSortModel->setSourceModel(std::move(tradeModel));
        else
        {
            auto sortModel = std::make_unique<SortFilterItemModel>(std::move(tradeModel));
            mSortModel = sortModel.get();
            mItemView->setModel(std::move(sortModel));
        }

        if (mFilterEdit)
            mSortModel->setNameFilter(mFilterEdit->getCaption());
        else
            mSortModel->setNameFilter({});

        mFilterAll->setStateSelected(true);
        mFilterWeapon->setStateSelected(false);
        mFilterApparel->setStateSelected(false);
        mFilterMagic->setStateSelected(false);
        mFilterMisc->setStateSelected(false);

        mPreview->updatePtr(mPtr);
        mPreview->rebuild();
        mPreview->update();

        dirtyPreview();

        updatePreviewSize();

        updateEncumbranceBar();
        mItemView->update();
        notifyContentChanged();
    }

    void InventoryWindow::clearFilter()
    {
        if (mFilterEdit && !mFilterEdit->getCaption().empty())
        {
            mFilterEdit->setCaption({});
            if (mSortModel)
                mSortModel->setNameFilter({});
            mItemView->update();
        }
    }

    void InventoryWindow::clear()
    {
        mPtr = MWWorld::Ptr();
        mTradeModel = nullptr;
        mSortModel = nullptr;
        mItemView->setModel(nullptr);
    }

    void InventoryWindow::toggleMaximized()
    {
        const WindowSettingValues settings = getModeSettings(mGuiMode);
        const WindowRectSettingValues& rect = settings.mIsMaximized ? settings.mRegular : settings.mMaximized;

        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        const int x = static_cast<int>(rect.mX * viewSize.width);
        const int y = static_cast<int>(rect.mY * viewSize.height);
        const int w = static_cast<int>(rect.mW * viewSize.width);
        const int h = static_cast<int>(rect.mH * viewSize.height);
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        window->setCoord(x, y, w, h);

        settings.mIsMaximized.set(!settings.mIsMaximized);

        adjustPanes();
        updatePreviewSize();
    }

    void InventoryWindow::setGuiMode(GuiMode mode)
    {
        if (Settings::gui().mControllerMenus && mGuiMode == mode && isVisible())
            return;

        mGuiMode = mode;
        setPinButtonVisible(
            mode != GM_Container && mode != GM_Companion && mode != GM_Barter && !Settings::gui().mControllerMenus);

        // For all modes, use fixed 16:10 at 85% of viewport
        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        MyGUI::IntCoord fixed = getFixedWindowCoord(viewSize);
        MyGUI::IntPoint pos(fixed.left, fixed.top);
        MyGUI::IntSize size(fixed.width, fixed.height);

        bool needUpdate = (size.width != mMainWidget->getWidth() || size.height != mMainWidget->getHeight());

        mMainWidget->setPosition(pos);
        mMainWidget->setSize(size);

        adjustPanes();

        if (needUpdate)
            updatePreviewSize();
    }

    SortFilterItemModel* InventoryWindow::getSortFilterModel()
    {
        return mSortModel;
    }

    TradeItemModel* InventoryWindow::getTradeModel()
    {
        return mTradeModel;
    }

    ItemModel* InventoryWindow::getModel()
    {
        return mTradeModel;
    }

    void InventoryWindow::onBackgroundSelected()
    {
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            if (!mTradeModel)
                return;
            mDragAndDrop->drop(mTradeModel, mItemView);
        }
    }

    int InventoryWindow::getSelectedTabIndex() const
    {
        if (mFilterAll->getStateSelected())
            return 0;
        if (mFilterWeapon->getStateSelected())
            return 1;
        if (mFilterApparel->getStateSelected())
            return 2;
        if (mFilterMagic->getStateSelected())
            return 3;
        if (mFilterMisc->getStateSelected())
            return 4;
        return 0;
    }

    void InventoryWindow::setControllerTabsActive(bool active)
    {
        if (mControllerTabWidgets.empty())
            return;

        if (!active && mItemView && mItemView->getItemCount() <= 0)
        {
            mControllerTabsActive = true;
            mControllerTabIndex = getSelectedTabIndex();
            updateControllerTabFocus(-1, mControllerTabIndex);
            mItemView->setActiveControllerWindow(false);
            return;
        }

        mControllerTabsActive = active;
        if (mControllerTabsActive)
        {
            mControllerTabIndex = getSelectedTabIndex();
            updateControllerTabFocus(-1, mControllerTabIndex);
            if (mItemView)
                mItemView->setActiveControllerWindow(false);
        }
        else
        {
            if (mControllerTabHighlight)
                mControllerTabHighlight->setVisible(false);
            mItemView->setActiveControllerWindow(true);
            mItemView->refreshControllerFocus();
            MWBase::Environment::get().getWindowManager()->setCursorVisible(false);
        }
    }

    void InventoryWindow::updateControllerTabFocus(int prevIndex, int newIndex)
    {
        if (newIndex < 0 || static_cast<size_t>(newIndex) >= mControllerTabWidgets.size())
            return;

        if (prevIndex >= 0 && static_cast<size_t>(prevIndex) < mControllerTabWidgets.size())
        {
            MyGUI::Widget* prevWidget = mControllerTabWidgets[prevIndex];
            if (mFilterEdit && prevWidget == mFilterEdit)
                MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(nullptr);
        }

        MyGUI::Widget* widget = mControllerTabWidgets[newIndex];
        if (widget == nullptr)
            return;

        if (mControllerTabHighlight)
        {
            if (mCategories)
            {
                const MyGUI::IntCoord widgetCoord = widget->getCoord();
                mControllerTabHighlight->setCoord(
                    widgetCoord.left, widgetCoord.top, widgetCoord.width, widgetCoord.height);
                mControllerTabHighlight->setVisible(true);
            }
            else
            {
                mControllerTabHighlight->setVisible(false);
            }
        }
    }

    void InventoryWindow::onItemSelected(int index)
    {
        if (!mSortModel || !mTradeModel)
            return;
        if (index < 0 || static_cast<size_t>(index) >= mSortModel->getItemCount())
            return;
        const int sourceIndex = mSortModel->mapToSource(index);
        if (sourceIndex < 0)
            return;
        onItemSelectedFromSourceModel(sourceIndex);
    }

    void InventoryWindow::onItemSelectedFromSourceModel(int index)
    {
        if (!mTradeModel)
            return;
        if (index < 0 || static_cast<size_t>(index) >= mTradeModel->getItemCount())
            return;
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            mDragAndDrop->drop(mTradeModel, mItemView);
            return;
        }

        const ItemStack& item = mTradeModel->getItem(index);
        const ESM::RefId& sound = item.mBase.getClass().getDownSoundId(item.mBase);

        MWWorld::Ptr object = item.mBase;
        size_t count = item.mCount;
        bool shift = MyGUI::InputManager::getInstance().isShiftPressed();

        if (MyGUI::InputManager::getInstance().isControlPressed())
            count = 1;

        if (mPendingControllerAction == ControllerAction::Use && count > 1)
            count = 1;

        if (mTrading)
        {
            // Can't give conjured items to a merchant
            if (item.mFlags & ItemStack::Flag_Bound)
            {
                MWBase::Environment::get().getWindowManager()->playSound(sound);
                MWBase::Environment::get().getWindowManager()->messageBox("#{sBarterDialog9}");
                return;
            }

            // check if merchant accepts item
            int services = MWBase::Environment::get().getWindowManager()->getTradeWindow()->getMerchantServices();
            if (!object.getClass().canSell(object, services))
            {
                MWBase::Environment::get().getWindowManager()->playSound(sound);
                MWBase::Environment::get().getWindowManager()->messageBox("#{sBarterDialog4}");
                return;
            }
        }

        // If we unequip weapon during attack, it can lead to unexpected behaviour
        if (MWBase::Environment::get().getMechanicsManager()->isAttackingOrSpell(mPtr))
        {
            MWWorld::InventoryStore& invStore = mPtr.getClass().getInventoryStore(mPtr);
            MWWorld::ContainerStoreIterator weapIt = invStore.getSlot(MWWorld::InventoryStore::Slot_CarriedRight);
            bool weapActive = mPtr.getClass().getCreatureStats(mPtr).getDrawState() == MWMechanics::DrawState::Weapon;
            if (weapActive && weapIt != invStore.end() && *weapIt == item.mBase)
            {
                MWBase::Environment::get().getWindowManager()->messageBox("#{sCantEquipWeapWarning}");
                return;
            }
        }

        // Show a dialog to select a count of items, but not when using an item from the inventory
        // in controller mode. In that case, we skip the dialog and just use one item immediately.
        if (count > 1 && !shift && mPendingControllerAction != ControllerAction::Use)
        {
            CountDialog* dialog = MWBase::Environment::get().getWindowManager()->getCountDialog();
            std::string message = "#{sTake}";
            if (mTrading || mPendingControllerAction == ControllerAction::Sell)
                message = "#{sQuanityMenuMessage01}";
            else if (mPendingControllerAction == ControllerAction::Drop)
                message = "#{sDrop}";
            else if (mGuiMode == MWGui::GM_Companion || mGuiMode == MWGui::GM_Container)
                message = "Give";
            std::string name{ object.getClass().getName(object) };
            name += MWGui::ToolTips::getSoulString(object.getCellRef());
            dialog->openCountDialog(name, message, static_cast<int>(count), this);
            dialog->eventOkClicked.clear();
            if (mTrading || mPendingControllerAction == ControllerAction::Sell)
                dialog->eventOkClicked += MyGUI::newDelegate(this, &InventoryWindow::sellItem);
            else if (mPendingControllerAction == ControllerAction::Drop)
                dialog->eventOkClicked += MyGUI::newDelegate(this, &InventoryWindow::dropItem);
            else if (MyGUI::InputManager::getInstance().isAltPressed()
                || mPendingControllerAction == ControllerAction::Transfer)
                dialog->eventOkClicked += MyGUI::newDelegate(this, &InventoryWindow::transferItem);
            else
                dialog->eventOkClicked += MyGUI::newDelegate(this, &InventoryWindow::dragItem);

            mSelectedItem = index;
        }
        else
        {
            mSelectedItem = index;

            if (mTrading || mPendingControllerAction == ControllerAction::Sell)
                sellItem(nullptr, count);
            else if (mPendingControllerAction == ControllerAction::Use)
                equipItem(count);
            else if (mPendingControllerAction == ControllerAction::Drop)
                dropItem(nullptr, count);
            else if (MyGUI::InputManager::getInstance().isAltPressed()
                || mPendingControllerAction == ControllerAction::Transfer)
                transferItem(nullptr, count);
            else
                dragItem(nullptr, count);
        }

        mPendingControllerAction = ControllerAction::None;
    }

    void InventoryWindow::ensureSelectedItemUnequipped(int count)
    {
        if (!mTradeModel || mSelectedItem < 0 || static_cast<size_t>(mSelectedItem) >= mTradeModel->getItemCount())
            return;
        const ItemStack& item = mTradeModel->getItem(mSelectedItem);
        if (item.mType == ItemStack::Type_Equipped)
        {
            MWWorld::InventoryStore& invStore = mPtr.getClass().getInventoryStore(mPtr);
            MWWorld::Ptr newStack = *invStore.unequipItemQuantity(item.mBase, count);

            // The unequipped item was re-stacked. We have to update the index
            // since the item pointed does not exist anymore.
            if (item.mBase != newStack)
            {
                updateItemView(); // Unequipping can produce a new stack, not yet in the window...

                // newIndex will store the index of the ItemStack the item was stacked on
                int newIndex = -1;
                for (size_t i = 0; i < mTradeModel->getItemCount(); ++i)
                {
                    if (mTradeModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase == newStack)
                    {
                        newIndex = static_cast<int>(i);
                        break;
                    }
                }

                if (newIndex == -1)
                {
                    mSelectedItem = -1;
                    return;
                }

                mSelectedItem = newIndex;
            }
        }
    }

    void InventoryWindow::dragItem(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        ensureSelectedItemUnequipped(static_cast<int>(count));
        mDragAndDrop->startDrag(mSelectedItem, mSortModel, mTradeModel, mItemView, count);
        notifyContentChanged();
    }

    void InventoryWindow::transferItem(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        ensureSelectedItemUnequipped(static_cast<int>(count));
        mItemTransfer->apply(mTradeModel->getItem(mSelectedItem), count, *mItemView);
        notifyContentChanged();
    }

    void InventoryWindow::sellItem(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        ensureSelectedItemUnequipped(static_cast<int>(count));
        const ItemStack& item = mTradeModel->getItem(mSelectedItem);
        const ESM::RefId& sound = item.mBase.getClass().getUpSoundId(item.mBase);
        MWBase::Environment::get().getWindowManager()->playSound(sound);

        if (item.mType == ItemStack::Type_Barter)
        {
            // this was an item borrowed to us by the merchant
            mTradeModel->returnItemBorrowedToUs(mSelectedItem, count);
            MWBase::Environment::get().getWindowManager()->getTradeWindow()->returnItem(mSelectedItem, count);
        }
        else
        {
            // borrow item to the merchant
            mTradeModel->borrowItemFromUs(mSelectedItem, count);
            MWBase::Environment::get().getWindowManager()->getTradeWindow()->borrowItem(mSelectedItem, count);
        }

        mItemView->update();
        notifyContentChanged();
    }

    void InventoryWindow::dropItem(MyGUI::Widget* sender, size_t count)
    {
        if (mGuiMode != MWGui::GM_Inventory)
            return;

        if (!mDragAndDrop->mIsOnDragAndDrop)
            dragItem(sender, count);

        // Drop the item into the gameworld
        if (mDragAndDrop->mIsOnDragAndDrop)
            MWBase::Environment::get().getWindowManager()->getHud()->dropDraggedItem(0.5f, 0.5f);
    }

    void InventoryWindow::equipItem(std::size_t count)
    {
        const int prevFocus = mItemView ? mItemView->getControllerFocus() : -1;

        const ItemStack preItem = mTradeModel->getItem(mSelectedItem);
        const bool wasEquipped = preItem.mType == ItemStack::Type_Equipped;
        const MWWorld::Ptr prePtr = preItem.mBase;

        ensureSelectedItemUnequipped(static_cast<int>(count));
        const MWWorld::Ptr postPtr
            = (mSelectedItem >= 0 && static_cast<size_t>(mSelectedItem) < mTradeModel->getItemCount())
            ? mTradeModel->getItem(mSelectedItem).mBase
            : MWWorld::Ptr();

        // Disable the pick up sound as the item will be used immediately
        mDragAndDrop->startDrag(mSelectedItem, mSortModel, mTradeModel, mItemView, count, false);
        notifyContentChanged();

        // Drop the item on the avatar to activate or equip it.
        if (!wasEquipped)
            onAvatarClicked(nullptr);

        // Drop the item to unequip it or drop any remaining items back in inventory.
        // This is needed when clicking on a stack of items; we only want to use the first item.
        if (mDragAndDrop->mIsOnDragAndDrop)
            mDragAndDrop->drop(mTradeModel, mItemView, wasEquipped);

        if (Settings::gui().mControllerMenus)
        {
            // Equipping can re-sort the view (equipped items are grouped first). Keep focus on the activated item.
            if (!wasEquipped)
                mItemView->setControllerFocusToItem(prePtr);
            else
            {
                // Unequipping should keep focus position stable. If the list shrank (e.g. stack merged), keep focus
                // on the resulting item instead of clamping to the previous slot.
                if (mItemView && prevFocus >= 0 && prevFocus < mItemView->getItemCount())
                    mItemView->setControllerFocusIndex(prevFocus);
                else if (!postPtr.isEmpty())
                    mItemView->setControllerFocusToItem(postPtr);
            }
        }
    }

    void InventoryWindow::updateItemView()
    {
        MWBase::Environment::get().getWindowManager()->updateSpellWindow();

        mItemView->update();

        dirtyPreview();
    }

    void InventoryWindow::onOpen()
    {
        resetFixedWindowGeometry();

        // Reset the filter focus when opening the window
        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (mFilterEdit && focus == mFilterEdit)
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(nullptr);

        if (!mPtr.isEmpty())
        {
            updateEncumbranceBar();
            mItemView->update();
            notifyContentChanged();
        }
        adjustPanes();
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getWindowManager()->setCursorVisible(false);

        mItemTransfer->addTarget(*mItemView);
    }

    void InventoryWindow::onClose()
    {
        mItemTransfer->removeTarget(*mItemView);
        if (mControllerTabHighlight)
            mControllerTabHighlight->setVisible(false);
        mControllerTabsActive = false;
    }

    void InventoryWindow::onWindowResize(MyGUI::Window* sender)
    {
        WindowBase::clampWindowCoordinates(sender);

        adjustPanes();
        const WindowSettingValues settings = getModeSettings(mGuiMode);

        MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();

        settings.mRegular.mX.set(sender->getPosition().left / static_cast<float>(viewSize.width));
        settings.mRegular.mY.set(sender->getPosition().top / static_cast<float>(viewSize.height));
        settings.mRegular.mW.set(sender->getSize().width / static_cast<float>(viewSize.width));
        settings.mRegular.mH.set(sender->getSize().height / static_cast<float>(viewSize.height));
        settings.mIsMaximized.set(false);

        if (mMainWidget->getSize().width != mLastXSize || mMainWidget->getSize().height != mLastYSize)
        {
            mLastXSize = mMainWidget->getSize().width;
            mLastYSize = mMainWidget->getSize().height;

            updatePreviewSize();
            updateArmorRating();
        }
    }

    void InventoryWindow::updateArmorRating()
    {
        if (mPtr.isEmpty())
            return;

        auto rating = MyGUI::utility::toString(static_cast<int>(mPtr.getClass().getArmorRating(mPtr, true)));
        mArmorRating->setCaptionWithReplacing("#{sArmor}: " + rating);
        if (mArmorRating->getTextSize().width > mArmorRating->getSize().width)
            mArmorRating->setCaptionWithReplacing(rating);
    }

    void InventoryWindow::updatePreviewSize()
    {
        const MyGUI::IntSize viewport = getPreviewViewportSize();
        mPreview->setViewport(viewport.width, viewport.height);
        const float top = viewport.height / static_cast<float>(mPreview->getTextureHeight());
        const float right = viewport.width / static_cast<float>(mPreview->getTextureWidth());
        // The widget is Y-down, the RTT image is Y-up, so this UV is inverted
        mAvatarImage->getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, top, right, 0.f));
    }

    void InventoryWindow::updateTradeInfo()
    {
        if (!mTrading)
            return;

        MWGui::TradeWindow* tradeWindow = MWBase::Environment::get().getWindowManager()->getTradeWindow();
        if (!tradeWindow)
            return;

        MWWorld::Ptr player = MWMechanics::getPlayer();
        int playerGold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);
        mTradePlayerGold->setCaptionWithReplacing("#{sYourGold} " + MyGUI::utility::toString(playerGold));

        TradeItemModel* playerTradeModel = getTradeModel();
        if (!playerTradeModel || !tradeWindow->mTradeModel)
            return;

        const std::vector<ItemStack>& playerBorrowed = playerTradeModel->getItemsBorrowedToUs();
        const std::vector<ItemStack>& merchantBorrowed = tradeWindow->mTradeModel->getItemsBorrowedToUs();
        int currentBalance = tradeWindow->mCurrentBalance;
        if (playerBorrowed.empty() && merchantBorrowed.empty())
            currentBalance = 0;

        if (currentBalance < 0)
            mTradeTotalBalanceLabel->setCaptionWithReplacing("#{sTotalCost}");
        else
            mTradeTotalBalanceLabel->setCaptionWithReplacing("#{sTotalSold}");

        mTradeTotalBalanceValue->setCaption(MyGUI::utility::toString(std::abs(currentBalance)));
        mTradeMerchantGold->setCaptionWithReplacing(
            "#{sSellerGold} " + MyGUI::utility::toString(tradeWindow->getMerchantGold()));
    }

    void InventoryWindow::onNameFilterChanged(MyGUI::EditBox* sender)
    {
        if (mSortModel)
            mSortModel->setNameFilter(sender->getCaption());
        mItemView->update();
    }

    void InventoryWindow::onFilterChanged(MyGUI::Widget* sender)
    {
        if (sender == mFilterAll)
            mSortModel->setCategory(SortFilterItemModel::Category_All);
        else if (sender == mFilterWeapon)
            mSortModel->setCategory(SortFilterItemModel::Category_Weapon);
        else if (sender == mFilterApparel)
            mSortModel->setCategory(SortFilterItemModel::Category_Apparel);
        else if (sender == mFilterMagic)
            mSortModel->setCategory(SortFilterItemModel::Category_Magic);
        else if (sender == mFilterMisc)
            mSortModel->setCategory(SortFilterItemModel::Category_Misc);
        mFilterAll->setStateSelected(false);
        mFilterWeapon->setStateSelected(false);
        mFilterApparel->setStateSelected(false);
        mFilterMagic->setStateSelected(false);
        mFilterMisc->setStateSelected(false);

        if (Settings::gui().mControllerMenus && mControllerTabsActive)
            mItemView->setActiveControllerWindow(false);
        mItemView->update();

        sender->castType<MyGUI::Button>()->setStateSelected(true);
        if (Settings::gui().mControllerMenus && mControllerTabsActive)
            mItemView->setActiveControllerWindow(false);
        if (Settings::gui().mControllerMenus && mItemView->getItemCount() <= 0)
            setControllerTabsActive(true);
    }

    void InventoryWindow::onPinToggled()
    {
        Settings::windows().mInventoryPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setWeaponVisibility(!mPinned);
    }

    void InventoryWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus && mGuiMode == GM_Inventory)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
            toggleMaximized();
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Inventory);
    }

    void InventoryWindow::useItem(const MWWorld::Ptr& ptr, bool force)
    {
        const ESM::RefId& script = ptr.getClass().getScript(ptr);
        if (!script.empty())
        {
            // Don't try to equip the item if PCSkipEquip is set to 1
            if (ptr.getRefData().getLocals().getIntVar(script, "pcskipequip") == 1)
            {
                ptr.getRefData().getLocals().setVarByInt(script, "onpcequip", 1);
                return;
            }
            ptr.getRefData().getLocals().setVarByInt(script, "onpcequip", 0);
        }

        MWWorld::Ptr player = MWMechanics::getPlayer();
        auto type = ptr.getType();
        bool isWeaponOrArmor = type == ESM::Weapon::sRecordId || type == ESM::Armor::sRecordId;
        bool isBroken = ptr.getClass().hasItemHealth(ptr) && ptr.getCellRef().getCharge() == 0;
        const bool isFromDragAndDrop = mDragAndDrop->mIsOnDragAndDrop && mDragAndDrop->mItem.mBase == ptr;
        const auto [canEquipResult, canEquipMsg] = ptr.getClass().canBeEquipped(ptr, mPtr);

        // In vanilla, broken armor or weapons cannot be equipped
        // tools with 0 charges is equippable
        if (isBroken && isWeaponOrArmor)
        {
            if (isFromDragAndDrop)
                mDragAndDrop->drop(mTradeModel, mItemView);
            MWBase::Environment::get().getWindowManager()->messageBox(canEquipMsg);
            return;
        }

        const bool willEquip = canEquipResult != 0 || force;

        // If the item has a script, set OnPCEquip or PCSkipEquip to 1
        if (!script.empty() && willEquip)
        {
            // Ingredients, books and repair hammers must not have OnPCEquip set to 1 here
            bool isBook = type == ESM::Book::sRecordId;
            if (!isBook && type != ESM::Ingredient::sRecordId && type != ESM::Repair::sRecordId)
                ptr.getRefData().getLocals().setVarByInt(script, "onpcequip", 1);
            // Books must have PCSkipEquip set to 1 instead
            else if (isBook)
                ptr.getRefData().getLocals().setVarByInt(script, "pcskipequip", 1);
        }

        std::unique_ptr<MWWorld::Action> action = ptr.getClass().use(ptr, force);

        MWWorld::InventoryStore& invStore = mPtr.getClass().getInventoryStore(mPtr);
        auto [eqSlots, canStack] = ptr.getClass().getEquipmentSlots(ptr);
        int useCount = isFromDragAndDrop ? static_cast<int>(mDragAndDrop->mDraggedCount) : ptr.getCellRef().getCount();

        if (!eqSlots.empty())
        {
            MWWorld::ContainerStoreIterator it = invStore.getSlot(eqSlots.front());
            if (it != invStore.end() && it->getCellRef().getRefId() == ptr.getCellRef().getRefId())
                useCount += it->getCellRef().getCount();
        }

        action->execute(player, !willEquip);

        // Partial equipping
        int excess = ptr.getCellRef().getCount() - useCount;
        if (excess > 0 && canStack)
            invStore.unequipItemQuantity(ptr, excess);

        if (isFromDragAndDrop)
        {
            // Feature: Don't finish draganddrop if potion or ingredient was used
            if (type == ESM::Potion::sRecordId || type == ESM::Ingredient::sRecordId)
                mDragAndDrop->update();
            else if (!willEquip)
                mDragAndDrop->drop(mTradeModel, mItemView);
            else
                mDragAndDrop->finish();
        }

        if (isVisible())
        {
            mItemView->update();
            notifyContentChanged();
        }
        // else: will be updated in open()
    }

    void InventoryWindow::onAvatarClicked(MyGUI::Widget* /*sender*/)
    {
        if (mDragAndDrop->mIsOnDragAndDrop)
        {
            MWWorld::Ptr ptr = mDragAndDrop->mItem.mBase;

            if (mDragAndDrop->mSourceModel != mTradeModel)
            {
                // Move item to the player's inventory
                ptr = mDragAndDrop->mSourceModel->moveItem(
                    mDragAndDrop->mItem, mDragAndDrop->mDraggedCount, mTradeModel);
            }

            MWBase::Environment::get().getLuaManager()->useItem(ptr, MWMechanics::getPlayer(), false);
        }
        else
        {
            MyGUI::IntPoint mousePos
                = MyGUI::InputManager::getInstance().getLastPressedPosition(MyGUI::MouseButton::Left);
            MyGUI::IntPoint relPos = mousePos - mAvatarImage->getAbsolutePosition();

            MWWorld::Ptr itemSelected = getAvatarSelectedItem(relPos.left, relPos.top);
            if (itemSelected.isEmpty())
                return;

            for (size_t i = 0; i < mTradeModel->getItemCount(); ++i)
            {
                if (mTradeModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase == itemSelected)
                {
                    onItemSelectedFromSourceModel(static_cast<int>(i));
                    return;
                }
            }
            throw std::runtime_error("Can't find clicked item");
        }
    }

    MWWorld::Ptr InventoryWindow::getAvatarSelectedItem(int x, int y)
    {
        const osg::Vec2i viewportCoords = mapPreviewWindowToViewport(x, y);
        int slot = mPreview->getSlotSelected(viewportCoords.x(), viewportCoords.y());

        if (slot == -1)
            return MWWorld::Ptr();

        MWWorld::InventoryStore& invStore = mPtr.getClass().getInventoryStore(mPtr);
        if (invStore.getSlot(slot) != invStore.end())
        {
            MWWorld::Ptr item = *invStore.getSlot(slot);
            if (!item.getClass().showsInInventory(item))
                return MWWorld::Ptr();
            return item;
        }

        return MWWorld::Ptr();
    }

    void InventoryWindow::updateEncumbranceBar()
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();

        int capacity = static_cast<int>(player.getClass().getCapacity(player));
        float encumbrance = player.getClass().getEncumbrance(player);
        if (mTradeModel)
            mTradeModel->adjustEncumbrance(encumbrance);
        mEncumbranceBar->setValue(static_cast<int>(std::ceil(encumbrance)), capacity);
    }

    void InventoryWindow::onFrame(float dt)
    {
        updateEncumbranceBar();

        if (mUpdateNextFrame)
        {
            if (mTrading)
            {
                mTradeModel->updateBorrowed();
                MWBase::Environment::get().getWindowManager()->getTradeWindow()->mTradeModel->updateBorrowed();
                MWBase::Environment::get().getWindowManager()->getTradeWindow()->updateItemView();
                MWBase::Environment::get().getWindowManager()->getTradeWindow()->updateOffer();
            }

            mDragAndDrop->update();
            mItemView->update();
            notifyContentChanged();
            if (Settings::gui().mControllerMenus && !mControllerTabWidgets.empty() && mItemView->getItemCount() <= 0)
                setControllerTabsActive(true);
            mUpdateNextFrame = false;
        }

        if (Settings::gui().mControllerMenus && !mControllerTabsActive && !mControllerTabWidgets.empty()
            && mItemView->getItemCount() <= 0)
            setControllerTabsActive(true);

        if (mTrading)
            updateTradeInfo();
    }

    void InventoryWindow::setTrading(bool trading)
    {
        mTrading = trading;
        mTradeInfoPane->setVisible(trading);
        if (mTrading)
            updateTradeInfo();
        adjustPanes();
    }

    void InventoryWindow::dirtyPreview()
    {
        mPreview->update();

        updateArmorRating();
    }

    void InventoryWindow::notifyContentChanged()
    {
        // update the spell window just in case new enchanted items were added to inventory
        MWBase::Environment::get().getWindowManager()->updateSpellWindow();

        MWBase::Environment::get().getMechanicsManager()->updateMagicEffects(MWMechanics::getPlayer());

        dirtyPreview();
    }

    void InventoryWindow::pickUpObject(MWWorld::Ptr object)
    {
        // If the inventory is not yet enabled, don't pick anything up
        if (!MWBase::Environment::get().getWindowManager()->isAllowed(GW_Inventory))
            return;
        // make sure the object is of a type that can be picked up
        auto type = object.getType();
        if ((type != ESM::Apparatus::sRecordId) && (type != ESM::Armor::sRecordId) && (type != ESM::Book::sRecordId)
            && (type != ESM::Clothing::sRecordId) && (type != ESM::Ingredient::sRecordId)
            && (type != ESM::Light::sRecordId) && (type != ESM::Miscellaneous::sRecordId)
            && (type != ESM::Lockpick::sRecordId) && (type != ESM::Probe::sRecordId) && (type != ESM::Repair::sRecordId)
            && (type != ESM::Weapon::sRecordId) && (type != ESM::Potion::sRecordId))
            return;

        // An object that can be picked up must have a tooltip.
        if (!object.getClass().hasToolTip(object))
            return;

        int count = object.getCellRef().getCount();
        if (object.getClass().isGold(object))
            count *= object.getClass().getValue(object);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWBase::Environment::get().getWorld()->breakInvisibility(player);

        if (!object.getRefData().activate())
            return;

        // Player must not be paralyzed, knocked down, or dead to pick up an item.
        const MWMechanics::NpcStats& playerStats = player.getClass().getNpcStats(player);
        if (playerStats.isParalyzed() || playerStats.getKnockedDown() || playerStats.isDead())
            return;

        MWBase::Environment::get().getMechanicsManager()->itemTaken(player, object, MWWorld::Ptr(), count);

        // add to player inventory
        // can't use ActionTake here because we need an MWWorld::Ptr to the newly inserted object
        MWWorld::Ptr newObject = *player.getClass().getContainerStore(player).add(object, count);

        // remove from world
        MWBase::Environment::get().getWorld()->deleteObject(object);

        // get ModelIndex to the item
        mTradeModel->update();
        size_t i = 0;
        for (; i < mTradeModel->getItemCount(); ++i)
        {
            if (mTradeModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase == newObject)
                break;
        }
        if (i == mTradeModel->getItemCount())
            throw std::runtime_error("Added item not found");

        if (mDragAndDrop->mIsOnDragAndDrop)
            mDragAndDrop->finish();

        if (MyGUI::InputManager::getInstance().isAltPressed())
        {
            const MWWorld::Ptr item = mTradeModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase;
            MWBase::Environment::get().getWindowManager()->playSound(item.getClass().getDownSoundId(item));
            mItemView->update();
        }
        else
        {
            mDragAndDrop->startDrag(static_cast<int>(i), mSortModel, mTradeModel, mItemView, count);
        }

        MWBase::Environment::get().getWindowManager()->updateSpellWindow();
    }

    void InventoryWindow::cycle(bool next)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();

        if (MWBase::Environment::get().getMechanicsManager()->isAttackingOrSpell(player))
            return;

        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        if (stats.isParalyzed() || stats.getKnockedDown() || stats.isDead() || stats.getHitRecovery())
            return;

        ItemModel::ModelIndex selected = -1;
        // not using mSortFilterModel as we only need sorting, not filtering
        SortFilterItemModel model(std::make_unique<InventoryItemModel>(player));
        model.setSortByType(false);
        model.update();
        if (model.getItemCount() == 0)
            return;

        for (ItemModel::ModelIndex i = 0; i < int(model.getItemCount()); ++i)
        {
            MWWorld::Ptr item = model.getItem(i).mBase;
            if (model.getItem(i).mType & ItemStack::Type_Equipped && isRightHandWeapon(item))
                selected = i;
        }

        int incr = next ? 1 : -1;
        bool found = false;
        ESM::RefId lastId;
        if (selected != -1)
            lastId = model.getItem(selected).mBase.getCellRef().getRefId();
        ItemModel::ModelIndex cycled = selected;
        for (size_t i = 0; i < model.getItemCount(); ++i)
        {
            cycled += incr;
            cycled = static_cast<ItemModel::ModelIndex>((cycled + model.getItemCount()) % model.getItemCount());

            MWWorld::Ptr item = model.getItem(cycled).mBase;

            // skip different stacks of the same item, or we will get stuck as stacking/unstacking them may change their
            // relative ordering
            if (lastId == item.getCellRef().getRefId())
                continue;

            lastId = item.getCellRef().getRefId();

            if (item.getClass().getType() == ESM::Weapon::sRecordId && isRightHandWeapon(item)
                && item.getClass().canBeEquipped(item, player).first)
            {
                found = true;
                break;
            }
        }

        if (!found || selected == cycled)
            return;

        useItem(model.getItem(cycled).mBase);
    }

    void InventoryWindow::rebuildAvatar()
    {
        mPreview->rebuild();
    }

    void InventoryWindow::onInventoryUpdate(const MWWorld::Ptr& ptr)
    {
        if (ptr == mPtr)
            mUpdateNextFrame = true;
    }

    MyGUI::IntSize InventoryWindow::getPreviewViewportSize() const
    {
        const MyGUI::IntSize previewWindowSize = mAvatarImage->getSize();
        const float scale = MWBase::Environment::get().getWindowManager()->getScalingFactor();

        return MyGUI::IntSize(std::min(mPreview->getTextureWidth(), static_cast<int>(previewWindowSize.width * scale)),
            std::min(mPreview->getTextureHeight(), static_cast<int>(previewWindowSize.height * scale)));
    }

    osg::Vec2i InventoryWindow::mapPreviewWindowToViewport(int x, int y) const
    {
        const MyGUI::IntSize previewWindowSize = mAvatarImage->getSize();
        const float normalisedX = x / std::max(1.f, static_cast<float>(previewWindowSize.width));
        const float normalisedY = y / std::max(1.f, static_cast<float>(previewWindowSize.height));

        const MyGUI::IntSize viewport = getPreviewViewportSize();
        return osg::Vec2i(static_cast<int>(normalisedX * (viewport.width - 1)),
            static_cast<int>((1 - normalisedY) * (viewport.height - 1)));
    }

    ControllerButtons* InventoryWindow::getControllerButtons()
    {
        mControllerButtons = {};
        switch (mGuiMode)
        {
            case MWGui::GM_Companion:
                mControllerButtons.mA = "Give";
                mControllerButtons.mB = "#{Interface:Close}";
                mControllerButtons.mY = "#{Interface:Info}";
                mControllerButtons.mL2 = "#{Interface:Container}";
                break;
            case MWGui::GM_Container:
                mControllerButtons.mA = "#{OMWEngine:InventorySelect}";
                mControllerButtons.mB = "#{Interface:Close}";
                mControllerButtons.mX = "#{Interface:TakeAll}";
                mControllerButtons.mY = "#{Interface:Info}";
                mControllerButtons.mL2 = "#{Interface:Container}";
                break;
            case MWGui::GM_Barter:
                mControllerButtons.mA = "#{Interface:Sell}";
                mControllerButtons.mB = "#{Interface:Cancel}";
                mControllerButtons.mX = "#{Interface:Offer}";
                mControllerButtons.mY = "#{Interface:Info}";
                mControllerButtons.mL2 = "#{Interface:Barter}";
                break;
            case MWGui::GM_Inventory:
            default:
                mControllerButtons.mA = "#{Interface:Select}";
                mControllerButtons.mB = "#{Interface:Back}";
                mControllerButtons.mX = "#{Interface:Drop}";
                mControllerButtons.mY = "#{Interface:Info}";
                const bool skipMap = MWBase::Environment::get().getWindowManager()->isCrassifiedNavigationEnabled();
                if (Settings::gui().mXboxTabOrder)
                {
                    mControllerButtons.mL2 = "#{sStats}";
                    mControllerButtons.mR2 = "#{sMagicMenu}";
                }
                else
                {
                    mControllerButtons.mL2 = skipMap ? "#{sStats}" : "#{sMap}";
                    mControllerButtons.mR2 = "#{sMagicMenu}";
                }
                break;
        }
        return &mControllerButtons;
    }

    bool InventoryWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        mPendingControllerAction = ControllerAction::None; // Clear any pending controller actions

        if (Settings::gui().mControllerMenus)
        {
            MWBase::Environment::get().getWindowManager()->setCursorVisible(false);
            if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            {
                if (mControllerTabWidgets.empty())
                    return true;

                const int delta = (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 1 : -1;
                const int prevIndex = mControllerTabsActive ? mControllerTabIndex : getSelectedTabIndex();
                const int newIndex
                    = static_cast<int>(wrap(static_cast<size_t>(prevIndex), mControllerTabWidgets.size(), delta));
                mControllerTabIndex = newIndex;

                if (MyGUI::Widget* widget = mControllerTabWidgets[newIndex])
                    onFilterChanged(widget);

                if (mControllerTabsActive)
                    updateControllerTabFocus(prevIndex, newIndex);

                return true;
            }
            if (mControllerTabsActive)
            {
                if (mControllerTabWidgets.empty())
                    return true;
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
                {
                    int delta = (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1 : -1;
                    int prev = mControllerTabIndex;
                    mControllerTabIndex = static_cast<int>(
                        wrap(static_cast<size_t>(mControllerTabIndex), mControllerTabWidgets.size(), delta));
                    updateControllerTabFocus(prev, mControllerTabIndex);
                    return true;
                }
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                {
                    if (mItemView->getItemCount() <= 0)
                        return true;
                    setControllerTabsActive(false);
                    return true;
                }
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
                    return true;
                if (arg.button == SDL_CONTROLLER_BUTTON_A)
                {
                    MyGUI::Widget* widget = mControllerTabWidgets[mControllerTabIndex];
                    onFilterChanged(widget);
                    MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
                    return true;
                }
            }
            else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            {
                if (mItemView->isControllerFocusTopRow())
                {
                    setControllerTabsActive(true);
                    return true;
                }
            }
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            if (mFilterEdit && !mFilterEdit->getCaption().empty())
            {
                clearFilter();
                return true;
            }
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mGuiMode == MWGui::GM_Inventory)
                mPendingControllerAction = ControllerAction::Use;
            else if (mGuiMode == MWGui::GM_Companion || mGuiMode == MWGui::GM_Container)
                mPendingControllerAction = ControllerAction::Transfer;
            else if (mGuiMode == MWGui::GM_Barter)
                mPendingControllerAction = ControllerAction::Sell;

            mItemView->onControllerButton(SDL_CONTROLLER_BUTTON_A);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            if (mGuiMode == MWGui::GM_Inventory)
            {
                mPendingControllerAction = ControllerAction::Drop;
                mItemView->onControllerButton(SDL_CONTROLLER_BUTTON_A);
            }
            else if (mGuiMode == MWGui::GM_Container)
            {
                // Take all. Pass the button press to the container window and let it do the
                // logic of taking all.
                MWGui::ContainerWindow* containerWindow = static_cast<MWGui::ContainerWindow*>(
                    MWBase::Environment::get().getWindowManager()->getGuiModeWindows(mGuiMode).at(0));
                containerWindow->onControllerButtonEvent(arg);
            }
            else if (mGuiMode == MWGui::GM_Barter)
            {
                // Offer. Pass the button press to the barter window and let it do the logic
                // of making an offer.
                MWGui::TradeWindow* tradeWindow = static_cast<MWGui::TradeWindow*>(
                    MWBase::Environment::get().getWindowManager()->getGuiModeWindows(mGuiMode).at(1));
                tradeWindow->onControllerButtonEvent(arg);
            }
        }
        else
        {
            mItemView->onControllerButton(arg.button);
        }

        return true;
    }

    void InventoryWindow::setActiveControllerWindow(bool active)
    {
        if (!Settings::gui().mControllerMenus)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            MyGUI::IntCoord coord = getFixedWindowCoord(viewSize);
            int x = coord.left;
            int y = coord.top;
            int width = coord.width;
            int height = coord.height;

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(x, active ? y : viewSize.height + 1, width, height);

            adjustPanes();
            updatePreviewSize();
        }

        if (mFilterEdit && active && winMgr->getMode() != MWGui::GM_Barter)
            clearFilter();
        winMgr->setCursorVisible(false);
        if (!active)
            mControllerTabsActive = false;
        if (active && Settings::gui().mControllerMenus && !mControllerTabWidgets.empty()
            && mItemView->getItemCount() <= 0)
        {
            setControllerTabsActive(true);
            mItemView->setActiveControllerWindow(false);
        }
        mItemView->setActiveControllerWindow(active && !mControllerTabsActive);
        if (active && !mControllerTabsActive)
            mItemView->refreshControllerFocus();
        winMgr->setCursorVisible(false);
        if (active)
        {
            adjustPanes();
            if (mControllerTabsActive)
                updateControllerTabFocus(-1, mControllerTabIndex);
        }
        WindowBase::setActiveControllerWindow(active);
    }

    MyGUI::IntCoord InventoryWindow::getFixedWindowCoord(const MyGUI::IntSize& viewSize) const
    {
        const float scale = 0.85f;
        const float targetWidth = viewSize.width * scale;
        const float targetHeightFromWidth = targetWidth * 10.f / 16.f;
        const float maxHeight = viewSize.height * scale;

        int width = static_cast<int>(targetWidth);
        int height = static_cast<int>(targetHeightFromWidth);

        if (height > maxHeight)
        {
            height = static_cast<int>(maxHeight);
            width = static_cast<int>(height * 16.f / 10.f);
        }

        const int x = (viewSize.width - width) / 2;
        const int y = (viewSize.height - height) / 2;
        return MyGUI::IntCoord(x, y, width, height);
    }
}
