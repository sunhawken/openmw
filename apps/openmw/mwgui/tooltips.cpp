#include "tooltips.hpp"

#include "mapwindow.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <iomanip>
#include <sstream>

#include <MyGUI_Button.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_TextIterator.h>
#include <MyGUI_UString.h>
#include <MyGUI_Window.h>

#include <components/esm/records.hpp>
#include <components/l10n/manager.hpp>
#include <components/lua_ui/util.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/myguiplatform/scalinglayer.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/widgets/box.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/magiceffects.hpp"
#include "../mwmechanics/spellutil.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include "alchemywindow.hpp"
#include "birth.hpp"
#include "class.hpp"
#include "companionwindow.hpp"
#include "confirmationdialog.hpp"
#include "container.hpp"
#include "countdialog.hpp"
#include "dialogue.hpp"
#include "enchantingdialog.hpp"
#include "inventorywindow.hpp"
#include "itemselection.hpp"
#include "itemview.hpp"
#include "levelupdialog.hpp"
#include "mapwindow.hpp"
#include "merchantrepair.hpp"
#include "quickkeysmenu.hpp"
#include "recharge.hpp"
#include "repair.hpp"
#include "review.hpp"
#include "spellbuyingwindow.hpp"
#include "spellcreationdialog.hpp"
#include "spellview.hpp"
#include "spellwindow.hpp"
#include "statswindow.hpp"
#include "tradewindow.hpp"
#include "trainingwindow.hpp"
#include "virtualkeyboard.hpp"

#include "itemmodel.hpp"

namespace MWGui
{
    namespace
    {
        struct WeaponDamageInfo
        {
            int mMin = 0;
            int mMax = 0;
            const char* mLabel = nullptr;
        };

        bool isTooltipDisabled(MyGUI::Widget* widget)
        {
            for (MyGUI::Widget* current = widget; current; current = current->getParent())
            {
                if (current->isUserString("DisableTooltips") && current->getUserString("DisableTooltips") == "true")
                {
                    return true;
                }
            }
            return false;
        }

        bool isSettingsTooltipPipeline(MyGUI::Widget* widget)
        {
            for (MyGUI::Widget* current = widget; current; current = current->getParent())
            {
                if (current->isUserString("SettingsTooltipPipeline")
                    && current->getUserString("SettingsTooltipPipeline") == "true")
                {
                    return true;
                }
            }
            return false;
        }

        WeaponDamageInfo getWeaponDamageInfo(const MWWorld::Ptr& ptr)
        {
            if (ptr.getType() != ESM::Weapon::sRecordId)
                return {};

            const ESM::Weapon* weapon = ptr.get<ESM::Weapon>()->mBase;

            const int chopMin = weapon->mData.mChop[0];
            const int chopMax = weapon->mData.mChop[1];
            const int slashMin = weapon->mData.mSlash[0];
            const int slashMax = weapon->mData.mSlash[1];
            const int thrustMin = weapon->mData.mThrust[0];
            const int thrustMax = weapon->mData.mThrust[1];

            WeaponDamageInfo best{ chopMin, chopMax, "Chop" };
            int bestSum = chopMin + chopMax;

            if (slashMin + slashMax > bestSum)
            {
                best = { slashMin, slashMax, "Slash" };
                bestSum = slashMin + slashMax;
            }
            if (thrustMin + thrustMax > bestSum)
                best = { thrustMin, thrustMax, "Thrust" };

            return best;
        }
    }

    ToolTips::ToolTips()
        : Layout("openmw_tooltips.layout")
        , mFocusToolTipX(0.0)
        , mFocusToolTipY(0.0)
        , mHorizontalScrollIndex(0)
        , mRemainingDelay(Settings::gui().mTooltipDelay)
        , mLastMouseX(0)
        , mLastMouseY(0)
        , mEnabled(true)
        , mFullHelp(false)
        , mFrameDuration(0.f)
    {
        getWidget(mDynamicToolTipBox, "DynamicToolTipBox");

        mDynamicToolTipBox->setVisible(false);

        // turn off mouse focus so that getMouseFocusWidget returns the correct widget,
        // even if the mouse is over the tooltip
        mDynamicToolTipBox->setNeedMouseFocus(false);
        mMainWidget->setNeedMouseFocus(false);

        for (size_t i = 0; i < mMainWidget->getChildCount(); ++i)
        {
            mMainWidget->getChildAt(i)->setVisible(false);
        }
    }

    void ToolTips::setEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    void ToolTips::onFrame(float frameDuration)
    {
        mFrameDuration = frameDuration;
    }

    void ToolTips::update(float frameDuration)
    {

        while (mDynamicToolTipBox->getChildCount())
        {
            MyGUI::Gui::getInstance().destroyWidget(mDynamicToolTipBox->getChildAt(0));
        }

        // start by hiding everything
        for (size_t i = 0; i < mMainWidget->getChildCount(); ++i)
        {
            mMainWidget->getChildAt(i)->setVisible(false);
        }

        auto ensureTooltipLayer = [&](std::string_view layerName) {
            MyGUI::ILayer* currentLayer = mMainWidget->getLayer();
            if (!currentLayer || currentLayer->getName() != layerName)
                mMainWidget->detachFromWidget(layerName);
            mMainWidget->setWidgetStyle(MyGUI::WidgetStyle::Popup, layerName);
        };

        ensureTooltipLayer("Popup");

        const MyGUI::IntSize& viewSize = MyGUI::RenderManager::getInstance().getViewSize();

        if (!mEnabled)
        {
            return;
        }

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const GuiMode mode = winMgr->getMode();
        bool guiMode = winMgr->isGuiMode();
        const bool luaUiWindowsVisible = LuaUi::isAnyElementVisibleOnLayer("Windows");

        CountDialog* countDialog = winMgr->getCountDialog();
        const bool countDialogVisible = countDialog && countDialog->isVisible();
        ConfirmationDialog* confirmDialog = winMgr->getConfirmationDialog();
        const bool confirmDialogVisible = confirmDialog && confirmDialog->isVisible();
        WindowBase* activeControllerWindow
            = Settings::gui().mControllerMenus ? winMgr->getActiveControllerWindow() : nullptr;
        const bool controllerAlchemy = Settings::gui().mControllerMenus && mode == GM_Alchemy;
        const bool controllerEnchanting = Settings::gui().mControllerMenus && mode == GM_Enchanting;
        const bool controllerSpellCreation = Settings::gui().mControllerMenus && mode == GM_SpellCreation;
        const bool controllerSpellBuying = Settings::gui().mControllerMenus && mode == GM_SpellBuying;
        const bool controllerQuickKeys = Settings::gui().mControllerMenus && mode == GM_QuickKeysMenu;
        const bool controllerInventory = Settings::gui().mControllerMenus
            && (mode == GM_Inventory || mode == GM_Container || mode == GM_Barter || mode == GM_Companion
                || mode == GM_Levelup || mode == GM_Training || mode == GM_Repair || mode == GM_MerchantRepair
                || controllerAlchemy || controllerEnchanting || controllerSpellCreation || controllerSpellBuying
                || controllerQuickKeys);
        const bool controllerRecharge = Settings::gui().mControllerMenus && mode == GM_Recharge;
        const bool controllerCharGen = Settings::gui().mControllerMenus
            && (mode == GM_Name || mode == GM_Race || mode == GM_Birth || mode == GM_Class || mode == GM_ClassGenerate
                || mode == GM_ClassPick || mode == GM_ClassCreate || mode == GM_Review);
        const bool controllerClassCreate = Settings::gui().mControllerMenus && mode == GM_ClassCreate;
        int focusItemCount = -1;
        if (Settings::gui().mControllerMenus && countDialogVisible && controllerInventory)
        {
            WindowBase* tooltipWindow = nullptr;
            if (countDialog && countDialog->getTooltipSourceWindow()
                && countDialog->getTooltipSourceWindow()->isVisible())
                tooltipWindow = countDialog->getTooltipSourceWindow();
            if (!tooltipWindow)
            {
                if (mode == GM_Inventory)
                    tooltipWindow = winMgr->getInventoryWindow();
                else if (mode == GM_Barter)
                    tooltipWindow = winMgr->getTradeWindow();
            }
            if (!tooltipWindow)
            {
                for (WindowBase* window : winMgr->getGuiModeWindows(mode))
                {
                    if (!window || !window->isVisible())
                        continue;
                    if (dynamic_cast<InventoryWindow*>(window) || dynamic_cast<TradeWindow*>(window)
                        || dynamic_cast<ContainerWindow*>(window) || dynamic_cast<CompanionWindow*>(window))
                    {
                        tooltipWindow = window;
                        break;
                    }
                }
            }
            if (tooltipWindow)
                activeControllerWindow = tooltipWindow;
        }
        if (Settings::gui().mControllerMenus && confirmDialogVisible && controllerInventory)
        {
            WindowBase* tooltipWindow = confirmDialog->getTooltipSourceWindow();
            if (tooltipWindow && tooltipWindow->isVisible())
                activeControllerWindow = tooltipWindow;
        }
        if (Settings::gui().mControllerMenus && winMgr->isVirtualKeyboardVisible() && controllerInventory)
        {
            if (auto* keyboard = dynamic_cast<VirtualKeyboard*>(winMgr->getActiveControllerWindow()))
            {
                WindowBase* tooltipWindow = keyboard->getTooltipSourceWindow();
                if (tooltipWindow && tooltipWindow->isVisible())
                    activeControllerWindow = tooltipWindow;
            }
        }
        if (Settings::gui().mControllerMenus && controllerInventory && winMgr->isInteractiveMessageBoxActive())
        {
            if (auto* invWindow = winMgr->getInventoryWindow())
                activeControllerWindow = invWindow;
        }
        const bool trainingMode = mode == GM_Training;
        const bool controllerMap = Settings::gui().mControllerMenus && mode == GM_Inventory
            && dynamic_cast<MapWindow*>(activeControllerWindow);
        const bool controllerCollapsed = controllerInventory && !winMgr->getControllerTooltipEnabled();
        const bool controllerForceCollapsed
            = controllerCollapsed || controllerMap || (countDialogVisible && controllerInventory);
        const bool controllerTooltipAnchor = controllerInventory || controllerCharGen || controllerRecharge;
        MyGUI::IntCoord inventoryTooltipRect;
        int inventoryTooltipWidth = 0;
        if (Settings::gui().mControllerMenus)
        {
            if (auto* invWindow = winMgr->getInventoryWindow())
            {
                inventoryTooltipRect = invWindow->mMainWidget->getAbsoluteCoord();
                inventoryTooltipWidth = inventoryTooltipRect.width;
            }
        }
        int controllerTooltipWidth = (controllerInventory || controllerMap)
            ? (activeControllerWindow ? activeControllerWindow->mMainWidget->getWidth() : viewSize.width)
            : 0;
        MyGUI::IntCoord controllerWinRect = (controllerInventory || controllerMap) && activeControllerWindow
            ? activeControllerWindow->mMainWidget->getAbsoluteCoord()
            : MyGUI::IntCoord(0, 0, viewSize.width, 32);
        if (Settings::gui().mControllerMenus
            && (winMgr->getMode() == GM_Repair || winMgr->getMode() == GM_MerchantRepair))
        {
            if (auto* invWindow = winMgr->getInventoryWindow())
            {
                controllerWinRect = invWindow->mMainWidget->getAbsoluteCoord();
                controllerTooltipWidth = controllerWinRect.width;
            }
        }
        if (Settings::gui().mControllerMenus && controllerClassCreate)
        {
            if (auto* invWindow = winMgr->getInventoryWindow())
            {
                controllerWinRect = invWindow->mMainWidget->getAbsoluteCoord();
                controllerTooltipWidth = controllerWinRect.width;
            }
        }
        if (controllerTooltipAnchor && !controllerInventory && !controllerClassCreate)
        {
            if (!activeControllerWindow)
            {
                int bestWidth = -1;
                for (WindowBase* window : winMgr->getGuiModeWindows(mode))
                {
                    if (!window || !window->isVisible())
                        continue;
                    const int width = window->mMainWidget->getWidth();
                    if (width > bestWidth)
                    {
                        bestWidth = width;
                        activeControllerWindow = window;
                    }
                }
            }
            if (activeControllerWindow)
            {
                controllerWinRect = activeControllerWindow->mMainWidget->getAbsoluteCoord();
                controllerTooltipWidth = controllerWinRect.width;
            }
        }
        if (trainingMode)
        {
            if (auto* invWindow = winMgr->getInventoryWindow())
            {
                controllerWinRect = invWindow->mMainWidget->getAbsoluteCoord();
                controllerTooltipWidth = controllerWinRect.width;
            }
            else
            {
                for (WindowBase* window : winMgr->getGuiModeWindows(GM_Training))
                {
                    if (auto* trainingWindow = dynamic_cast<TrainingWindow*>(window))
                    {
                        controllerWinRect = trainingWindow->mMainWidget->getAbsoluteCoord();
                        controllerTooltipWidth = controllerWinRect.width;
                        break;
                    }
                }
            }
        }
        if (controllerTooltipAnchor && inventoryTooltipWidth > 0)
        {
            const bool inventoryTabActive
                = controllerInventory && mode == GM_Inventory && dynamic_cast<InventoryWindow*>(activeControllerWindow);
            if (!controllerInventory || mode != GM_Inventory || inventoryTabActive)
            {
                controllerWinRect = inventoryTooltipRect;
                controllerTooltipWidth = inventoryTooltipWidth;
            }
        }

        // Keep the controller tooltip bar anchored to the map window while editing notes,
        // so it doesn't jump to the modal dialog's smaller size/position.
        if (Settings::gui().mControllerMenus && mode == GM_Inventory
            && dynamic_cast<EditNoteDialog*>(winMgr->getActiveControllerWindow()) != nullptr)
        {
            for (WindowBase* window : winMgr->getGuiModeWindows(GM_Inventory))
            {
                auto* mapWindow = dynamic_cast<MapWindow*>(window);
                if (!mapWindow || !mapWindow->isVisible())
                    continue;

                controllerWinRect = mapWindow->mMainWidget->getAbsoluteCoord();
                controllerTooltipWidth = controllerWinRect.width;
                break;
            }
        }
        const bool suppressControllerBar = [&]() {
            if (!Settings::gui().mControllerMenus)
                return false;
            if (winMgr->isConsoleMode())
                return true;
            switch (winMgr->getMode())
            {
                case GM_Book:
                case GM_Scroll:
                case GM_Journal:
                case GM_MainMenu:
                case GM_Rest:
                case GM_Review:
                case GM_Name:
                case GM_Race:
                case GM_Birth:
                case GM_Class:
                case GM_ClassPick:
                case GM_ClassGenerate:
                case GM_Travel:
                case GM_Jail:
                    return true;
                default:
                    break;
            }

            if (winMgr->isInteractiveMessageBoxActive())
                return !controllerInventory;
            if (confirmDialogVisible)
                return confirmDialog->getTooltipSourceWindow() == nullptr;
            return false;
        }();
        auto getOwningWindow = [](MyGUI::Widget* widget) -> MyGUI::Window* {
            while (widget != nullptr)
            {
                if (auto* window = widget->castType<MyGUI::Window>(false))
                    return window;
                widget = widget->getParent();
            }
            return nullptr;
        };
        auto toScreenRect = [](const MyGUI::Widget* widget, MyGUI::IntCoord rect) {
            if (!widget || !widget->getLayer())
                return rect;

            auto* scalingLayer = dynamic_cast<const MyGUIPlatform::ScalingLayer*>(widget->getLayer());
            if (!scalingLayer)
                return rect;

            const MyGUI::IntSize layerSize = widget->getLayer()->getSize();
            const MyGUI::IntSize renderViewSize = MyGUI::RenderManager::getInstance().getViewSize();
            const float scale = MyGUIPlatform::ScalingLayer::getScaleFactor(layerSize);

            rect.left = static_cast<int>(
                std::lround((rect.left - layerSize.width / 2.f) * scale + renderViewSize.width / 2.f));
            rect.top = static_cast<int>(
                std::lround((rect.top - layerSize.height / 2.f) * scale + renderViewSize.height / 2.f));
            rect.width = static_cast<int>(std::lround(rect.width * scale));
            rect.height = static_cast<int>(std::lround(rect.height * scale));
            return rect;
        };

        auto toInt = [](const MyGUI::Widget* widget, const std::string& key) {
            if (!widget || !widget->isUserString(key))
                return 0;
            try
            {
                return std::stoi(std::string(widget->getUserString(key)));
            }
            catch (...)
            {
                return 0;
            }
        };

        auto renderDialogueCollapsedBar = [&](DialogueWindow* dialogueWindow) -> bool {
            if (!dialogueWindow || !dialogueWindow->hasActor())
                return false;

            const std::string name = dialogueWindow->getActorName();
            if (name.empty())
                return false;

            const int paddingX = 6;
            const int rowHeight = 18;
            const int barHeight = 28;

            const MyGUI::IntCoord winRect
                = toScreenRect(dialogueWindow->mMainWidget, dialogueWindow->mMainWidget->getAbsoluteCoord());
            const int width = winRect.width;
            const int posY = std::max(0, winRect.top - barHeight - 4);
            const int y = std::max(0, (barHeight - rowHeight) / 2);

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box");
            setCoord(winRect.left, posY, width, barHeight);

            const int desiredBarWidth = static_cast<int>(dialogueWindow->getDispositionBarWidth() * 0.7f);
            const int barWidth = std::max(0, std::min(desiredBarWidth, width - paddingX * 2));
            const int barLeft = std::max(paddingX, width - paddingX - barWidth);
            const int labelWidth = std::max(0, barLeft - paddingX);

            MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                MyGUI::IntCoord(paddingX, y, labelWidth, rowHeight), MyGUI::Align::Left | MyGUI::Align::VCenter);
            label->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
            label->setCaption(name);

            MyGUI::ProgressBar* bar = mDynamicToolTipBox->createWidget<MyGUI::ProgressBar>("MW_Progress_Blue",
                MyGUI::IntCoord(barLeft, y, barWidth, rowHeight), MyGUI::Align::Right | MyGUI::Align::VCenter);
            bar->setProgressRange(100);
            const int disposition = std::clamp(dialogueWindow->getDisposition(), 0, 100);
            bar->setProgressPosition(disposition);

            MyGUI::TextBox* value = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("ProgressText",
                MyGUI::IntCoord(barLeft, y + 1, barWidth, rowHeight), MyGUI::Align::Right | MyGUI::Align::VCenter);
            value->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
            value->setCaption(MyGUI::utility::toString(disposition, "/100"));
            return true;
        };

        auto renderCollapsedLayoutTooltip
            = [&](bool collapsed, MyGUI::Widget* focus, const MyGUI::IntCoord& winRect, int tooltipWidth) -> bool {
            if (!collapsed || focus == nullptr || !focus->isUserString("ToolTipLayout"))
                return false;

            std::string layout = std::string(focus->getUserString("ToolTipLayout"));
            struct CollapsedRow
            {
                std::string mLabel;
                std::string mValue;
                bool mProgress = false;
                float mProgressScale = 1.f;
                int mProgressValue = 0;
                int mProgressRange = 0;
                std::string mProgressText;
                bool mCenter = false;
                bool mLabelOnly = false;
            };
            std::vector<CollapsedRow> rows;

            auto addLabelValue = [&](std::string label, std::string value) {
                CollapsedRow row;
                row.mLabel = std::move(label);
                row.mValue = std::move(value);
                rows.push_back(std::move(row));
            };

            if (layout == "HealthToolTip")
            {
                addLabelValue(std::string(focus->getUserString("CollapsedLabel")),
                    std::string(focus->getUserString("CollapsedValue")));
            }
            else if (layout == "LevelToolTip")
            {
                CollapsedRow row;
                row.mLabel = std::string(focus->getUserString("CollapsedLabel"));
                row.mValue = std::string(focus->getUserString("CollapsedValue"));
                row.mProgress = true;
                row.mProgressScale = 0.4f;
                row.mProgressRange = toInt(focus, "Range_LevelProgress");
                row.mProgressValue = toInt(focus, "RangePosition_LevelProgress");
                row.mProgressText = std::string(focus->getUserString("Caption_LevelProgressText"));
                rows.push_back(std::move(row));
            }
            else if (layout == "RaceToolTip" || layout == "ClassToolTip")
            {
                addLabelValue(std::string(focus->getUserString("CollapsedLabel")),
                    std::string(focus->getUserString("CollapsedValue")));
            }
            else if (layout == "SpecializationToolTip")
            {
                CollapsedRow row;
                if (focus->isUserString("CollapsedValue"))
                    row.mLabel = std::string(focus->getUserString("CollapsedValue"));
                if (row.mLabel.empty() && focus->isUserString("CollapsedLabel"))
                    row.mLabel = std::string(focus->getUserString("CollapsedLabel"));
                if (row.mLabel.empty() && focus->isUserString("Caption_Caption"))
                    row.mLabel = std::string(focus->getUserString("Caption_Caption"));
                if (row.mLabel.empty())
                {
                    if (auto* text = focus->castType<MyGUI::TextBox>(false))
                        row.mLabel = text->getCaption();
                    else if (auto* button = focus->castType<MyGUI::Button>(false))
                        row.mLabel = button->getCaption();
                }
                row.mLabelOnly = true;
                rows.push_back(std::move(row));
            }
            else if (layout == "AttributeToolTip" || layout == "SkillToolTip" || layout == "SkillNoProgressToolTip")
            {
                addLabelValue(std::string(focus->getUserString("CollapsedLabel")),
                    std::string(focus->getUserString("CollapsedValue")));
            }
            else if (layout == "TextToolTipOneLine")
            {
                if (focus->isUserString("CollapsedLabel"))
                {
                    addLabelValue(std::string(focus->getUserString("CollapsedLabel")),
                        std::string(focus->getUserString("CollapsedValue")));
                }
                else
                {
                    CollapsedRow row;
                    row.mLabel = std::string(focus->getUserString("Caption_TextOneLine"));
                    row.mCenter = true;
                    rows.push_back(std::move(row));
                }
            }
            else if (layout == "TextToolTip" || layout == "BirthSignToolTip")
            {
                addLabelValue(std::string(focus->getUserString("CollapsedLabel")),
                    std::string(focus->getUserString("CollapsedValue")));
            }
            else if (layout == "FactionToolTip")
            {
                CollapsedRow row;
                row.mLabel = std::string(focus->getUserString("CollapsedLabel"));
                row.mLabelOnly = true;
                rows.push_back(std::move(row));
            }
            else if (layout == "MagicEffectToolTip")
            {
                const int paddingX = 6;
                const int rowHeight = 18;
                const int barHeight = 28;
                const int iconSize = 16;
                const int iconTop = (barHeight - iconSize) / 2;
                const int spacing = 4;
                const int width = tooltipWidth > 0 ? tooltipWidth : winRect.width;

                std::string label = std::string(focus->getUserString("Caption_MagicEffectName"));
                label = MyGUI::LanguageManager::getInstance().replaceTags(label);

                mDynamicToolTipBox->setVisible(true);
                mDynamicToolTipBox->changeWidgetSkin(collapsed ? "HUD_Box" : "HUD_Box_NoTransp");

                int textLeft = paddingX;
                if (focus->isUserString("ImageTexture_MagicEffectImage"))
                {
                    const std::string iconTexture = std::string(focus->getUserString("ImageTexture_MagicEffectImage"));
                    if (!iconTexture.empty())
                    {
                        const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
                        const VFS::Path::Normalized iconPath
                            = Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(iconTexture), vfs);
                        MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                            MyGUI::IntCoord(paddingX, iconTop, iconSize, iconSize),
                            MyGUI::Align::Left | MyGUI::Align::VCenter);
                        icon->setImageTexture(iconPath);
                        textLeft = paddingX + iconSize + spacing;
                    }
                }

                MyGUI::TextBox* labelWidget = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(textLeft, (barHeight - rowHeight) / 2, width - textLeft - paddingX, rowHeight),
                    MyGUI::Align::Left | MyGUI::Align::VCenter);
                labelWidget->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                labelWidget->setCaption(label);

                const int posY = std::max(0, winRect.top - barHeight - 4);
                setCoord(winRect.left, posY, width, barHeight);
                return true;
            }
            else
                return false;

            for (CollapsedRow& row : rows)
            {
                if (row.mLabel.empty())
                    row.mLabel = layout;
                row.mLabel = MyGUI::LanguageManager::getInstance().replaceTags(row.mLabel);
                row.mValue = MyGUI::LanguageManager::getInstance().replaceTags(row.mValue);
            }

            const int paddingX = 6;
            const int rowHeight = 18;
            const int spacing = 4;
            const int barHeight = 28;
            const int width = tooltipWidth > 0 ? tooltipWidth : winRect.width;
            const int totalHeight
                = static_cast<int>(rows.size()) * rowHeight + std::max<int>(0, int(rows.size()) - 1) * spacing;
            int y = std::max(0, (barHeight - totalHeight) / 2);

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin(collapsed ? "HUD_Box" : "HUD_Box_NoTransp");

            for (const CollapsedRow& row : rows)
            {
                if (row.mCenter)
                {
                    MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(paddingX, y, width - paddingX * 2, rowHeight),
                        MyGUI::Align::Center | MyGUI::Align::VCenter);
                    label->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                    label->setCaption(row.mLabel);
                }
                else if (row.mLabelOnly)
                {
                    MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(paddingX, y, width - paddingX * 2, rowHeight),
                        MyGUI::Align::Left | MyGUI::Align::VCenter);
                    label->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    label->setCaption(row.mLabel);
                }
                else
                {
                    const int labelWidth = width / 2 - paddingX * 2;
                    const int valueWidth = width - labelWidth - paddingX * 3;

                    MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(paddingX, y, labelWidth, rowHeight),
                        MyGUI::Align::Left | MyGUI::Align::VCenter);
                    label->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    label->setCaption(row.mLabel);

                    if (row.mProgress)
                    {
                        const int barWidth = std::max(0, static_cast<int>(valueWidth * row.mProgressScale));
                        const int barLeft = paddingX * 2 + labelWidth + (valueWidth - barWidth);
                        MyGUI::ProgressBar* bar = mDynamicToolTipBox->createWidget<MyGUI::ProgressBar>(
                            "MW_Progress_Red", MyGUI::IntCoord(barLeft, y, barWidth, rowHeight),
                            MyGUI::Align::Right | MyGUI::Align::VCenter);
                        const int range = std::max(1, row.mProgressRange);
                        const int pos = std::clamp(row.mProgressValue, 0, range);
                        bar->setProgressRange(range);
                        bar->setProgressPosition(pos);
                        const int textHeight = 16;
                        const int textTop = std::max(0, (rowHeight - textHeight) / 2 - 1);
                        MyGUI::TextBox* text = bar->createWidget<MyGUI::TextBox>(
                            "ProgressText", MyGUI::IntCoord(0, textTop, barWidth, textHeight), MyGUI::Align::Stretch);
                        text->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                        text->setCaption(row.mProgressText);
                    }
                    else
                    {
                        MyGUI::TextBox* value = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                            MyGUI::IntCoord(paddingX * 2 + labelWidth, y, valueWidth, rowHeight),
                            MyGUI::Align::Right | MyGUI::Align::VCenter);
                        value->setTextAlign(MyGUI::Align::Right | MyGUI::Align::VCenter);
                        value->setCaption(row.mValue);
                    }
                }

                y += rowHeight + spacing;
            }

            const int height = barHeight;
            const int posY = std::max(0, winRect.top - height - 4);
            setCoord(winRect.left, posY, width, height);
            return true;
        };
        auto renderStatsLayoutTooltip
            = [&](MyGUI::Widget* focus, const MyGUI::IntCoord& winRect, int tooltipWidth) -> bool {
            if (!focus || !focus->isUserString("ToolTipDynamic") || focus->getUserString("ToolTipDynamic") != "Stats")
                return false;
            if (!focus->isUserString("ToolTipLayout"))
                return false;

            const std::string layout = std::string(focus->getUserString("ToolTipLayout"));
            if (layout != "SkillToolTip" && layout != "SkillNoProgressToolTip" && layout != "AttributeToolTip"
                && layout != "HealthToolTip" && layout != "LevelToolTip" && layout != "BirthSignToolTip")
                return false;

            int padding = 8;
            int spacing = 8;
            int iconSize = 32;
            const int lineHeight = 18;
            const int barHeight = 20;
            int width = tooltipWidth > 0 ? tooltipWidth : 300;
            int y = 0;

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box_NoTransp");

            auto getText = [&](std::string_view key) -> std::string {
                if (!focus->isUserString(std::string(key)))
                    return std::string();
                return MyGUI::LanguageManager::getInstance().replaceTags(
                    std::string(focus->getUserString(std::string(key))));
            };

            std::string iconPath;
            std::string nameText;
            std::string attributeText;
            std::string descriptionText;
            if (layout == "AttributeToolTip")
            {
                y = padding;
                iconPath = focus->isUserString("ImageTexture_AttributeImage")
                    ? std::string(focus->getUserString("ImageTexture_AttributeImage"))
                    : std::string();
                nameText = getText("Caption_AttributeName");
                descriptionText = getText("Caption_AttributeDescription");

                int nameLeft = padding;
                if (!iconPath.empty())
                {
                    MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                        MyGUI::IntCoord(padding, y, iconSize, iconSize), MyGUI::Align::Left | MyGUI::Align::Top);
                    icon->setImageTexture(Misc::ResourceHelpers::correctIconPath(
                        VFS::Path::toNormalized(iconPath), *MWBase::Environment::get().getResourceSystem()->getVFS()));
                    nameLeft = padding + iconSize + spacing;
                }

                const int textWidth = std::max(0, width - nameLeft - padding);
                if (!nameText.empty())
                {
                    MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(nameLeft, y, textWidth, lineHeight), MyGUI::Align::Left | MyGUI::Align::Top);
                    name->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    name->setCaption(nameText);
                }

                int descHeight = 0;
                if (!descriptionText.empty())
                {
                    auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                        MyGUI::IntCoord(nameLeft, y + lineHeight, textWidth, 0),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setEditStatic(true);
                    desc->setNeedKeyFocus(false);
                    desc->setEditMultiLine(true);
                    desc->setEditWordWrap(true);
                    desc->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setCaptionWithReplacing(descriptionText);
                    desc->forceTextUpdate();
                    const MyGUI::IntSize descSize = desc->getRequestedSize();
                    desc->setSize(MyGUI::IntSize(textWidth, descSize.height));
                    descHeight = descSize.height;
                }

                const int topBlockHeight = std::max(iconSize, lineHeight + descHeight);
                y += topBlockHeight + spacing;
            }
            else if (layout == "HealthToolTip")
            {
                padding = 14;
                spacing = 8;
                iconSize = 32;
                y = padding;

                iconPath = focus->isUserString("ImageTexture_HealthImage")
                    ? std::string(focus->getUserString("ImageTexture_HealthImage"))
                    : std::string();
                descriptionText = getText("Caption_HealthDescription");

                int textLeft = padding;
                if (!iconPath.empty())
                {
                    MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                        MyGUI::IntCoord(padding, y, iconSize, iconSize), MyGUI::Align::Left | MyGUI::Align::Top);
                    icon->setImageTexture(Misc::ResourceHelpers::correctIconPath(
                        VFS::Path::toNormalized(iconPath), *MWBase::Environment::get().getResourceSystem()->getVFS()));
                    textLeft = padding + iconSize + spacing;
                }

                const int textWidth = std::max(0, width - textLeft - padding);
                if (!descriptionText.empty())
                {
                    auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>(
                        "SandText", MyGUI::IntCoord(textLeft, y, textWidth, 0), MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setEditStatic(true);
                    desc->setNeedKeyFocus(false);
                    desc->setEditMultiLine(true);
                    desc->setEditWordWrap(true);
                    desc->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setCaptionWithReplacing(descriptionText);
                    desc->forceTextUpdate();
                    const MyGUI::IntSize descSize = desc->getRequestedSize();
                    desc->setSize(MyGUI::IntSize(textWidth, descSize.height));
                    y += std::max(iconSize, descSize.height);
                }
                else
                {
                    y += iconSize;
                }
            }
            else if (layout == "LevelToolTip")
            {
                padding = 8;
                spacing = 6;
                y = padding;

                const std::string progressLabel
                    = MyGUI::LanguageManager::getInstance().replaceTags("#{sLevelProgress}");
                MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), lineHeight),
                    MyGUI::Align::Center | MyGUI::Align::Top);
                label->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                label->setCaption(progressLabel);
                y += lineHeight + spacing;

                const int barWidth = std::min(200, std::max(0, width - padding * 2));
                const int barLeft = (width - barWidth) / 2;
                MyGUI::ProgressBar* bar = mDynamicToolTipBox->createWidget<MyGUI::ProgressBar>("MW_Progress_Red",
                    MyGUI::IntCoord(barLeft, y, barWidth, barHeight), MyGUI::Align::Center | MyGUI::Align::Top);
                const int range = std::max(1, toInt(focus, "Range_LevelProgress"));
                const int pos = std::clamp(toInt(focus, "RangePosition_LevelProgress"), 0, range);
                bar->setProgressRange(range);
                bar->setProgressPosition(pos);

                const std::string progressText = getText("Caption_LevelProgressText");
                const int textHeight = 16;
                const int textTop = std::max(0, (barHeight - textHeight) / 2 - 1);
                MyGUI::TextBox* text = bar->createWidget<MyGUI::TextBox>(
                    "ProgressText", MyGUI::IntCoord(0, textTop, barWidth, textHeight), MyGUI::Align::Stretch);
                text->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                text->setCaption(progressText);
                y += barHeight + spacing;

                const std::string detailText = getText("Caption_LevelDetailText");
                if (!detailText.empty())
                {
                    auto* detail = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), 0),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    detail->setEditStatic(true);
                    detail->setNeedKeyFocus(false);
                    detail->setEditMultiLine(true);
                    detail->setEditWordWrap(true);
                    detail->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Top);
                    detail->setCaptionWithReplacing(detailText);
                    detail->forceTextUpdate();
                    const MyGUI::IntSize detailSize = detail->getRequestedSize();
                    detail->setSize(MyGUI::IntSize(std::max(0, width - padding * 2), detailSize.height));
                    y += detailSize.height;
                }
            }
            else if (layout == "BirthSignToolTip")
            {
                padding = 10;
                spacing = 8;
                width = tooltipWidth > 0 ? tooltipWidth : (Settings::gui().mControllerMenus ? winRect.width : 300);
                y = padding;

                const std::string imageTexture = focus->isUserString("ImageTexture_BirthSignImage")
                    ? std::string(focus->getUserString("ImageTexture_BirthSignImage"))
                    : std::string();
                const std::string signName = getText("Caption_BirthSignName");
                const std::string signDescription = getText("Caption_BirthSignDescription");
                const std::string signAbilities = getText("Caption_BirthSignAbilities");
                const std::string signPowers = getText("Caption_BirthSignPowers");
                const std::string signSpells = getText("Caption_BirthSignSpells");

                const int maxImageWidth = std::max(0, width - padding * 2);
                const int imageBoxWidth = std::min(263, maxImageWidth);
                const int imageBoxHeight = imageBoxWidth > 0 ? imageBoxWidth * 137 / 263 : 0;
                const int imageLeft = (width - imageBoxWidth) / 2;

                if (imageBoxWidth > 0 && imageBoxHeight > 0)
                {
                    MyGUI::Widget* imageBox = mDynamicToolTipBox->createWidget<MyGUI::Widget>("MW_Box",
                        MyGUI::IntCoord(imageLeft, y, imageBoxWidth, imageBoxHeight),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    if (!imageTexture.empty())
                    {
                        MyGUI::ImageBox* image = imageBox->createWidget<MyGUI::ImageBox>("ImageBox",
                            MyGUI::IntCoord(2, 2, std::max(0, imageBoxWidth - 4), std::max(0, imageBoxHeight - 4)),
                            MyGUI::Align::Left | MyGUI::Align::Top);
                        image->setImageTexture(imageTexture);
                    }
                    y += imageBoxHeight + spacing;
                }

                if (!signName.empty())
                {
                    MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), lineHeight),
                        MyGUI::Align::Center | MyGUI::Align::Top);
                    name->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                    name->setCaption(signName);
                    y += lineHeight + spacing;
                }

                if (!signDescription.empty())
                {
                    auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), 0),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setEditStatic(true);
                    desc->setNeedKeyFocus(false);
                    desc->setEditMultiLine(true);
                    desc->setEditWordWrap(true);
                    desc->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Top);
                    desc->setCaptionWithReplacing(signDescription);
                    desc->forceTextUpdate();
                    const MyGUI::IntSize descSize = desc->getRequestedSize();
                    desc->setSize(MyGUI::IntSize(std::max(0, width - padding * 2), descSize.height));
                    y += descSize.height + spacing;
                }

                auto addCategory = [&](const std::string& text) {
                    if (text.empty())
                        return;
                    auto* line = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), 0),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    line->setEditStatic(true);
                    line->setNeedKeyFocus(false);
                    line->setEditMultiLine(true);
                    line->setEditWordWrap(true);
                    line->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Top);
                    line->setCaptionWithReplacing(text);
                    line->forceTextUpdate();
                    const MyGUI::IntSize lineSize = line->getRequestedSize();
                    line->setSize(MyGUI::IntSize(std::max(0, width - padding * 2), lineSize.height));
                    y += lineSize.height + spacing;
                };

                addCategory(signAbilities);
                addCategory(signPowers);
                addCategory(signSpells);
            }
            else
            {
                y = padding;
                iconPath = focus->isUserString("ImageTexture_SkillImage")
                    ? std::string(focus->getUserString("ImageTexture_SkillImage"))
                    : (focus->isUserString("ImageTexture_SkillNoProgressImage")
                            ? std::string(focus->getUserString("ImageTexture_SkillNoProgressImage"))
                            : std::string());
                nameText
                    = getText(layout == "SkillNoProgressToolTip" ? "Caption_SkillNoProgressName" : "Caption_SkillName");
                descriptionText = getText(layout == "SkillNoProgressToolTip" ? "Caption_SkillNoProgressDescription"
                                                                             : "Caption_SkillDescription");
                attributeText = getText(
                    layout == "SkillNoProgressToolTip" ? "Caption_SkillNoProgressAttribute" : "Caption_SkillAttribute");
                int nameLeft = padding;
                if (!iconPath.empty())
                {
                    MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                        MyGUI::IntCoord(padding, y, iconSize, iconSize), MyGUI::Align::Left | MyGUI::Align::Top);
                    icon->setImageTexture(Misc::ResourceHelpers::correctIconPath(
                        VFS::Path::toNormalized(iconPath), *MWBase::Environment::get().getResourceSystem()->getVFS()));
                    nameLeft = padding + iconSize + spacing;
                }

                const int textWidth = std::max(0, width - nameLeft - padding);
                if (!nameText.empty())
                {
                    MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(nameLeft, y, textWidth, lineHeight), MyGUI::Align::Left | MyGUI::Align::Top);
                    name->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    name->setCaption(nameText);
                }

                if (!attributeText.empty())
                {
                    MyGUI::TextBox* attr = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("SandText",
                        MyGUI::IntCoord(nameLeft, y + lineHeight, textWidth, lineHeight),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    attr->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    attr->setCaption(attributeText);
                }

                const int topBlockHeight = std::max(iconSize, attributeText.empty() ? lineHeight : lineHeight * 2);
                y += topBlockHeight + spacing;

                if (!descriptionText.empty())
                {
                    auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), 0),
                        MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setEditStatic(true);
                    desc->setNeedKeyFocus(false);
                    desc->setEditMultiLine(true);
                    desc->setEditWordWrap(true);
                    desc->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                    desc->setCaptionWithReplacing(descriptionText);
                    desc->forceTextUpdate();
                    const MyGUI::IntSize descSize = desc->getRequestedSize();
                    desc->setSize(MyGUI::IntSize(std::max(0, width - padding * 2), descSize.height));
                    y += descSize.height;
                }
            }

            if (layout == "SkillToolTip")
            {
                const std::string showProgress = focus->isUserString("Visible_SkillProgressVBox")
                    ? std::string(focus->getUserString("Visible_SkillProgressVBox"))
                    : std::string();
                const std::string showMaxed = focus->isUserString("Visible_SkillMaxed")
                    ? std::string(focus->getUserString("Visible_SkillMaxed"))
                    : std::string();

                y += spacing;
                if (showMaxed == "true")
                {
                    const std::string maxedText
                        = MyGUI::LanguageManager::getInstance().replaceTags("#{sSkillMaxReached}");
                    MyGUI::TextBox* maxed = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("SandText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), lineHeight),
                        MyGUI::Align::Center | MyGUI::Align::Top);
                    maxed->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                    maxed->setCaption(maxedText);
                    y += lineHeight;
                }
                else if (showProgress == "true")
                {
                    const std::string progressLabel
                        = MyGUI::LanguageManager::getInstance().replaceTags("#{sSkillProgress}");
                    MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), lineHeight),
                        MyGUI::Align::Center | MyGUI::Align::Top);
                    label->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                    label->setCaption(progressLabel);
                    y += lineHeight + 4;

                    const int barWidth = std::min(200, std::max(0, width - padding * 2));
                    const int barLeft = (width - barWidth) / 2;
                    MyGUI::ProgressBar* bar = mDynamicToolTipBox->createWidget<MyGUI::ProgressBar>("MW_Progress_Red",
                        MyGUI::IntCoord(barLeft, y, barWidth, barHeight), MyGUI::Align::Center | MyGUI::Align::Top);
                    const int range = std::max(1, toInt(focus, "Range_SkillProgress"));
                    const int pos = std::clamp(toInt(focus, "RangePosition_SkillProgress"), 0, range);
                    bar->setProgressRange(range);
                    bar->setProgressPosition(pos);

                    const std::string progressText = getText("Caption_SkillProgressText");
                    const int textHeight = 16;
                    const int textTop = std::max(0, (barHeight - textHeight) / 2 - 1);
                    MyGUI::TextBox* text = bar->createWidget<MyGUI::TextBox>(
                        "ProgressText", MyGUI::IntCoord(0, textTop, barWidth, textHeight), MyGUI::Align::Stretch);
                    text->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                    text->setCaption(progressText);

                    y += barHeight;
                }
            }

            const int height = y + padding;
            const MyGUI::IntSize statsSize(width, height);

            if (controllerTooltipAnchor || trainingMode)
            {
                const MyGUI::IntCoord anchorRect = trainingMode ? controllerWinRect : winRect;
                const int anchorHeight = 28;
                const int anchorPadding = 4;
                const int posY = std::max(0, anchorRect.top - anchorHeight - anchorPadding);
                setCoord(anchorRect.left, posY, statsSize.width, statsSize.height);
                return true;
            }

            MyGUI::IntPoint tooltipPosition = MyGUI::InputManager::getInstance().getMousePosition();
            if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
            {
                const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                tooltipPosition
                    = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
            }
            position(tooltipPosition, statsSize, viewSize);
            setCoord(tooltipPosition.left, tooltipPosition.top, statsSize.width, statsSize.height);
            return true;
        };
        auto renderClassLayoutTooltip
            = [&](MyGUI::Widget* focus, const MyGUI::IntCoord& winRect, int tooltipWidth) -> bool {
            if (!focus || !focus->isUserString("ToolTipLayout"))
                return false;
            if (focus->getUserString("ToolTipLayout") != "ClassToolTip")
                return false;

            const int padding = 8;
            const int spacing = 8;
            const int lineHeight = 18;
            const int width
                = tooltipWidth > 0 ? tooltipWidth : (Settings::gui().mControllerMenus ? winRect.width : 300);

            auto getText = [&](std::string_view key) -> std::string {
                if (!focus->isUserString(std::string(key)))
                    return std::string();
                return MyGUI::LanguageManager::getInstance().replaceTags(
                    std::string(focus->getUserString(std::string(key))));
            };

            const std::string nameText = getText("Caption_ClassName");
            const std::string descriptionText = getText("Caption_ClassDescription");
            const std::string specializationText = getText("Caption_ClassSpecialisation");

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box_NoTransp");

            int y = padding;
            bool hasContent = false;
            const int textWidth = std::max(0, width - padding * 2);

            auto addSpacing = [&]() {
                if (hasContent)
                    y += spacing;
            };

            if (!nameText.empty())
            {
                MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(padding, y, textWidth, lineHeight), MyGUI::Align::Center | MyGUI::Align::Top);
                name->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                name->setCaption(nameText);
                y += lineHeight;
                hasContent = true;
            }

            if (!descriptionText.empty())
            {
                addSpacing();
                auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>(
                    "SandText", MyGUI::IntCoord(padding, y, textWidth, 0), MyGUI::Align::Left | MyGUI::Align::Top);
                desc->setEditStatic(true);
                desc->setNeedKeyFocus(false);
                desc->setEditMultiLine(true);
                desc->setEditWordWrap(true);
                desc->setTextAlign(MyGUI::Align::Center | MyGUI::Align::Top);
                desc->setCaptionWithReplacing(descriptionText);
                desc->forceTextUpdate();
                const MyGUI::IntSize descSize = desc->getRequestedSize();
                desc->setSize(MyGUI::IntSize(textWidth, descSize.height));
                y += descSize.height;
                hasContent = true;
            }

            if (!specializationText.empty())
            {
                addSpacing();
                MyGUI::TextBox* spec = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("SandText",
                    MyGUI::IntCoord(padding, y, textWidth, lineHeight), MyGUI::Align::Center | MyGUI::Align::Top);
                spec->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
                spec->setCaption(specializationText);
                y += lineHeight;
                hasContent = true;
            }

            if (!hasContent)
                return false;

            const int height = y + padding;
            const MyGUI::IntSize tipSize(width, height);

            if (controllerTooltipAnchor)
            {
                const int anchorHeight = 28;
                const int anchorPadding = 4;
                const int posY = std::max(0, winRect.top - anchorHeight - anchorPadding);
                setCoord(winRect.left, posY, tipSize.width, tipSize.height);
                return true;
            }

            MyGUI::IntPoint tooltipPosition = MyGUI::InputManager::getInstance().getMousePosition();
            if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
            {
                const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                tooltipPosition
                    = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
            }
            position(tooltipPosition, tipSize, viewSize);
            setCoord(tooltipPosition.left, tooltipPosition.top, tipSize.width, tipSize.height);
            return true;
        };
        auto renderMagicEffectLayoutTooltip
            = [&](MyGUI::Widget* focus, const MyGUI::IntCoord& winRect, int tooltipWidth) -> bool {
            if (!focus || !focus->isUserString("ToolTipLayout"))
                return false;
            if (focus->getUserString("ToolTipLayout") != "MagicEffectToolTip")
                return false;

            const int padding = 8;
            const int spacing = 8;
            const int iconSize = 32;
            const int lineHeight = 18;
            const int width = tooltipWidth > 0 ? tooltipWidth : 300;

            auto getText = [&](const char* key) -> std::string {
                if (!focus->isUserString(key))
                    return {};
                return MyGUI::LanguageManager::getInstance().replaceTags(std::string(focus->getUserString(key)));
            };

            const std::string nameText = getText("Caption_MagicEffectName");
            const std::string schoolText = getText("Caption_MagicEffectSchool");
            const std::string descriptionText = getText("Caption_MagicEffectDescription");
            const std::string iconTexture = focus->isUserString("ImageTexture_MagicEffectImage")
                ? std::string(focus->getUserString("ImageTexture_MagicEffectImage"))
                : std::string();

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box_NoTransp");

            int y = padding;
            int textLeft = padding;

            if (!iconTexture.empty())
            {
                const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
                const VFS::Path::Normalized iconPath
                    = Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(iconTexture), vfs);
                MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                    MyGUI::IntCoord(padding, y, iconSize, iconSize), MyGUI::Align::Left | MyGUI::Align::Top);
                icon->setImageTexture(iconPath);
                textLeft = padding + iconSize + spacing;
            }

            const int textWidth = std::max(0, width - textLeft - padding);
            int textY = y;

            if (!nameText.empty())
            {
                MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(textLeft, textY, textWidth, lineHeight), MyGUI::Align::Left | MyGUI::Align::Top);
                name->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                name->setCaption(nameText);
                textY += lineHeight;
            }

            if (!schoolText.empty())
            {
                MyGUI::TextBox* school = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("SandText",
                    MyGUI::IntCoord(textLeft, textY, textWidth, lineHeight), MyGUI::Align::Left | MyGUI::Align::Top);
                school->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                school->setCaption(schoolText);
                textY += lineHeight;
            }

            const int topBlockHeight = std::max(iconSize, textY - y);
            y += topBlockHeight + spacing;

            if (!descriptionText.empty())
            {
                auto* desc = mDynamicToolTipBox->createWidget<Gui::AutoSizedEditBox>("SandText",
                    MyGUI::IntCoord(padding, y, std::max(0, width - padding * 2), 0),
                    MyGUI::Align::Left | MyGUI::Align::Top);
                desc->setEditStatic(true);
                desc->setNeedKeyFocus(false);
                desc->setEditMultiLine(true);
                desc->setEditWordWrap(true);
                desc->setTextAlign(MyGUI::Align::Left | MyGUI::Align::Top);
                desc->setCaptionWithReplacing(descriptionText);
                desc->forceTextUpdate();
                const MyGUI::IntSize descSize = desc->getRequestedSize();
                desc->setSize(MyGUI::IntSize(std::max(0, width - padding * 2), descSize.height));
                y += descSize.height;
            }

            const int height = y + padding;
            const MyGUI::IntSize tipSize(width, height);

            if (controllerTooltipAnchor)
            {
                const int anchorHeight = 28;
                const int anchorPadding = 4;
                const int posY = std::max(0, winRect.top - anchorHeight - anchorPadding);
                setCoord(winRect.left, posY, tipSize.width, tipSize.height);
                return true;
            }

            MyGUI::IntPoint tooltipPosition = MyGUI::InputManager::getInstance().getMousePosition();
            if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
            {
                const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                tooltipPosition
                    = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
            }
            position(tooltipPosition, tipSize, viewSize);
            setCoord(tooltipPosition.left, tooltipPosition.top, tipSize.width, tipSize.height);
            return true;
        };

        // Settings window: keep legacy tooltip behavior (no collapsed/expanded states).
        auto renderSettingsTooltip = [&](MyGUI::Widget* focus, std::string_view type) -> bool {
            if (!focus || !isSettingsTooltipPipeline(focus))
                return false;

            ensureTooltipLayer("SettingsPopup");

            auto getSettingsViewport = [&]() {
                MyGUI::ILayer* layer = mMainWidget->getLayer();
                MyGUI::IntSize viewportSize = viewSize;
                MyGUI::IntPoint mousePos = MyGUI::InputManager::getInstance().getMousePosition();
                float scale = 1.f;
                if (layer)
                {
                    viewportSize = layer->getSize();
                    mousePos = layer->getPosition(mousePos.left, mousePos.top);
                    scale = MyGUIPlatform::ScalingLayer::getScaleFactor(viewportSize);
                    if (scale <= 0.f)
                        scale = 1.f;
                }
                return std::tuple<MyGUI::ILayer*, MyGUI::IntSize, MyGUI::IntPoint, float>(
                    layer, viewportSize, mousePos, scale);
            };

            auto positionInViewport = [&](MyGUI::IntPoint& position, const MyGUI::IntSize& size,
                                          const MyGUI::IntSize& viewportSize, const MyGUI::IntPoint& mousePos,
                                          float scale) {
                if (viewportSize.width <= 0 || viewportSize.height <= 0)
                    return;

                const float safeScale = scale > 0.f ? scale : 1.f;
                const int offsetY = static_cast<int>(std::lround(32.f / safeScale));
                const int offsetUp = static_cast<int>(std::lround(8.f / safeScale));

                position += MyGUI::IntPoint(0, offsetY)
                    - MyGUI::IntPoint(static_cast<int>(mousePos.left / float(viewportSize.width) * size.width), 0);

                if ((position.left + size.width) > viewportSize.width)
                {
                    position.left = viewportSize.width - size.width;
                }
                if ((position.top + size.height) > viewportSize.height)
                {
                    position.top = mousePos.top - size.height - offsetUp;
                }
            };

            if (type == "Layout")
            {
                if (!focus->isUserString("ToolTipLayout"))
                    return false;

                const std::string layout = std::string(focus->getUserString("ToolTipLayout"));
                if (layout == "TextToolTip" || layout == "TextToolTipOneLine")
                {
                    ToolTipInfo info;
                    if (layout == "TextToolTipOneLine")
                        info.text = std::string(focus->getUserString("Caption_TextOneLine"));
                    else
                        info.text = std::string(focus->getUserString("Caption_Text"));

                    const MyGUI::IntSize tooltipSize = createToolTip(info, false, 0, true);
                    mDynamicToolTipBox->changeWidgetSkin("HUD_Box_NoTransp_Settings");
                    auto [layer, viewportSize, mousePos, scale] = getSettingsViewport();

                    MyGUI::IntPoint tooltipPosition = mousePos;
                    if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
                    {
                        const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                        tooltipPosition = MyGUI::IntPoint(
                            focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
                        if (layer)
                            tooltipPosition = layer->getPosition(tooltipPosition.left, tooltipPosition.top);
                        mousePos = tooltipPosition;
                    }

                    positionInViewport(tooltipPosition, tooltipSize, viewportSize, mousePos, scale);
                    setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
                    return true;
                }

                MyGUI::Widget* tooltip = nullptr;
                getWidget(tooltip, focus->getUserString("ToolTipLayout"));

                tooltip->setVisible(true);
                tooltip->changeWidgetSkin("HUD_Box_NoTransp_Settings");

                const auto& userStrings = focus->getUserStrings();
                std::vector<std::pair<std::string, std::string>> pendingImageTextures;
                const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
                for (auto& userStringPair : userStrings)
                {
                    size_t underscorePos = userStringPair.first.find('_');
                    if (underscorePos == std::string::npos)
                        continue;
                    std::string key = userStringPair.first.substr(0, underscorePos);
                    std::string_view first = userStringPair.first;
                    std::string_view widgetName = first.substr(underscorePos + 1);

                    std::string_view updateType = "Property";
                    size_t caretPos = key.find('^');
                    if (caretPos != std::string::npos)
                    {
                        updateType = first.substr(0, caretPos);
                        key.erase(key.begin(), key.begin() + caretPos + 1);
                    }

                    MyGUI::Widget* w = nullptr;
                    getWidget(w, widgetName);
                    if (updateType == "Property")
                    {
                        if (key == "ImageTexture")
                        {
                            pendingImageTextures.emplace_back(
                                std::string(widgetName), std::string(userStringPair.second));
                            continue;
                        }
                        w->setProperty(key, userStringPair.second);
                    }
                    else if (updateType == "UserData")
                        w->setUserString(key, userStringPair.second);
                }

                auto updateLayoutSize = [&]() {
                    MyGUI::IntSize size = tooltip->getSize();
                    tooltip->setCoord(0, 0, size.width, size.height);
                    return size;
                };

                const MyGUI::IntSize tooltipSize = updateLayoutSize();

                for (const auto& [imageWidgetName, texture] : pendingImageTextures)
                {
                    MyGUI::Widget* imageWidget = nullptr;
                    getWidget(imageWidget, imageWidgetName);
                    if (auto* image = dynamic_cast<MyGUI::ImageBox*>(imageWidget))
                    {
                        image->setImageTexture(
                            Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(texture), *vfs));
                    }
                }

                auto [layer, viewportSize, mousePos, scale] = getSettingsViewport();

                MyGUI::IntPoint tooltipPosition = mousePos;
                if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
                {
                    const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                    tooltipPosition
                        = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
                    if (layer)
                        tooltipPosition = layer->getPosition(tooltipPosition.left, tooltipPosition.top);
                    mousePos = tooltipPosition;
                }

                positionInViewport(tooltipPosition, tooltipSize, viewportSize, mousePos, scale);
                setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
                return true;
            }

            if (type == "ToolTipInfo")
            {
                const MyGUI::IntSize tooltipSize
                    = createToolTip(*focus->getUserData<MWGui::ToolTipInfo>(), false, 0, true);
                mDynamicToolTipBox->changeWidgetSkin("HUD_Box_NoTransp_Settings");
                auto [layer, viewportSize, mousePos, scale] = getSettingsViewport();

                MyGUI::IntPoint tooltipPosition = mousePos;
                if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
                {
                    const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                    tooltipPosition
                        = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
                    if (layer)
                        tooltipPosition = layer->getPosition(tooltipPosition.left, tooltipPosition.top);
                    mousePos = tooltipPosition;
                }

                positionInViewport(tooltipPosition, tooltipSize, viewportSize, mousePos, scale);
                setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
                return true;
            }

            return false;
        };

        auto showEmptyControllerBar = [&](const MyGUI::IntCoord& winRect, int width) {
            if (luaUiWindowsVisible)
                return;
            const int height = 28;
            const int posY = std::max(0, winRect.top - height - 4);
            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box");
            setCoord(winRect.left, posY, width, height);
        };
        auto showEmptyControllerBarForActive = [&](WindowBase* activeWindow) {
            if (controllerInventory || controllerTooltipAnchor)
            {
                const int width = controllerTooltipWidth > 0 ? controllerTooltipWidth : controllerWinRect.width;
                showEmptyControllerBar(controllerWinRect, width);
                return;
            }
            if (activeWindow)
                showEmptyControllerBar(
                    activeWindow->mMainWidget->getAbsoluteCoord(), activeWindow->mMainWidget->getWidth());
        };
        auto renderCollapsedTextBar = [&](const std::string& text, const MyGUI::IntCoord& winRect, int width) -> bool {
            if (text.empty())
                return false;

            std::string displayText = MyGUI::LanguageManager::getInstance().replaceTags(text);
            if (displayText.empty())
                return false;

            const int paddingX = 6;
            const int rowHeight = 18;
            const int barHeight = 28;
            const int posY = std::max(0, winRect.top - barHeight - 4);
            const int y = std::max(0, (barHeight - rowHeight) / 2);

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box");
            setCoord(winRect.left, posY, width, barHeight);

            MyGUI::TextBox* label = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                MyGUI::IntCoord(paddingX, y, width - paddingX * 2, rowHeight),
                MyGUI::Align::Center | MyGUI::Align::VCenter);
            label->setTextAlign(MyGUI::Align::Center | MyGUI::Align::VCenter);
            label->setCaption(displayText);
            return true;
        };
        auto getControllerFocusedItem = [&](WindowBase* activeWindow, ItemStack& outItem, bool& outAllowed) -> bool {
            if (!activeWindow)
                return false;

            ItemView* itemView = nullptr;
            if (auto* invWindow = dynamic_cast<InventoryWindow*>(activeWindow))
            {
                if (invWindow->isControllerTabsActive())
                    return false;
                itemView = invWindow->getItemView();
            }
            else if (auto* tradeWindow = dynamic_cast<TradeWindow*>(activeWindow))
            {
                if (tradeWindow->isControllerTabsActive())
                    return false;
                itemView = tradeWindow->getItemView();
            }
            else if (auto* containerWindow = dynamic_cast<ContainerWindow*>(activeWindow))
                itemView = containerWindow->getItemView();
            else if (auto* companionWindow = dynamic_cast<CompanionWindow*>(activeWindow))
                itemView = companionWindow->getItemView();
            else if (auto* alchemyWindow = dynamic_cast<AlchemyWindow*>(activeWindow))
            {
                if (!alchemyWindow->isControllerItemViewFocused())
                    return false;
                itemView = alchemyWindow->getItemView();
            }
            else if (auto* selectionDialog = dynamic_cast<ItemSelectionDialog*>(activeWindow))
                itemView = selectionDialog->getItemView();

            if (!itemView)
                return false;

            ItemModel* model = itemView->getModel();
            const int itemCount = itemView->getItemCount();
            const int focusIndex = itemView->getControllerFocus();
            if (!model || itemCount <= 0 || focusIndex < 0 || focusIndex >= itemCount)
                return false;

            outItem = model->getItem(focusIndex);
            if (outItem.mBase.isEmpty())
                return false;

            outAllowed = model->allowedToUseItems();
            return true;
        };
        auto getControllerFocusWidget = [&](WindowBase* activeWindow) -> MyGUI::Widget* {
            if (!activeWindow)
                return nullptr;
            if (auto* statsWindow = dynamic_cast<StatsWindow*>(activeWindow))
                return statsWindow->getControllerFocusTooltipWidget();
            if (auto* reviewDialog = dynamic_cast<ReviewDialog*>(activeWindow))
                return reviewDialog->getControllerFocusTooltipWidget();
            if (auto* spellWindow = dynamic_cast<SpellWindow*>(activeWindow))
                return spellWindow->getControllerFocusTooltipWidget();
            if (auto* quickKeysMenu = dynamic_cast<QuickKeysMenu*>(activeWindow))
                return quickKeysMenu->getControllerFocusTooltipWidget();
            if (auto* enchantingDialog = dynamic_cast<EnchantingDialog*>(activeWindow))
                return enchantingDialog->getControllerFocusTooltipWidget();
            if (auto* alchemyWindow = dynamic_cast<AlchemyWindow*>(activeWindow))
                return alchemyWindow->getControllerFocusTooltipWidget();
            if (auto* spellCreationDialog = dynamic_cast<SpellCreationDialog*>(activeWindow))
                return spellCreationDialog->getControllerFocusTooltipWidget();
            if (auto* spellBuyingWindow = dynamic_cast<SpellBuyingWindow*>(activeWindow))
                return spellBuyingWindow->getControllerFocusTooltipWidget();
            if (auto* levelupDialog = dynamic_cast<LevelupDialog*>(activeWindow))
                return levelupDialog->getControllerFocusTooltipWidget();
            if (auto* trainingWindow = dynamic_cast<TrainingWindow*>(activeWindow))
                return trainingWindow->getControllerFocusTooltipWidget();
            if (auto* repairWindow = dynamic_cast<Repair*>(activeWindow))
                return repairWindow->getControllerFocusTooltipWidget();
            if (auto* merchantRepairWindow = dynamic_cast<MerchantRepair*>(activeWindow))
                return merchantRepairWindow->getControllerFocusTooltipWidget();
            if (auto* createClassDialog = dynamic_cast<CreateClassDialog*>(activeWindow))
                return createClassDialog->getControllerFocusTooltipWidget();
            if (auto* pickClassDialog = dynamic_cast<PickClassDialog*>(activeWindow))
                return pickClassDialog->getControllerFocusTooltipWidget();
            if (auto* birthDialog = dynamic_cast<BirthDialog*>(activeWindow))
                return birthDialog->getControllerFocusTooltipWidget();
            if (auto* selectAttributeDialog = dynamic_cast<SelectAttributeDialog*>(activeWindow))
                return selectAttributeDialog->getControllerFocusTooltipWidget();
            if (auto* selectSpecializationDialog = dynamic_cast<SelectSpecializationDialog*>(activeWindow))
                return selectSpecializationDialog->getControllerFocusTooltipWidget();
            if (auto* selectSkillDialog = dynamic_cast<SelectSkillDialog*>(activeWindow))
                return selectSkillDialog->getControllerFocusTooltipWidget();
            if (auto* rechargeWindow = dynamic_cast<Recharge*>(activeWindow))
                return rechargeWindow->getControllerFocusTooltipWidget();
            return nullptr;
        };
        auto tryUseControllerItemViewTooltip = [&](WindowBase* activeWindow) -> bool {
            if (controllerForceCollapsed)
                return false;

            ItemStack item;
            bool allowedToUse = true;
            if (!getControllerFocusedItem(activeWindow, item, allowedToUse))
                return false;

            mFocusObject = item.mBase;
            const MyGUI::IntSize tooltipSize
                = getToolTipViaPtr(static_cast<int>(item.mCount), false, !allowedToUse, controllerTooltipWidth, true);

            const int anchorHeight = 28;
            const int anchorPadding = 4;
            const int posY = std::max(0, controllerWinRect.top - anchorHeight - anchorPadding);
            setCoord(controllerWinRect.left, posY, controllerWinRect.width, tooltipSize.height);
            return true;
        };
        auto renderCollapsedFocusBar = [&](const MyGUI::IntCoord& winRect) -> bool {
            if (mFocusObject.isEmpty())
                return false;

            const int count = focusItemCount > 0 ? focusItemCount : mFocusObject.getCellRef().getCount();
            ToolTipInfo collapsedInfo = mFocusObject.getClass().getToolTipInfo(mFocusObject, count);

            const int collapsedHeight = 28;
            const int posY = std::max(0, winRect.top - collapsedHeight - 4);

            mDynamicToolTipBox->setVisible(true);
            mDynamicToolTipBox->changeWidgetSkin("HUD_Box");
            setCoord(winRect.left, posY, winRect.width, collapsedHeight);

            const int padding = 6;
            const int iconSize = 16;
            const int textHeight = collapsedHeight - 8;
            const int textTop = 4;
            const int iconTop = (collapsedHeight - iconSize) / 2;
            const int valueSpacing = 4;

            int right = winRect.width - padding;

            auto addValueWithIcon = [&](const std::string& caption, std::string_view iconTexture,
                                        const std::string& textName, const std::string& iconName) {
                MyGUI::TextBox* text = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(0, textTop, 0, textHeight), MyGUI::Align::VStretch | MyGUI::Align::Right, textName);
                text->setTextAlign(MyGUI::Align::Right | MyGUI::Align::VCenter);
                text->setCaption(caption);
                const int textWidth = std::max(iconSize, text->getTextSize().width);
                const int groupWidth = textWidth + valueSpacing + iconSize;

                right -= groupWidth;
                text->setCoord(right, textTop, textWidth, textHeight);

                MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                    MyGUI::IntCoord(text->getRight() + valueSpacing, iconTop, iconSize, iconSize),
                    MyGUI::Align::VCenter | MyGUI::Align::Right, iconName);
                icon->setImageTexture(iconTexture);

                right -= padding;
                return text;
            };

            addValueWithIcon(MyGUI::utility::toString(mFocusObject.getClass().getValue(mFocusObject)),
                "textures\\tx_goldicon.dds", "CollapsedGoldText", "CollapsedGoldIcon");

            std::ostringstream w;
            w << std::fixed << std::setprecision(1) << mFocusObject.getClass().getWeight(mFocusObject);
            MyGUI::TextBox* weightText
                = addValueWithIcon(w.str(), "textures\\weight.dds", "CollapsedWeightIcon", "CollapsedWeightText");

            MyGUI::TextBox* leftMostText = weightText;
            bool showChargeBar = false;
            int charge = 0;
            int maxCharge = 0;
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::Enchantment* enchant = nullptr;
            if (!collapsedInfo.enchant.empty())
                enchant = store.get<ESM::Enchantment>().search(collapsedInfo.enchant);
            if (enchant
                && (enchant->mData.mType == ESM::Enchantment::WhenStrikes
                    || enchant->mData.mType == ESM::Enchantment::WhenUsed))
            {
                maxCharge = MWMechanics::getEnchantmentCharge(*enchant);
                if (maxCharge > 0)
                {
                    charge
                        = collapsedInfo.remainingEnchantCharge == -1 ? maxCharge : collapsedInfo.remainingEnchantCharge;
                    showChargeBar = true;
                }
            }

            const int nameRight = std::max(padding, leftMostText->getLeft() - padding * 2);
            const int nameWidth = std::max(0, nameRight - padding);
            MyGUI::TextBox* nameText = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                MyGUI::IntCoord(padding, textTop, nameWidth, textHeight), MyGUI::Align::VStretch | MyGUI::Align::Left,
                "CollapsedName");
            nameText->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
            nameText->setCaption(collapsedInfo.caption);

            const WeaponDamageInfo damageInfo = getWeaponDamageInfo(mFocusObject);
            const bool hasDamage = damageInfo.mMin > 0 || damageInfo.mMax > 0;
            if (showChargeBar)
            {
                const int maxBarWidth = 160;
                const int minBarWidth = 48;
                const int barHeight = 18;

                const int textWidth = std::min(nameText->getTextSize().width, nameWidth);
                const int textRight = padding + textWidth;
                const int barAreaLeft = std::max(padding, textRight + padding);
                const int barAreaRight = leftMostText->getLeft() - padding;
                const int availableBarWidth = std::max(0, barAreaRight - barAreaLeft);

                int barWidth = std::min(maxBarWidth, availableBarWidth);
                if ((winRect.width - barWidth) % 2 != 0)
                    barWidth = std::max(0, barWidth - 1);

                if (barWidth >= minBarWidth)
                {
                    const int maxBarLeft = barAreaRight - barWidth;
                    int barLeft = (winRect.width - barWidth) / 2;
                    barLeft = std::clamp(barLeft, barAreaLeft, maxBarLeft);
                    const int barTop = (collapsedHeight - barHeight) / 2;
                    auto* chargeBar = mDynamicToolTipBox->createWidget<Widgets::MWDynamicStat>("MW_ChargeBar",
                        MyGUI::IntCoord(barLeft, barTop, barWidth, barHeight), MyGUI::Align::Default,
                        "CollapsedChargeBar");
                    chargeBar->setValue(charge, maxCharge);
                }
            }
            else if (hasDamage)
            {
                std::ostringstream stats;
                if (hasDamage)
                    stats << "( " << damageInfo.mLabel << " " << damageInfo.mMin << " - " << damageInfo.mMax << " )";
                const int statsLeft = padding;
                const int statsWidth = std::max(0, winRect.width - padding * 2);
                MyGUI::TextBox* statsText = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                    MyGUI::IntCoord(statsLeft, textTop, statsWidth, textHeight),
                    MyGUI::Align::VStretch | MyGUI::Align::Left, "CollapsedStats");
                statsText->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::VCenter);
                statsText->setCaption(stats.str());
            }

            return true;
        };
        auto renderActiveEffectTooltip = [&](MyGUI::Widget* effectWidget, const MyGUI::IntPoint& mousePos) -> bool {
            if (!effectWidget || !effectWidget->isUserString("ToolTipType")
                || effectWidget->getUserString("ToolTipType") != "ActiveEffect")
                return false;

            const bool controllerMenu = Settings::gui().mControllerMenus;
            const bool collapsed = controllerMenu && (!winMgr->getControllerTooltipEnabled() || controllerMap);

            const ToolTipInfo* effectTooltipInfo = effectWidget->getUserData<ToolTipInfo>(false);
            const std::string nameRaw = [&]() -> std::string {
                if (effectWidget->isUserString("ActiveEffectName"))
                {
                    const std::string userName = std::string(effectWidget->getUserString("ActiveEffectName"));
                    if (!userName.empty())
                        return userName;
                }
                if (effectTooltipInfo && !effectTooltipInfo->caption.empty())
                    return effectTooltipInfo->caption;
                return {};
            }();
            const std::string iconTexture = effectWidget->isUserString("ActiveEffectIcon")
                ? std::string(effectWidget->getUserString("ActiveEffectIcon"))
                : std::string();
            const std::string textRaw = effectWidget->isUserString("ActiveEffectText")
                ? std::string(effectWidget->getUserString("ActiveEffectText"))
                : std::string();

            const std::string nameText = MyGUI::LanguageManager::getInstance().replaceTags(nameRaw);

            MyGUI::IntSize tooltipSize;
            if (controllerMenu)
            {
                const int padding = 8;
                const int spacing = 4;
                const int iconSize = 16;
                const int width = controllerTooltipWidth > 0 ? controllerTooltipWidth : controllerWinRect.width;

                if (collapsed)
                {
                    const int height = 28;
                    const int textHeight = height - 8;
                    const int textTop = 4;

                    mDynamicToolTipBox->setVisible(true);
                    mDynamicToolTipBox->changeWidgetSkin("HUD_Box");

                    int nameLeft = padding;
                    if (!iconTexture.empty())
                    {
                        const int iconTop = (height - iconSize) / 2;
                        MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                            MyGUI::IntCoord(padding, iconTop, iconSize, iconSize),
                            MyGUI::Align::VCenter | MyGUI::Align::Left);
                        icon->setImageTexture(
                            Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(iconTexture),
                                *MWBase::Environment::get().getResourceSystem()->getVFS()));
                        nameLeft = padding + iconSize + spacing;
                    }

                    const int rightPadding = padding;
                    const int nameWidth = std::max(0, width - nameLeft - rightPadding);

                    MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                        MyGUI::IntCoord(nameLeft, textTop, nameWidth, textHeight),
                        MyGUI::Align::Left | MyGUI::Align::VCenter);
                    name->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                    name->setCaption(nameText.empty() ? nameRaw : nameText);

                    tooltipSize = MyGUI::IntSize(width, height);
                }
                else
                {
                    ToolTipInfo info;
                    info.caption = nameRaw;
                    info.icon = iconTexture;
                    info.imageSize = 16;
                    info.text = textRaw;
                    tooltipSize = createToolTip(info, false, width, true);
                }
            }
            else
            {
                ToolTipInfo info;
                info.caption = nameRaw;
                info.icon = iconTexture;
                info.imageSize = 16;
                info.text = textRaw;
                tooltipSize = createToolTip(info, false, 0, false);
            }

            if (controllerTooltipAnchor)
            {
                const int anchorHeight = 28;
                const int anchorPadding = 4;
                const int posY = std::max(0, controllerWinRect.top - anchorHeight - anchorPadding);
                setCoord(controllerWinRect.left, posY, controllerWinRect.width, tooltipSize.height);
                return true;
            }

            MyGUI::IntPoint tooltipPosition = mousePos;
            if (controllerMenu && !winMgr->getCursorVisible())
            {
                const MyGUI::IntCoord focusRect = effectWidget->getAbsoluteCoord();
                tooltipPosition
                    = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
            }

            position(tooltipPosition, tooltipSize, viewSize);
            setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
            return true;
        };
        if (guiMode)
        {
            const bool controllerActive = Settings::gui().mControllerMenus;
            if (countDialogVisible && mode == GM_Alchemy)
            {
                mFocusObject = MWWorld::Ptr();
                mDynamicToolTipBox->setVisible(false);
                if (controllerActive && !suppressControllerBar)
                {
                    MyGUI::IntCoord barRect = controllerWinRect;
                    int barWidth = barRect.width;
                    if (inventoryTooltipWidth > 0)
                    {
                        barRect = inventoryTooltipRect;
                        barWidth = inventoryTooltipWidth;
                    }
                    showEmptyControllerBar(barRect, barWidth);
                }
                return;
            }
            if (countDialogVisible && mode == GM_Barter && countDialog->getTooltipSourceWindow() == nullptr)
            {
                // Offer amount dialog: keep the top tooltip strip visible but empty.
                mFocusObject = MWWorld::Ptr();
                mDynamicToolTipBox->setVisible(false);
                if (controllerActive && !suppressControllerBar)
                {
                    MyGUI::IntCoord barRect = controllerWinRect;
                    int barWidth = barRect.width;
                    if (inventoryTooltipWidth > 0)
                    {
                        barRect = inventoryTooltipRect;
                        barWidth = inventoryTooltipWidth;
                    }
                    showEmptyControllerBar(barRect, barWidth);
                }
                return;
            }
            if (winMgr->getMode() == GM_Dialogue)
            {
                DialogueWindow* dialogueWindow = nullptr;
                for (WindowBase* window : winMgr->getGuiModeWindows(GM_Dialogue))
                {
                    if (auto* candidate = dynamic_cast<DialogueWindow*>(window))
                    {
                        dialogueWindow = candidate;
                        break;
                    }
                }
                if (renderDialogueCollapsedBar(dialogueWindow))
                    return;
            }

            const MyGUI::IntPoint& mousePos = MyGUI::InputManager::getInstance().getMousePosition();
            if (controllerInventory)
            {
                mRemainingDelay = 0.f;
            }
            else if (!winMgr->getCursorVisible() && !winMgr->getControllerTooltipVisible()
                && !(controllerActive && controllerTooltipAnchor && !winMgr->getControllerTooltipEnabled()))
            {
                if (controllerActive && !suppressControllerBar)
                {
                    if (activeControllerWindow)
                        showEmptyControllerBar(activeControllerWindow->mMainWidget->getAbsoluteCoord(),
                            activeControllerWindow->mMainWidget->getWidth());
                }
                return;
            }

            if (!controllerInventory && winMgr->getWorldMouseOver()
                && (winMgr->isConsoleMode() || (winMgr->getMode() == GM_Container)
                    || (winMgr->getMode() == GM_Inventory)))
            {
                if (mFocusObject.isEmpty())
                {
                    if (controllerActive && !suppressControllerBar)
                    {
                        if (activeControllerWindow)
                            showEmptyControllerBar(activeControllerWindow->mMainWidget->getAbsoluteCoord(),
                                activeControllerWindow->mMainWidget->getWidth());
                    }
                    return;
                }

                const MWWorld::Class& objectclass = mFocusObject.getClass();

                MyGUI::IntSize tooltipSize;
                ToolTipInfo info;
                if (!objectclass.hasToolTip(mFocusObject) && winMgr->isConsoleMode())
                {
                    setCoord(0, 0, 300, 300);
                    mDynamicToolTipBox->setVisible(true);
                    info.caption = mFocusObject.getClass().getName(mFocusObject);
                    if (info.caption.empty())
                        info.caption = mFocusObject.getCellRef().getRefId().toDebugString();
                    info.icon.clear();
                    tooltipSize = createToolTip(info, checkOwned(), 0, false);
                }
                else
                    tooltipSize = getToolTipViaPtr(mFocusObject.getCellRef().getCount(), true, false, 0, false);

                MyGUI::IntPoint tooltipPosition = MyGUI::InputManager::getInstance().getMousePosition();
                position(tooltipPosition, tooltipSize, viewSize);

                setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
            }

            else
            {
                if (mousePos.left == mLastMouseX && mousePos.top == mLastMouseY)
                    mRemainingDelay -= frameDuration;
                else
                {
                    mHorizontalScrollIndex = 0;
                    mRemainingDelay = Settings::gui().mTooltipDelay;
                }
                mLastMouseX = mousePos.left;
                mLastMouseY = mousePos.top;

                if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible())
                    mRemainingDelay = 0.f;

                if (mRemainingDelay > 0)
                {
                    if (controllerActive && !suppressControllerBar)
                    {
                        if (activeControllerWindow)
                            showEmptyControllerBar(activeControllerWindow->mMainWidget->getAbsoluteCoord(),
                                activeControllerWindow->mMainWidget->getWidth());
                    }
                    return;
                }

                if (auto* spellWindow = dynamic_cast<SpellWindow*>(activeControllerWindow))
                {
                    MyGUI::Widget* effectWidget = nullptr;
                    if (controllerActive && !winMgr->getCursorVisible())
                        effectWidget = spellWindow->getControllerFocusTooltipWidget();
                    else
                        effectWidget = spellWindow->getEffectWidgetAt(mousePos);

                    if (renderActiveEffectTooltip(effectWidget, mousePos))
                        return;
                }

                MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getMouseFocusWidget();
                if (auto* spellWindow = dynamic_cast<SpellWindow*>(activeControllerWindow))
                {
                    if (MyGUI::Widget* effect = spellWindow->getEffectWidgetAt(mousePos))
                        focus = effect;
                }
                if (controllerActive && !winMgr->getCursorVisible())
                {
                    if (activeControllerWindow)
                    {
                        if (MyGUI::Widget* controllerFocus = getControllerFocusWidget(activeControllerWindow))
                            focus = controllerFocus;
                    }
                }
                // In controller inventory modes, item tooltips should be based on the controller-focused item, not
                // whichever widget currently has mouse focus (which can desync in scroll views).
                if (controllerInventory && controllerActive && !winMgr->getCursorVisible())
                {
                    ItemStack item;
                    bool allowedToUse = true;
                    if (getControllerFocusedItem(activeControllerWindow, item, allowedToUse))
                    {
                        (void)item;
                        (void)allowedToUse;
                        focus = nullptr;
                    }
                }
                if (controllerActive && !winMgr->getCursorVisible())
                {
                    if (auto* alchemyWindow = dynamic_cast<AlchemyWindow*>(activeControllerWindow))
                    {
                        if (!alchemyWindow->isControllerItemViewFocused())
                            focus = alchemyWindow->getControllerFocusTooltipWidget();
                    }
                }
                if (focus == nullptr)
                {
                    if (controllerInventory && controllerActive)
                    {
                        if (controllerForceCollapsed)
                        {
                            ItemStack item;
                            bool allowedToUse = true;
                            if (getControllerFocusedItem(activeControllerWindow, item, allowedToUse))
                            {
                                mFocusObject = item.mBase;
                                focusItemCount = static_cast<int>(item.mCount);
                                if (renderCollapsedFocusBar(controllerWinRect))
                                    return;
                            }
                        }
                        else if (winMgr->getControllerTooltipVisible())
                        {
                            if (tryUseControllerItemViewTooltip(activeControllerWindow))
                                return;
                        }
                    }
                    else if (controllerActive)
                    {
                        if (activeControllerWindow)
                            focus = getControllerFocusWidget(activeControllerWindow);
                    }
                    if (focus == nullptr)
                    {
                        if (controllerActive && !suppressControllerBar)
                        {
                            showEmptyControllerBarForActive(activeControllerWindow);
                        }
                        return;
                    }
                }

                if (controllerActive && focus->isUserString("ForceEmptyTooltipBar")
                    && focus->getUserString("ForceEmptyTooltipBar") == "true")
                {
                    if (!suppressControllerBar)
                        showEmptyControllerBarForActive(activeControllerWindow);
                    return;
                }

                if (isTooltipDisabled(focus))
                {
                    mDynamicToolTipBox->setVisible(false);
                    return;
                }

                MyGUI::IntSize tooltipSize;
                bool allowFocusBar = false;

                // try to go 1 level up until there is a widget that has tooltip
                // this is necessary because some skin elements are actually separate widgets
                while (focus && !focus->isUserString("ToolTipType"))
                {
                    focus = focus->getParent();
                }
                if (!focus)
                {
                    if (controllerInventory && controllerActive)
                    {
                        if (controllerForceCollapsed)
                        {
                            ItemStack item;
                            bool allowedToUse = true;
                            if (getControllerFocusedItem(activeControllerWindow, item, allowedToUse))
                            {
                                mFocusObject = item.mBase;
                                focusItemCount = static_cast<int>(item.mCount);
                                if (renderCollapsedFocusBar(controllerWinRect))
                                    return;
                            }
                        }
                        else if (winMgr->getControllerTooltipVisible())
                        {
                            if (tryUseControllerItemViewTooltip(activeControllerWindow))
                                return;
                        }
                    }
                    else if (controllerActive)
                    {
                        if (activeControllerWindow)
                            focus = getControllerFocusWidget(activeControllerWindow);
                    }
                    if (!focus)
                    {
                        if (controllerActive && !suppressControllerBar)
                        {
                            showEmptyControllerBarForActive(activeControllerWindow);
                        }
                        return;
                    }
                }

                std::string_view type = focus->getUserString("ToolTipType");

                if (type.empty())
                {
                    if (controllerInventory && controllerActive)
                    {
                        if (controllerForceCollapsed)
                        {
                            ItemStack item;
                            bool allowedToUse = true;
                            if (getControllerFocusedItem(activeControllerWindow, item, allowedToUse))
                            {
                                mFocusObject = item.mBase;
                                focusItemCount = static_cast<int>(item.mCount);
                                if (renderCollapsedFocusBar(controllerWinRect))
                                    return;
                            }
                        }
                        else if (winMgr->getControllerTooltipVisible())
                        {
                            if (tryUseControllerItemViewTooltip(activeControllerWindow))
                                return;
                        }
                    }
                    else if (controllerActive)
                    {
                        if (activeControllerWindow)
                            focus = getControllerFocusWidget(activeControllerWindow);
                    }
                    if (focus == nullptr || focus->getUserString("ToolTipType").empty())
                    {
                        if (controllerActive && !suppressControllerBar)
                        {
                            showEmptyControllerBarForActive(activeControllerWindow);
                        }
                        return;
                    }
                    type = focus->getUserString("ToolTipType");
                }

                if (renderSettingsTooltip(focus, type))
                    return;

                MyGUI::IntCoord tooltipWinRect = controllerWinRect;
                int tooltipWidth = controllerTooltipWidth;
                if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible())
                {
                    if (!controllerTooltipAnchor && winMgr->getMode() != GM_Repair
                        && winMgr->getMode() != GM_MerchantRepair && winMgr->getMode() != GM_Training)
                    {
                        if (MyGUI::Window* owner = getOwningWindow(focus))
                        {
                            tooltipWinRect = owner->getAbsoluteCoord();
                            tooltipWidth = tooltipWinRect.width;
                        }
                        else if (activeControllerWindow)
                        {
                            tooltipWinRect = activeControllerWindow->mMainWidget->getAbsoluteCoord();
                            tooltipWidth = tooltipWinRect.width;
                        }
                    }
                }

                // special handling for markers on the local map: the tooltip should only be visible
                // if the marker is not hidden due to the fog of war.
                const bool forceCollapsedLayout = focus->isUserString("ForceCollapsedTooltip")
                    && focus->getUserString("ForceCollapsedTooltip") == "true";
                const bool controllerCollapseTooltip = controllerForceCollapsed
                    || (controllerActive && controllerTooltipAnchor && !winMgr->getControllerTooltipEnabled());

                if (type == "MapMarker")
                {
                    LocalMapBase::MarkerUserData data = *focus->getUserData<LocalMapBase::MarkerUserData>();

                    if (!data.isPositionExplored())
                    {
                        // Marker widgets can still receive focus when partially visible or outside explored areas.
                        // In that case, ignore them for tooltip rendering so we don't hide the controller bar.
                        if (controllerActive && !suppressControllerBar)
                        {
                            std::string cellName;
                            if (MyGUI::Window* owner = getOwningWindow(focus))
                            {
                                const std::string ownerName = owner->getName();
                                constexpr std::string_view kMainSuffix = "_Main";
                                if (ownerName.size() > kMainSuffix.size() && ownerName.ends_with(kMainSuffix))
                                {
                                    const std::string prefix
                                        = ownerName.substr(0, ownerName.size() - kMainSuffix.size());
                                    if (MyGUI::Widget* eventBox = owner->findWidget(prefix + "EventBoxLocal"))
                                    {
                                        if (eventBox->isUserString("Caption_TextOneLine"))
                                            cellName = std::string(eventBox->getUserString("Caption_TextOneLine"));
                                    }
                                }
                            }

                            if (!cellName.empty() && controllerForceCollapsed)
                                renderCollapsedTextBar(cellName, tooltipWinRect, tooltipWidth);
                            else
                                showEmptyControllerBarForActive(activeControllerWindow);
                        }
                        return;
                    }

                    if (controllerForceCollapsed && renderCollapsedTextBar(data.caption, tooltipWinRect, tooltipWidth))
                        return;

                    ToolTipInfo info;
                    info.text = data.caption;
                    info.notes = data.notes;
                    tooltipSize = createToolTip(info, false, tooltipWidth, true);
                }
                else if (type == "ItemPtr")
                {
                    mFocusObject = *focus->getUserData<MWWorld::Ptr>();
                    if (mFocusObject.isEmpty())
                        return;

                    focusItemCount = mFocusObject.getCellRef().getCount();
                    allowFocusBar = true;
                    if (!controllerCollapseTooltip)
                        tooltipSize = getToolTipViaPtr(
                            mFocusObject.getCellRef().getCount(), false, checkOwned(), tooltipWidth, true);
                }
                else if (type == "ItemModelIndex")
                {
                    std::pair<ItemModel::ModelIndex, ItemModel*> pair
                        = *focus->getUserData<std::pair<ItemModel::ModelIndex, ItemModel*>>();
                    mFocusObject = pair.second->getItem(pair.first).mBase;
                    focusItemCount = static_cast<int>(pair.second->getItem(pair.first).mCount);
                    bool isAllowedToUse = pair.second->allowedToUseItems();
                    allowFocusBar = true;
                    if (!controllerCollapseTooltip)
                        tooltipSize = getToolTipViaPtr(static_cast<int>(pair.second->getItem(pair.first).mCount), false,
                            !isAllowedToUse, tooltipWidth, true);
                }
                else if (type == "ToolTipInfo")
                {
                    tooltipSize = createToolTip(*focus->getUserData<MWGui::ToolTipInfo>(), false, tooltipWidth, true);
                }
                else if (type == "AvatarItemSelection")
                {
                    MyGUI::IntCoord avatarPos = focus->getAbsoluteCoord();
                    MyGUI::IntPoint relMousePos = MyGUI::InputManager::getInstance().getMousePosition()
                        - MyGUI::IntPoint(avatarPos.left, avatarPos.top);
                    MWWorld::Ptr item
                        = winMgr->getInventoryWindow()->getAvatarSelectedItem(relMousePos.left, relMousePos.top);

                    mFocusObject = item;
                    if (!mFocusObject.isEmpty())
                    {
                        focusItemCount = mFocusObject.getCellRef().getCount();
                        allowFocusBar = true;
                        if (!controllerCollapseTooltip)
                            tooltipSize = getToolTipViaPtr(
                                mFocusObject.getCellRef().getCount(), false, false, tooltipWidth, true);
                    }
                }
                else if (type == "Spell")
                {
                    const auto& store = MWBase::Environment::get().getESMStore();
                    const ESM::Spell* spell
                        = store->get<ESM::Spell>().find(ESM::RefId::deserialize(focus->getUserString("Spell")));
                    const std::string spellName = [&]() -> std::string {
                        if (focus->isUserString("SpellName"))
                            return std::string(focus->getUserString("SpellName"));
                        if (!spell->mName.empty())
                            return spell->mName;
                        if (const MyGUI::TextBox* textWidget = focus->castType<MyGUI::TextBox>(false))
                            return textWidget->getCaption().asUTF8();
                        return spell->mId.toDebugString();
                    }();

                    auto buildSpellToolTipInfo = [&]() {
                        ToolTipInfo info;
                        info.caption = spellName;

                        Widgets::SpellEffectList effects;
                        for (const ESM::IndexedENAMstruct& spellEffect : spell->mEffects.mList)
                        {
                            Widgets::SpellEffectParams params;
                            params.mEffectID = spellEffect.mData.mEffectID;
                            params.mSkill = spellEffect.mData.mSkill;
                            params.mAttribute = spellEffect.mData.mAttribute;
                            params.mDuration = spellEffect.mData.mDuration;
                            params.mMagnMin = spellEffect.mData.mMagnMin;
                            params.mMagnMax = spellEffect.mData.mMagnMax;
                            params.mRange = spellEffect.mData.mRange;
                            params.mArea = spellEffect.mData.mArea;
                            params.mIsConstant = (spell->mData.mType == ESM::Spell::ST_Ability);
                            params.mNoTarget = false;
                            effects.push_back(params);
                        }

                        if (MWMechanics::spellIncreasesSkill(spell))
                        {
                            ESM::RefId id = MWMechanics::getSpellSchool(spell, MWMechanics::getPlayer());
                            if (!id.empty())
                            {
                                const auto& school = store->get<ESM::Skill>().find(id)->mSchool;
                                info.text = "#{sSchool}: " + MyGUI::TextIterator::toTagsString(school->mName).asUTF8();
                            }
                        }

                        if (focus->getUserString("SpellCost") == "true")
                            info.text
                                += MWGui::ToolTips::getValueString(MWMechanics::calcSpellCost(*spell), "#{sCastCost}");

                        info.effects = std::move(effects);
                        return info;
                    };

                    const bool controllerMenu = Settings::gui().mControllerMenus;
                    const bool collapsed = controllerMenu && (!winMgr->getControllerTooltipEnabled() || controllerMap);
                    if (controllerMenu)
                    {
                        const int padding = 8;
                        const int spacing = 4;
                        const int iconSize = 16;
                        const int width = tooltipWidth > 0 ? tooltipWidth : tooltipWinRect.width;

                        mDynamicToolTipBox->setVisible(true);
                        mDynamicToolTipBox->changeWidgetSkin(collapsed ? "HUD_Box" : "HUD_Box_NoTransp");

                        std::string iconTexture;
                        for (const ESM::IndexedENAMstruct& spellEffect : spell->mEffects.mList)
                        {
                            if (spellEffect.mData.mEffectID.empty())
                                continue;
                            const ESM::MagicEffect* magicEffect
                                = store->get<ESM::MagicEffect>().find(spellEffect.mData.mEffectID);
                            if (magicEffect)
                            {
                                iconTexture = magicEffect->mIcon;
                                break;
                            }
                        }

                        if (collapsed)
                        {
                            const int height = 28;
                            const int textHeight = height - 8;
                            const int textTop = 4;

                            int nameLeft = padding;
                            if (!iconTexture.empty())
                            {
                                const int iconTop = (height - iconSize) / 2;
                                MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                                    MyGUI::IntCoord(padding, iconTop, iconSize, iconSize),
                                    MyGUI::Align::VCenter | MyGUI::Align::Left);
                                icon->setImageTexture(
                                    Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(iconTexture),
                                        *MWBase::Environment::get().getResourceSystem()->getVFS()));
                                icon->setColour(MyGUI::Colour::White);
                                icon->setAlpha(1.0f);
                                icon->setInheritsAlpha(false);
                                nameLeft = padding + iconSize + spacing;
                            }

                            std::string costChance;
                            if (spell->mData.mType == ESM::Spell::ST_Spell)
                            {
                                const int cost = MWMechanics::calcSpellCost(*spell);
                                const int chance = static_cast<int>(
                                    MWMechanics::getSpellSuccessChance(spell, MWMechanics::getPlayer()));
                                costChance = std::format("{}/{}", cost, chance);
                            }

                            const int rightPadding = padding;
                            const int valueWidth = costChance.empty() ? 0 : 64;
                            const int nameWidth = std::max(0, width - nameLeft - valueWidth - rightPadding);

                            MyGUI::TextBox* name = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                                MyGUI::IntCoord(nameLeft, textTop, nameWidth, textHeight),
                                MyGUI::Align::Left | MyGUI::Align::VCenter);
                            name->setTextAlign(MyGUI::Align::Left | MyGUI::Align::VCenter);
                            name->setCaption(spellName);

                            if (!costChance.empty())
                            {
                                MyGUI::TextBox* value = mDynamicToolTipBox->createWidget<MyGUI::TextBox>("NormalText",
                                    MyGUI::IntCoord(width - valueWidth - rightPadding, textTop, valueWidth, textHeight),
                                    MyGUI::Align::Right | MyGUI::Align::VCenter);
                                value->setTextAlign(MyGUI::Align::Right | MyGUI::Align::VCenter);
                                value->setCaption(costChance);
                            }

                            tooltipSize = MyGUI::IntSize(width, height);
                        }
                        else
                            tooltipSize = createToolTip(buildSpellToolTipInfo(), false, tooltipWidth, true);
                    }
                    else
                        tooltipSize = createToolTip(buildSpellToolTipInfo(), false, tooltipWidth, true);
                }
                else if (type == "Layout")
                {
                    if (forceCollapsedLayout)
                    {
                        if (renderCollapsedLayoutTooltip(true, focus, tooltipWinRect, tooltipWidth))
                            return;
                    }
                    const bool controllerLayoutCollapsed = controllerForceCollapsed
                        || (controllerActive && controllerTooltipAnchor && !winMgr->getControllerTooltipEnabled());
                    if (renderCollapsedLayoutTooltip(controllerLayoutCollapsed, focus, tooltipWinRect, tooltipWidth))
                        return;
                    if (renderClassLayoutTooltip(focus, tooltipWinRect, tooltipWidth))
                        return;
                    if (renderStatsLayoutTooltip(focus, tooltipWinRect, tooltipWidth))
                        return;
                    if (renderMagicEffectLayoutTooltip(focus, tooltipWinRect, tooltipWidth))
                        return;

                    // tooltip defined in the layout
                    MyGUI::Widget* tooltip;
                    getWidget(tooltip, focus->getUserString("ToolTipLayout"));

                    tooltip->setVisible(true);
                    tooltip->changeWidgetSkin("HUD_Box_NoTransp");

                    const bool forceWidth = tooltipWidth > 0;
                    if (forceWidth)
                    {
                        tooltip->setProperty("AutoResize", "false");
                        tooltip->setSize(tooltipWidth, tooltip->getHeight());
                        if (auto* box = dynamic_cast<Gui::Box*>(tooltip))
                            box->notifyChildrenSizeChanged();
                    }
                    else
                    {
                        tooltip->setProperty("AutoResize", "true");
                    }

                    const auto& userStrings = focus->getUserStrings();
                    std::vector<std::pair<std::string, std::string>> pendingImageTextures;
                    const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
                    for (auto& userStringPair : userStrings)
                    {
                        size_t underscorePos = userStringPair.first.find('_');
                        if (underscorePos == std::string::npos)
                            continue;
                        std::string key = userStringPair.first.substr(0, underscorePos);
                        std::string_view first = userStringPair.first;
                        std::string_view widgetName = first.substr(underscorePos + 1);

                        type = "Property";
                        size_t caretPos = key.find('^');
                        if (caretPos != std::string::npos)
                        {
                            type = first.substr(0, caretPos);
                            key.erase(key.begin(), key.begin() + caretPos + 1);
                        }

                        MyGUI::Widget* w;
                        getWidget(w, widgetName);
                        if (type == "Property")
                        {
                            if (key == "ImageTexture")
                            {
                                pendingImageTextures.emplace_back(
                                    std::string(widgetName), std::string(userStringPair.second));
                                continue;
                            }
                            w->setProperty(key, userStringPair.second);
                        }
                        else if (type == "UserData")
                            w->setUserString(key, userStringPair.second);
                    }

                    auto updateLayoutSize = [&]() {
                        MyGUI::IntSize size = tooltip->getSize();
                        if (forceWidth)
                        {
                            tooltip->setSize(tooltipWidth, size.height);
                            if (auto* box = dynamic_cast<Gui::Box*>(tooltip))
                                box->notifyChildrenSizeChanged();

                            if (auto* autosize = dynamic_cast<Gui::AutoSizedWidget*>(tooltip))
                            {
                                const std::string_view layout = focus->getUserString("ToolTipLayout");
                                if (layout == "ClassToolTip")
                                {
                                    MyGUI::Widget* descriptionWidget = nullptr;
                                    getWidget(descriptionWidget, "ClassDescription");
                                    if (descriptionWidget)
                                    {
                                        descriptionWidget->setProperty("Shrink", "false");
                                        if (auto* edit = dynamic_cast<Gui::AutoSizedEditBox*>(descriptionWidget))
                                            edit->forceTextUpdate();
                                    }
                                }

                                const MyGUI::IntSize requested = autosize->getRequestedSize();
                                size.width = tooltipWidth;
                                size.height = requested.height;
                                tooltip->setSize(size);
                                if (auto* box = dynamic_cast<Gui::Box*>(tooltip))
                                    box->notifyChildrenSizeChanged();
                            }
                            else
                            {
                                size.width = tooltipWidth;
                            }
                        }

                        tooltip->setCoord(0, 0, size.width, size.height);
                        return size;
                    };

                    tooltipSize = updateLayoutSize();

                    for (const auto& [imageWidgetName, texture] : pendingImageTextures)
                    {
                        MyGUI::Widget* imageWidget = nullptr;
                        getWidget(imageWidget, imageWidgetName);
                        if (auto* image = dynamic_cast<MyGUI::ImageBox*>(imageWidget))
                        {
                            image->setImageTexture(
                                Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(texture), *vfs));
                        }
                    }
                }
                else
                    throw std::runtime_error("unknown tooltip type");

                if (controllerCollapseTooltip && allowFocusBar && !suppressControllerBar
                    && renderCollapsedFocusBar(tooltipWinRect))
                    return;

                if (controllerTooltipAnchor)
                {
                    const int anchorHeight = 28;
                    const int anchorPadding = 4;
                    int posY = std::max(0, tooltipWinRect.top - anchorHeight - anchorPadding);
                    if (tooltipSize.height < viewSize.height && posY + tooltipSize.height > viewSize.height)
                        posY = std::max(0, viewSize.height - tooltipSize.height);
                    setCoord(tooltipWinRect.left, posY, tooltipWinRect.width, tooltipSize.height);
                    return;
                }

                MyGUI::IntPoint tooltipPosition = MyGUI::InputManager::getInstance().getMousePosition();
                if (Settings::gui().mControllerMenus && !winMgr->getCursorVisible() && focus)
                {
                    const MyGUI::IntCoord focusRect = focus->getAbsoluteCoord();
                    tooltipPosition
                        = MyGUI::IntPoint(focusRect.left + focusRect.width / 2, focusRect.top + focusRect.height / 2);
                }

                position(tooltipPosition, tooltipSize, viewSize);

                setCoord(tooltipPosition.left, tooltipPosition.top, tooltipSize.width, tooltipSize.height);
            }
        }
        else
        {
            if (!mFocusObject.isEmpty())
            {
                MyGUI::IntSize tooltipSize
                    = getToolTipViaPtr(mFocusObject.getCellRef().getCount(), true, checkOwned(), 0, false);

                const int left = viewSize.width / 2 - tooltipSize.width / 2;
                const int top = std::max(0, int(mFocusToolTipY * viewSize.height - tooltipSize.height - 20));
                setCoord(left, top, tooltipSize.width, tooltipSize.height);

                mDynamicToolTipBox->setVisible(true);
            }
        }
    }

    void ToolTips::position(MyGUI::IntPoint& position, MyGUI::IntSize size, MyGUI::IntSize viewportSize)
    {
        position += MyGUI::IntPoint(0, 32)
            - MyGUI::IntPoint(static_cast<int>(MyGUI::InputManager::getInstance().getMousePosition().left
                                  / float(viewportSize.width) * size.width),
                0);

        if (position.left < 0)
            position.left = 0;
        if ((position.left + size.width) > viewportSize.width)
        {
            position.left = viewportSize.width - size.width;
        }

        if (position.top < 0)
            position.top = 0;
        if ((position.top + size.height) > viewportSize.height)
        {
            position.top = MyGUI::InputManager::getInstance().getMousePosition().top - size.height - 8;
        }
        if (position.top < 0)
            position.top = 0;
    }

    void ToolTips::clear()
    {
        mFocusObject = MWWorld::Ptr();

        while (mDynamicToolTipBox->getChildCount())
        {
            MyGUI::Gui::getInstance().destroyWidget(mDynamicToolTipBox->getChildAt(0));
        }

        for (size_t i = 0; i < mMainWidget->getChildCount(); ++i)
        {
            mMainWidget->getChildAt(i)->setVisible(false);
        }
    }

    void ToolTips::setFocusObject(const MWWorld::Ptr& focus)
    {
        mFocusObject = focus;

        update(mFrameDuration);
    }

    MyGUI::IntSize ToolTips::getToolTipViaPtr(int count, bool image, bool isOwned, int forcedWidth, bool menuMode)
    {
        // this the maximum width of the tooltip before it starts word-wrapping
        setCoord(0, 0, 300, 300);

        MyGUI::IntSize tooltipSize;

        const MWWorld::Class& object = mFocusObject.getClass();
        if (!object.hasToolTip(mFocusObject))
        {
            mDynamicToolTipBox->setVisible(false);
        }
        else
        {
            mDynamicToolTipBox->setVisible(true);

            ToolTipInfo info = object.getToolTipInfo(mFocusObject, count);
            if (!image)
                info.icon.clear();
            tooltipSize = createToolTip(info, isOwned, forcedWidth, menuMode);
        }

        return tooltipSize;
    }

    bool ToolTips::checkOwned()
    {
        if (mFocusObject.isEmpty())
            return false;

        MWWorld::Ptr ptr = MWMechanics::getPlayer();
        MWWorld::Ptr victim;

        MWBase::MechanicsManager* mm = MWBase::Environment::get().getMechanicsManager();
        return !mm->isAllowedToUse(ptr, mFocusObject, victim);
    }

    MyGUI::IntSize ToolTips::createToolTip(const MWGui::ToolTipInfo& info, bool isOwned, int forcedWidth, bool menuMode)
    {
        mDynamicToolTipBox->setVisible(true);

        const int showOwned = Settings::game().mShowOwned;
        if ((showOwned == 1 || showOwned == 3) && isOwned)
            mDynamicToolTipBox->changeWidgetSkin(MWBase::Environment::get().getWindowManager()->isGuiMode()
                    ? "HUD_Box_NoTransp_Owned"
                    : "HUD_Box_Owned");
        else
            mDynamicToolTipBox->changeWidgetSkin(
                MWBase::Environment::get().getWindowManager()->isGuiMode() ? "HUD_Box_NoTransp" : "HUD_Box");

        const std::string& caption = info.caption;
        const std::string& image = info.icon;
        int imageSize = (!image.empty()) ? info.imageSize : 0;
        std::string text = info.text;
        std::string_view extra = info.extra;

        // remove the first newline (easier this way)
        if (!text.empty() && text[0] == '\n')
            text.erase(0, 1);
        if (!extra.empty() && extra[0] == '\n')
            extra = extra.substr(1);

        const ESM::Enchantment* enchant = nullptr;
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        if (!info.enchant.empty())
        {
            enchant = store.get<ESM::Enchantment>().search(info.enchant);
            if (enchant)
            {
                if (enchant->mData.mType == ESM::Enchantment::CastOnce)
                    text += "\n#{sItemCastOnce}";
                else if (enchant->mData.mType == ESM::Enchantment::WhenStrikes)
                    text += "\n#{sItemCastWhenStrikes}";
                else if (enchant->mData.mType == ESM::Enchantment::WhenUsed)
                    text += "\n#{sItemCastWhenUsed}";
                else if (enchant->mData.mType == ESM::Enchantment::ConstantEffect)
                    text += "\n#{sItemCastConstant}";
            }
        }

        // this the maximum width of the tooltip before it starts word-wrapping
        // (already set in getToolTipViaPtr, but set again here since createToolTip can be called directly)
        setCoord(0, 0, 300, 300);

        const MyGUI::IntPoint padding(8, 8);
        const int contentProbeHeight
            = menuMode ? std::max(300, MyGUI::RenderManager::getInstance().getViewSize().height) : 300;

        // Only use menu mode formatting when menuMode is true
        const int forcedContentWidth = (menuMode && forcedWidth > 0) ? std::max(0, forcedWidth - padding.left * 2) : 0;
        const int contentLeft = (forcedContentWidth > 0) ? padding.left : 0;
        const int contentWidth = (forcedContentWidth > 0) ? forcedContentWidth : 300;

        const int imageCaptionHPadding = !caption.empty() ? 8 : 0;
        const int imageCaptionVPadding = !caption.empty() ? 4 : 0;

        const int maximumWidth = (!menuMode)
            ? (MyGUI::RenderManager::getInstance().getViewSize().width - imageCaptionHPadding * 2)
            : ((forcedContentWidth > 0)
                    ? forcedContentWidth
                    : MyGUI::RenderManager::getInstance().getViewSize().width - imageCaptionHPadding * 2);

        const VFS::Path::Normalized realImage = Misc::ResourceHelpers::correctIconPath(
            VFS::Path::toNormalized(image), *MWBase::Environment::get().getResourceSystem()->getVFS());

        Gui::EditBox* captionWidget = mDynamicToolTipBox->createWidget<Gui::EditBox>("NormalText",
            MyGUI::IntCoord(0, 0, (!menuMode) ? 300 : contentWidth, contentProbeHeight),
            MyGUI::Align::Left | MyGUI::Align::Top, "ToolTipCaption");
        captionWidget->setEditStatic(true);
        captionWidget->setNeedKeyFocus(false);
        captionWidget->setCaptionWithReplacing(caption);
        captionWidget->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::Top);
        MyGUI::IntSize captionSize = captionWidget->getTextSize();

        int captionHeight = std::max(!caption.empty() ? captionSize.height : 0, imageSize);

        const int textWidgetWidth = (!menuMode) ? 300 : contentWidth;
        Gui::EditBox* textWidget = mDynamicToolTipBox->createWidget<Gui::EditBox>("SandText",
            MyGUI::IntCoord((!menuMode) ? 0 : contentLeft, captionHeight + imageCaptionVPadding, textWidgetWidth,
                std::max(0, contentProbeHeight - captionHeight - imageCaptionVPadding)),
            (!menuMode) ? MyGUI::Align::Stretch : (MyGUI::Align::Left | MyGUI::Align::Top), "ToolTipText");
        textWidget->setEditStatic(true);
        textWidget->setEditMultiLine(true);
        textWidget->setEditWordWrap(info.wordWrap);
        textWidget->setCaptionWithReplacing(text);
        textWidget->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::Top);
        textWidget->setNeedKeyFocus(false);
        MyGUI::IntSize textSize = textWidget->getTextSize();

        captionSize += MyGUI::IntSize(imageSize, 0); // adjust for image
        MyGUI::IntSize totalSize = MyGUI::IntSize(
            std::min(std::max(textSize.width, captionSize.width + ((!image.empty()) ? imageCaptionHPadding : 0)),
                maximumWidth),
            (!text.empty() ? textSize.height + imageCaptionVPadding : 0) + captionHeight);
        if (menuMode && forcedContentWidth > 0)
            totalSize.width = forcedContentWidth;

        for (const std::string& note : info.notes)
        {
            MyGUI::ImageBox* icon = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("MarkerButton",
                MyGUI::IntCoord((!menuMode) ? 0 : contentLeft, totalSize.height + padding.top, 8, 8),
                MyGUI::Align::Default);
            icon->setColour(MyGUI::Colour(1.0f, 0.3f, 0.3f));
            Gui::EditBox* edit = mDynamicToolTipBox->createWidget<Gui::EditBox>("SandText",
                MyGUI::IntCoord((!menuMode) ? (8 + 4) : (contentLeft + 8 + 4), totalSize.height + padding.top,
                    (!menuMode) ? (300 - 8 - 4) : (contentWidth - 8 - 4),
                    std::max(0, contentProbeHeight - totalSize.height)),
                MyGUI::Align::Default);
            constexpr size_t maxLength = 60;
            std::string shortenedNote = note.substr(0, std::min(maxLength, note.find('\n')));
            if (shortenedNote.size() < note.size())
                shortenedNote += " ...";
            edit->setCaption(shortenedNote);
            MyGUI::IntSize noteTextSize = edit->getTextSize();
            edit->setSize(std::max(edit->getWidth(), noteTextSize.width), noteTextSize.height);
            icon->setPosition(icon->getLeft(), (edit->getTop() + edit->getBottom()) / 2 - icon->getHeight() / 2);
            totalSize.height += std::max(edit->getHeight(), icon->getHeight());
            totalSize.width = std::max(totalSize.width, edit->getWidth() + 8 + 4);
        }

        if (!info.effects.empty())
        {
            const int effectAreaTop = totalSize.height;
            MyGUI::Widget* effectArea = mDynamicToolTipBox->createWidget<MyGUI::Widget>({},
                MyGUI::IntCoord((!menuMode) ? padding.left : contentLeft, effectAreaTop,
                    (!menuMode) ? (300 - padding.left) : contentWidth,
                    std::max(0, contentProbeHeight - totalSize.height)),
                (!menuMode) ? MyGUI::Align::Stretch : (MyGUI::Align::Left | MyGUI::Align::Top));

            MyGUI::IntCoord coord(0, 6, totalSize.width, 24);

            Widgets::MWEffectListPtr effectsWidget
                = effectArea->createWidget<Widgets::MWEffectList>("MW_StatName", coord, MyGUI::Align::Default);
            effectsWidget->setEffectList(info.effects);

            std::vector<MyGUI::Widget*> effectItems;
            int flag = info.isPotion ? Widgets::MWEffectList::EF_NoTarget : 0;
            flag |= info.isIngredient ? Widgets::MWEffectList::EF_NoMagnitude : 0;
            flag |= info.isIngredient ? Widgets::MWEffectList::EF_Constant : 0;
            const bool centerEffects = menuMode ? true : (info.isPotion || info.isIngredient);
            effectsWidget->createEffectWidgets(effectItems, effectArea, coord, centerEffects, flag);

            if (menuMode)
                effectArea->setSize(effectArea->getWidth(), std::max(effectArea->getHeight(), coord.top));

            totalSize.height += coord.top - 6;
            totalSize.width = std::max(totalSize.width, coord.width);
        }

        if (enchant)
        {
            const int enchantAreaTop = totalSize.height;
            MyGUI::Widget* enchantArea = mDynamicToolTipBox->createWidget<MyGUI::Widget>({},
                MyGUI::IntCoord((!menuMode) ? padding.left : contentLeft, enchantAreaTop,
                    (!menuMode) ? (300 - padding.left) : contentWidth,
                    std::max(0, contentProbeHeight - totalSize.height)),
                (!menuMode) ? MyGUI::Align::Stretch : (MyGUI::Align::Left | MyGUI::Align::Top));

            MyGUI::IntCoord coord(0, 6, totalSize.width, 24);

            Widgets::MWEffectListPtr enchantWidget
                = enchantArea->createWidget<Widgets::MWEffectList>("MW_StatName", coord, MyGUI::Align::Default);
            enchantWidget->setEffectList(Widgets::MWEffectList::effectListFromESM(&enchant->mEffects));

            std::vector<MyGUI::Widget*> enchantEffectItems;
            int flag
                = (enchant->mData.mType == ESM::Enchantment::ConstantEffect) ? Widgets::MWEffectList::EF_Constant : 0;
            const bool centerEnchantEffects = false;
            enchantWidget->createEffectWidgets(enchantEffectItems, enchantArea, coord, centerEnchantEffects, flag);

            totalSize.height += coord.top - 6;
            totalSize.width = std::max(totalSize.width, coord.width);

            int enchantAreaHeight = coord.top;

            if (enchant->mData.mType == ESM::Enchantment::WhenStrikes
                || enchant->mData.mType == ESM::Enchantment::WhenUsed)
            {
                const int maxCharge = MWMechanics::getEnchantmentCharge(*enchant);
                int charge = (info.remainingEnchantCharge == -1) ? maxCharge : info.remainingEnchantCharge;

                const int maxChargeWidth = 155;

                MyGUI::TextBox* chargeText = enchantArea->createWidget<MyGUI::TextBox>(
                    "SandText", MyGUI::IntCoord(0, 0, 10, 18), MyGUI::Align::Default, "ToolTipEnchantChargeText");
                chargeText->setCaptionWithReplacing("#{sCharges}");

                const int chargeTextWidth = chargeText->getTextSize().width + 5;

                const int chargeWidth = maxChargeWidth;
                const int chargeAndTextWidth = chargeWidth + chargeTextWidth;

                totalSize.width = std::max(totalSize.width, chargeAndTextWidth);

                chargeText->setCoord((totalSize.width - chargeAndTextWidth) / 2, coord.top + 6, chargeTextWidth, 18);

                MyGUI::IntCoord chargeCoord;
                if (totalSize.width < chargeWidth)
                {
                    totalSize.width = chargeWidth;
                    chargeCoord = MyGUI::IntCoord(0, coord.top + 6, chargeWidth, 18);
                }
                else
                {
                    chargeCoord = MyGUI::IntCoord(
                        (totalSize.width - chargeAndTextWidth) / 2 + chargeTextWidth, coord.top + 6, chargeWidth, 18);
                }
                Widgets::MWDynamicStatPtr chargeWidget = enchantArea->createWidget<Widgets::MWDynamicStat>(
                    "MW_ChargeBar", chargeCoord, MyGUI::Align::Default);
                chargeWidget->setValue(charge, maxCharge);
                totalSize.height += 24;

                enchantAreaHeight += 24;
            }

            if (menuMode)
                enchantArea->setSize(enchantArea->getWidth(), std::max(enchantArea->getHeight(), enchantAreaHeight));
        }

        Gui::EditBox* extraWidget = nullptr;
        if (!extra.empty())
        {
            extraWidget = mDynamicToolTipBox->createWidget<Gui::EditBox>("SandText",
                MyGUI::IntCoord((!menuMode) ? 0 : contentLeft, totalSize.height + 12, (!menuMode) ? 300 : contentWidth,
                    std::max(0, contentProbeHeight - totalSize.height)),
                (!menuMode) ? MyGUI::Align::Stretch : (MyGUI::Align::Left | MyGUI::Align::Top), "ToolTipExtraText");

            extraWidget->setEditStatic(true);
            extraWidget->setEditMultiLine(true);
            extraWidget->setEditWordWrap(info.wordWrap);
            extraWidget->setCaptionWithReplacing(extra);
            extraWidget->setTextAlign(MyGUI::Align::HCenter | MyGUI::Align::Top);
            extraWidget->setNeedKeyFocus(false);

            MyGUI::IntSize extraTextSize = extraWidget->getTextSize();
            totalSize.height += extraTextSize.height + 4;
            totalSize.width = std::max(totalSize.width, extraTextSize.width);
        }

        captionWidget->setCoord((totalSize.width - captionSize.width) / 2 + imageSize,
            (captionHeight - captionSize.height) / 2, captionSize.width - imageSize, captionSize.height);

        // if its too long we do hscroll with the caption
        const bool captionScrolling = captionSize.width > maximumWidth;
        if (captionScrolling)
        {
            mHorizontalScrollIndex = mHorizontalScrollIndex + 2;
            if (mHorizontalScrollIndex > captionSize.width)
            {
                mHorizontalScrollIndex = -totalSize.width;
            }
            int horizontalScroll = mHorizontalScrollIndex;
            if (horizontalScroll < 40)
            {
                horizontalScroll = 40;
            }
            else
            {
                horizontalScroll = 80 - mHorizontalScrollIndex;
            }
            captionWidget->setPosition(
                MyGUI::IntPoint(horizontalScroll, captionWidget->getPosition().top + padding.top));
        }
        else
        {
            captionWidget->setPosition(captionWidget->getPosition() + padding);
        }

        const int captionCenterX = captionWidget->getLeft() + captionWidget->getWidth() / 2;

        textWidget->setPosition(
            textWidget->getPosition() + MyGUI::IntPoint(0, padding.top)); // only apply vertical padding

        if (menuMode)
        {
            textWidget->setCoord(0, textWidget->getCoord().top, totalSize.width, textWidget->getHeight());
            if (extraWidget != nullptr)
                extraWidget->setCoord(0, extraWidget->getCoord().top, totalSize.width, extraWidget->getHeight());
        }

        const int textWidgetCenterX = textWidget->getLeft() + textWidget->getWidth() / 2;
        const int textCenterOffset = (menuMode && !captionScrolling) ? (captionCenterX - textWidgetCenterX) : 0;

        if (textCenterOffset != 0)
        {
            textWidget->setPosition(textWidget->getPosition() + MyGUI::IntPoint(textCenterOffset, 0));
            if (extraWidget != nullptr)
                extraWidget->setPosition(extraWidget->getPosition() + MyGUI::IntPoint(textCenterOffset, 0));
        }

        if (!image.empty())
        {
            MyGUI::ImageBox* imageWidget = mDynamicToolTipBox->createWidget<MyGUI::ImageBox>("ImageBox",
                MyGUI::IntCoord(
                    (totalSize.width - captionSize.width - imageCaptionHPadding) / 2, 0, imageSize, imageSize),
                MyGUI::Align::Left | MyGUI::Align::Top);
            imageWidget->setImageTexture(realImage);
            imageWidget->setPosition(imageWidget->getPosition() + padding);
        }

        totalSize += MyGUI::IntSize(padding.left * 2, padding.top * 2);

        return totalSize;
    }

    std::string ToolTips::toString(const float value)
    {
        std::string s = std::format("{:.2f}", value);
        // Trim result so 1.00 turns into 1
        while (!s.empty() && s.back() == '0')
            s.pop_back();
        if (!s.empty() && s.back() == '.')
            s.pop_back();
        return s;
    }

    std::string ToolTips::toString(const int value)
    {
        return std::to_string(value);
    }

    std::string ToolTips::getWeightString(const float weight, std::string_view prefix)
    {
        if (weight == 0)
            return {};
        return std::format("\n{}: {}", prefix, toString(weight));
    }

    std::string ToolTips::getPercentString(const float value, std::string_view prefix)
    {
        if (value == 0)
            return {};
        return std::format("\n{}: {}%", prefix, toString(value * 100));
    }

    std::string ToolTips::getValueString(const int value, std::string_view prefix)
    {
        if (value == 0)
            return {};
        return std::format("\n{}: {}", prefix, value);
    }

    std::string ToolTips::getMiscString(std::string_view text, std::string_view prefix)
    {
        if (text.empty())
            return {};
        return std::format("\n{}: {}", prefix, text);
    }

    std::string ToolTips::getCountString(const int value)
    {
        if (value == 1)
            return {};
        return std::format(" ({})", value);
    }

    std::string ToolTips::getSoulString(const MWWorld::CellRef& cellref)
    {
        const ESM::RefId& soul = cellref.getSoul();
        if (soul.empty())
            return {};
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Creature* creature = store.get<ESM::Creature>().search(soul);
        if (!creature)
            return {};
        if (creature->mName.empty())
            return std::format(" ({})", creature->mId.toDebugString());
        return std::format(" ({})", creature->mName);
    }

    std::string ToolTips::getCellRefString(const MWWorld::CellRef& cellref)
    {
        std::string ret;
        ret += getMiscString(cellref.getOwner().getRefIdString(), "Owner");
        const ESM::RefId& factionId = cellref.getFaction();
        if (!factionId.empty())
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const ESM::Faction* fact = store.get<ESM::Faction>().search(factionId);
            if (fact != nullptr)
            {
                ret += getMiscString(fact->mName.empty() ? factionId.getRefIdString() : fact->mName, "Owner Faction");
                if (cellref.getFactionRank() >= 0)
                {
                    int rank = cellref.getFactionRank();
                    const std::string& rankName = fact->mRanks[rank];
                    if (rankName.empty())
                        ret += getValueString(cellref.getFactionRank(), "Rank");
                    else
                        ret += getMiscString(rankName, "Rank");
                }
            }
        }

        std::vector<std::pair<ESM::RefId, int>> itemOwners
            = MWBase::Environment::get().getMechanicsManager()->getStolenItemOwners(cellref.getRefId());

        for (std::pair<ESM::RefId, int>& owner : itemOwners)
        {
            if (owner.second == std::numeric_limits<int>::max())
                ret += std::format("\nStolen from {}", owner.first.toDebugString()); // for legacy (ESS) savegames
            else
                ret += std::format("\nStolen {} from {}", owner.second, owner.first.toDebugString());
        }

        ret += getMiscString(cellref.getGlobalVariable(), "Global");
        return ret;
    }

    std::string ToolTips::getDurationString(float duration, std::string_view prefix)
    {
        auto l10n = MWBase::Environment::get().getL10nManager()->getContext("Interface");

        std::string ret(prefix);
        ret += ": ";

        if (duration < 1.f)
        {
            ret += l10n->formatMessage("DurationSecond", { "seconds" }, { 0 });
            return ret;
        }

        constexpr int secondsPerMinute = 60; // 60 seconds
        constexpr int secondsPerHour = secondsPerMinute * 60; // 60 minutes
        constexpr int secondsPerDay = secondsPerHour * 24; // 24 hours
        constexpr int secondsPerMonth = secondsPerDay * 30; // 30 days
        constexpr int secondsPerYear = secondsPerDay * 365;
        int fullDuration = static_cast<int>(duration);
        int units = 0;
        int years = fullDuration / secondsPerYear;
        int months = fullDuration % secondsPerYear / secondsPerMonth;
        int days = fullDuration % secondsPerYear % secondsPerMonth
            / secondsPerDay; // Because a year is not exactly 12 "months"
        int hours = fullDuration % secondsPerDay / secondsPerHour;
        int minutes = fullDuration % secondsPerHour / secondsPerMinute;
        int seconds = fullDuration % secondsPerMinute;
        if (years)
        {
            units++;
            ret += l10n->formatMessage("DurationYear", { "years" }, { years });
        }
        if (months)
        {
            units++;
            ret += l10n->formatMessage("DurationMonth", { "months" }, { months });
        }
        if (units < 2 && days)
        {
            units++;
            ret += l10n->formatMessage("DurationDay", { "days" }, { days });
        }
        if (units < 2 && hours)
        {
            units++;
            ret += l10n->formatMessage("DurationHour", { "hours" }, { hours });
        }
        if (units >= 2)
            return ret;
        if (minutes)
            ret += l10n->formatMessage("DurationMinute", { "minutes" }, { minutes });
        if (seconds)
            ret += l10n->formatMessage("DurationSecond", { "seconds" }, { seconds });

        return ret;
    }

    bool ToolTips::toggleFullHelp()
    {
        mFullHelp = !mFullHelp;
        return mFullHelp;
    }

    bool ToolTips::getFullHelp() const
    {
        return mFullHelp;
    }

    void ToolTips::setFocusObjectScreenCoords(float x, float y)
    {
        mFocusToolTipX = x;
        mFocusToolTipY = y;
    }

    void ToolTips::createSkillToolTip(MyGUI::Widget* widget, ESM::RefId skillId)
    {
        if (skillId.empty())
            return;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const ESM::Skill* skill = store.get<ESM::Skill>().find(skillId);
        const ESM::Attribute* attr
            = store.get<ESM::Attribute>().find(ESM::Attribute::indexToRefId(skill->mData.mAttribute));
        const VFS::Path::Normalized iconPath
            = Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(skill->mIcon), *vfs);

        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "SkillNoProgressToolTip");
        widget->setUserString("Caption_SkillNoProgressName", MyGUI::TextIterator::toTagsString(skill->mName));
        widget->setUserString("Caption_SkillNoProgressDescription", skill->mDescription);
        widget->setUserString("Caption_SkillNoProgressAttribute",
            "#{sGoverningAttribute}: " + MyGUI::TextIterator::toTagsString(attr->mName));
        widget->setUserString("ImageTexture_SkillNoProgressImage", iconPath);
    }

    void ToolTips::createAttributeToolTip(MyGUI::Widget* widget, ESM::RefId attributeId)
    {
        const ESM::Attribute* attribute
            = MWBase::Environment::get().getESMStore()->get<ESM::Attribute>().search(attributeId);
        if (!attribute)
            return;
        const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const VFS::Path::Normalized iconPath
            = Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(attribute->mIcon), *vfs);

        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "AttributeToolTip");
        widget->setUserString("Caption_AttributeName", MyGUI::TextIterator::toTagsString(attribute->mName));
        widget->setUserString(
            "Caption_AttributeDescription", MyGUI::TextIterator::toTagsString(attribute->mDescription));
        widget->setUserString("ImageTexture_AttributeImage", iconPath);
    }

    void ToolTips::createSpecializationToolTip(MyGUI::Widget* widget, std::string_view name, int specId)
    {
        widget->setUserString("Caption_Caption", name);
        std::string specText;
        // get all skills of this specialisation
        const MWWorld::Store<ESM::Skill>& skills = MWBase::Environment::get().getESMStore()->get<ESM::Skill>();

        bool isFirst = true;
        for (const auto& skill : skills)
        {
            if (skill.mData.mSpecialization == specId)
            {
                if (isFirst)
                    isFirst = false;
                else
                    specText += "\n";

                specText += MyGUI::TextIterator::toTagsString(skill.mName);
            }
        }
        widget->setUserString("Caption_ColumnText", specText);
        widget->setUserString("ToolTipLayout", "SpecializationToolTip");
        widget->setUserString("ToolTipType", "Layout");
    }

    void ToolTips::createBirthsignToolTip(MyGUI::Widget* widget, const ESM::RefId& birthsignId)
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

        const ESM::BirthSign* sign = store.get<ESM::BirthSign>().find(birthsignId);
        const VFS::Manager* const vfs = MWBase::Environment::get().getResourceSystem()->getVFS();

        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "BirthSignToolTip");
        widget->setUserString("ImageTexture_BirthSignImage",
            Misc::ResourceHelpers::correctTexturePath(VFS::Path::toNormalized(sign->mTexture), *vfs));
        widget->setUserString("Caption_BirthSignName", sign->mName);
        widget->setUserString("Caption_BirthSignDescription", sign->mDescription);

        std::vector<const ESM::Spell*> abilities, powers, spells;

        for (const ESM::RefId& spellId : sign->mPowers.mList)
        {
            const ESM::Spell* spell = store.get<ESM::Spell>().search(spellId);
            if (!spell)
                continue; // Skip spells which cannot be found
            ESM::Spell::SpellType type = static_cast<ESM::Spell::SpellType>(spell->mData.mType);
            if (type != ESM::Spell::ST_Spell && type != ESM::Spell::ST_Ability && type != ESM::Spell::ST_Power)
                continue; // We only want spell, ability and powers.

            if (type == ESM::Spell::ST_Ability)
                abilities.push_back(spell);
            else if (type == ESM::Spell::ST_Power)
                powers.push_back(spell);
            else if (type == ESM::Spell::ST_Spell)
                spells.push_back(spell);
        }

        using Category = std::tuple<const std::vector<const ESM::Spell*>&, std::string_view, std::string_view>;
        std::initializer_list<Category> categories{ { abilities, "#{sBirthsignmenu1}", "Abilities" },
            { powers, "#{sPowers}", "Powers" }, { spells, "#{sBirthsignmenu2}", "Spells" } };

        for (const auto& [category, label, widgetName] : categories)
        {
            std::string text;
            if (!category.empty())
            {
                text = std::string(label) + "\n#{fontcolourhtml=normal}";
                for (const ESM::Spell* spell : category)
                    text += spell->mName + ' ';
                text.pop_back();
            }
            widget->setUserString("Caption_BirthSign" + std::string(widgetName), text);
        }
    }

    void ToolTips::createRaceToolTip(MyGUI::Widget* widget, const ESM::Race* playerRace)
    {
        widget->setUserString("Caption_CenteredCaption", playerRace->mName);
        widget->setUserString("Caption_CenteredCaptionText", playerRace->mDescription);
        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "RaceToolTip");
    }

    void ToolTips::createClassToolTip(MyGUI::Widget* widget, const ESM::Class& playerClass)
    {
        if (playerClass.mName.empty())
            return;

        std::string description = playerClass.mDescription;
        Misc::StringUtils::trim(description);

        int spec = playerClass.mData.mSpecialization;
        std::string specStr = "#{";
        specStr += ESM::Class::sGmstSpecializationIds[spec];
        specStr += '}';

        widget->setUserString("Caption_ClassName", playerClass.mName);
        if (!description.empty())
            widget->setUserString("Caption_ClassDescription", description);
        else
            widget->clearUserString("Caption_ClassDescription");
        widget->setUserString("UserData^Hidden_ClassDescription", description.empty() ? "true" : "false");
        widget->setUserString("Caption_ClassSpecialisation", "#{sSpecialization}: " + specStr);
        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "ClassToolTip");
    }

    void ToolTips::createMagicEffectToolTip(MyGUI::Widget* widget, ESM::RefId effectId)
    {
        const auto& store = MWBase::Environment::get().getESMStore();
        const ESM::MagicEffect* effect = store->get<ESM::MagicEffect>().find(effectId);

        const VFS::Manager& vfs = *MWBase::Environment::get().getResourceSystem()->getVFS();
        VFS::Path::Normalized iconPath
            = Misc::ResourceHelpers::correctBigIconPath(VFS::Path::toNormalized(effect->mIcon), vfs);
        if (!vfs.exists(iconPath))
            iconPath = Misc::ResourceHelpers::correctIconPath(VFS::Path::toNormalized(effect->mIcon), vfs);

        widget->setUserString("ToolTipType", "Layout");
        widget->setUserString("ToolTipLayout", "MagicEffectToolTip");
        widget->setUserString("Caption_MagicEffectName", effect->mName);
        widget->setUserString("Caption_MagicEffectDescription", effect->mDescription);
        widget->setUserString("Caption_MagicEffectSchool",
            "#{sSchool}: "
                + MyGUI::TextIterator::toTagsString(
                    store->get<ESM::Skill>().find(effect->mData.mSchool)->mSchool->mName));
        widget->setUserString("ImageTexture_MagicEffectImage", iconPath);
    }
}
