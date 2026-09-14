#ifndef OPENMW_APPS_OPENMW_MWGUI_ITEMTRANSFER_H
#define OPENMW_APPS_OPENMW_MWGUI_ITEMTRANSFER_H

#include "companionwindow.hpp"
#include "container.hpp"
#include "inventorywindow.hpp"
#include "itemmodel.hpp"
#include "itemview.hpp"
#include "tradewindow.hpp"
#include "windowmanagerimp.hpp"
#include "worlditemmodel.hpp"

#include <apps/openmw/mwbase/windowmanager.hpp>
#include <apps/openmw/mwworld/class.hpp>

#include <components/misc/notnullptr.hpp>

#include <unordered_set>

namespace MWGui
{
    class ItemTransfer
    {
    public:
        explicit ItemTransfer(WindowManager& windowManager)
            : mWindowManager(&windowManager)
        {
        }

        void addTarget(ItemView& view) { mTargets.insert(&view); }

        void removeTarget(ItemView& view) { mTargets.erase(&view); }

        void apply(const ItemStack& item, std::size_t count, ItemView& sourceView)
        {
            if (item.mFlags & ItemStack::Flag_Bound)
            {
                mWindowManager->messageBox("#{sBarterDialog12}");
                return;
            }

            ItemView* targetView = nullptr;

            for (ItemView* const view : mTargets)
            {
                if (view == &sourceView)
                    continue;
                if (!view->getInheritedVisible())
                    continue;

                if (targetView != nullptr)
                {
                    mWindowManager->messageBox("#{sContentsMessage2}");
                    return;
                }

                targetView = view;
            }

            if (targetView == nullptr)
            {
                const MWGui::GuiMode mode = mWindowManager->getMode();
                if (mode == MWGui::GM_Container || mode == MWGui::GM_Companion || mode == MWGui::GM_Barter)
                    targetView = findModeTarget(sourceView, mode);
            }

            WorldItemModel worldItemModel(0.5f, 0.5f);
            ItemModel* const targetModel = targetView == nullptr ? &worldItemModel : targetView->getModel();

            if (!targetModel->onDropItem(item.mBase, static_cast<int>(count)))
                return;

            sourceView.getModel()->moveItem(item, count, targetModel);

            if (targetView != nullptr)
                targetView->update();

            sourceView.update();

            mWindowManager->getInventoryWindow()->updateItemView();
            mWindowManager->playSound(item.mBase.getClass().getDownSoundId(item.mBase));
        }

    private:
        Misc::NotNullPtr<WindowManager> mWindowManager;
        std::unordered_set<ItemView*> mTargets;

        ItemView* findModeTarget(ItemView& sourceView, MWGui::GuiMode mode) const
        {
            ItemView* targetView = nullptr;
            for (WindowBase* window : mWindowManager->getGuiModeWindows(mode))
            {
                ItemView* view = nullptr;
                if (auto* container = dynamic_cast<ContainerWindow*>(window))
                    view = container->getItemView();
                else if (auto* companion = dynamic_cast<CompanionWindow*>(window))
                    view = companion->getItemView();
                else if (auto* inventory = dynamic_cast<InventoryWindow*>(window))
                    view = inventory->getItemView();
                else if (auto* trade = dynamic_cast<TradeWindow*>(window))
                    view = trade->getItemView();

                if (!view || view == &sourceView)
                    continue;

                if (targetView != nullptr && targetView != view)
                    return nullptr;

                targetView = view;
            }

            return targetView;
        }
    };
}

#endif
