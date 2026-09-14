#include "spellwindow.hpp"

#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_Window.h>

#include <components/esm3/loadbsgn.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/misc/strings/format.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/datetimemanager.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spells.hpp"
#include "../mwmechanics/spellutil.hpp"

#include "confirmationdialog.hpp"
#include "spellicons.hpp"
#include "spellview.hpp"
#include "statswindow.hpp"

namespace MWGui
{

    SpellWindow::SpellWindow(DragAndDrop* drag)
        : WindowPinnableBase("openmw_spell_window.layout")
        , NoDrop(drag, mMainWidget)
        , mSpellView(nullptr)
        , mUpdateTimer(0.0f)
    {
        mSpellIcons = std::make_unique<SpellIcons>();

        getWidget(mSpellView, "SpellView");
        getWidget(mEffectBox, "EffectsBox");
        if (mEffectBox)
        {
            mEffectIcons.clear();
            mEffectIconCoords.clear();
        }

        mSpellView->eventSpellClicked += MyGUI::newDelegate(this, &SpellWindow::onModelIndexSelected);

        setCoord(498, 300, 302, 300);

        if (Settings::gui().mControllerMenus)
        {
            setPinButtonVisible(false);
            mControllerButtons = {};
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Back}";
            mControllerButtons.mY = "#{Interface:Info}";
            mControllerButtons.mX = "#{Interface:Delete}";
            mControllerButtons.mL2 = "#{Interface:Inventory}";
            mControllerButtons.mR2 = Settings::gui().mXboxTabOrder ? "#{Interface:Map}" : "#{sStats}";
            mDisableGamepadCursor = true;
        }
    }

    ControllerButtons* SpellWindow::getControllerButtons()
    {
        if (Settings::gui().mControllerMenus)
        {
            const bool skipMap = MWBase::Environment::get().getWindowManager()->isCrassifiedNavigationEnabled();
            mControllerButtons.mL2 = "#{Interface:Inventory}";
            mControllerButtons.mR2 = (Settings::gui().mXboxTabOrder && !skipMap) ? "#{Interface:Map}" : "#{sStats}";
        }

        return &mControllerButtons;
    }

    void SpellWindow::onPinToggled()
    {
        Settings::windows().mSpellsPin.set(mPinned);

        MWBase::Environment::get().getWindowManager()->setSpellVisibility(!mPinned);
    }

    void SpellWindow::onTitleDoubleClicked()
    {
        if (Settings::gui().mControllerMenus)
            return;
        else if (MyGUI::InputManager::getInstance().isShiftPressed())
            MWBase::Environment::get().getWindowManager()->toggleMaximized(this);
        else if (!mPinned)
            MWBase::Environment::get().getWindowManager()->toggleVisible(GW_Magic);
    }

    void SpellWindow::onOpen()
    {
        resetFixedWindowGeometry();

        updateSpells();
    }

    void SpellWindow::onFrame(float dt)
    {
        NoDrop::onFrame(dt);
        mUpdateTimer += dt;
        if (0.5f < mUpdateTimer)
        {
            mUpdateTimer = 0;
            mSpellView->incrementalUpdate();
        }

        // Update effects if the time is unpaused for any reason (e.g. the window is pinned)
        if (!MWBase::Environment::get().getWorld()->getTimeManager()->isPaused())
            mSpellIcons->updateWidgets(mEffectBox, false);
        refreshEffectWidgets();
        updateEffectsHighlight();
    }

    void SpellWindow::resetFixedWindowGeometry()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            window->setCoord(getFixedWindowCoord(viewSize));
        }
    }

    void SpellWindow::updateSpells()
    {
        mSpellIcons->updateWidgets(mEffectBox, false);
        mSpellView->setModel(new SpellModel(MWMechanics::getPlayer()));
        refreshEffectWidgets();
        updateEffectsHighlight();
    }

    void SpellWindow::onEnchantedItemSelected(MWWorld::Ptr item, bool alreadyEquipped)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);

        // retrieve ContainerStoreIterator to the item
        MWWorld::ContainerStoreIterator it = store.begin();
        for (; it != store.end(); ++it)
        {
            if (*it == item)
            {
                break;
            }
        }
        if (it == store.end())
            throw std::runtime_error("can't find selected item");

        // equip, if it can be equipped and is not already equipped
        if (!alreadyEquipped && !item.getClass().getEquipmentSlots(item).first.empty())
        {
            MWBase::Environment::get().getWindowManager()->useItem(item);
            // make sure that item was successfully equipped
            if (!store.isEquipped(item))
                return;
        }

        store.setSelectedEnchantItem(it);
        // to reset WindowManager::mSelectedSpell immediately
        MWBase::Environment::get().getWindowManager()->setSelectedEnchantItem(*it);

        updateSpells();
    }

    void SpellWindow::askDeleteSpell(const ESM::RefId& spellId)
    {
        // delete spell, if allowed
        const ESM::Spell* spell = MWBase::Environment::get().getESMStore()->get<ESM::Spell>().find(spellId);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        const ESM::RefId& raceId = player.get<ESM::NPC>()->mBase->mRace;
        const ESM::Race* race = MWBase::Environment::get().getESMStore()->get<ESM::Race>().find(raceId);
        // can't delete racial spells, birthsign spells or powers
        bool isInherent = race->mPowers.exists(spell->mId) || spell->mData.mType == ESM::Spell::ST_Power;
        const ESM::RefId& signId = MWBase::Environment::get().getWorld()->getPlayer().getBirthSign();
        if (!isInherent && !signId.empty())
        {
            const ESM::BirthSign* sign = MWBase::Environment::get().getESMStore()->get<ESM::BirthSign>().find(signId);
            isInherent = sign->mPowers.exists(spell->mId);
        }

        const auto windowManager = MWBase::Environment::get().getWindowManager();
        if (isInherent)
        {
            windowManager->messageBox("#{sDeleteSpellError}");
        }
        else
        {
            // ask for confirmation
            mSpellToDelete = spellId;
            ConfirmationDialog* dialog = windowManager->getConfirmationDialog();
            std::string question{ windowManager->getGameSettingString("sQuestionDeleteSpell", "Delete %s?") };
            question = Misc::StringUtils::format(question, spell->mName);
            dialog->askForConfirmation(question);
            dialog->setTooltipSourceWindow(this);
            dialog->eventOkClicked.clear();
            dialog->eventOkClicked += MyGUI::newDelegate(this, &SpellWindow::onDeleteSpellAccept);
            dialog->eventCancelClicked.clear();
        }
    }

    void SpellWindow::onModelIndexSelected(SpellModel::ModelIndex index)
    {
        const Spell& spell = mSpellView->getModel()->getItem(index);
        if (spell.mType == Spell::Type_EnchantedItem)
        {
            onEnchantedItemSelected(spell.mItem, spell.mActive);
        }
        else
        {
            if (MyGUI::InputManager::getInstance().isShiftPressed())
                askDeleteSpell(spell.mId);
            else
                onSpellSelected(spell.mId);
        }
    }

    void SpellWindow::onSpellSelected(const ESM::RefId& spellId)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWWorld::InventoryStore& store = player.getClass().getInventoryStore(player);
        store.setSelectedEnchantItem(store.end());
        MWBase::Environment::get().getWindowManager()->setSelectedSpell(
            spellId, int(MWMechanics::getSpellSuccessChance(spellId, player)));

        updateSpells();
    }

    void SpellWindow::onDeleteSpellAccept()
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        MWMechanics::Spells& spells = stats.getSpells();

        if (MWBase::Environment::get().getWindowManager()->getSelectedSpell() == mSpellToDelete)
            MWBase::Environment::get().getWindowManager()->unsetSelectedSpell();

        spells.remove(mSpellToDelete);

        updateSpells();
    }

    void SpellWindow::cycle(bool next)
    {
        MWWorld::Ptr player = MWMechanics::getPlayer();

        if (MWBase::Environment::get().getMechanicsManager()->isAttackingOrSpell(player))
            return;

        const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        if (stats.isParalyzed() || stats.getKnockedDown() || stats.isDead() || stats.getHitRecovery())
            return;

        mSpellView->setModel(new SpellModel(MWMechanics::getPlayer()));
        int itemCount = static_cast<int>(mSpellView->getModel()->getItemCount());
        if (itemCount == 0)
            return;

        SpellModel::ModelIndex nextIndex;
        SpellModel::ModelIndex currentIndex = mSpellView->getModel()->getSelectedIndex();

        // If we have a selected index, search for a valid selection in the target direction
        if (currentIndex >= 0)
        {
            MWWorld::ContainerStore store;
            const Spell& currentSpell = mSpellView->getModel()->getItem(currentIndex);

            nextIndex = currentIndex;
            for (int i = 0; i < itemCount; i++)
            {
                nextIndex += next ? 1 : -1;
                nextIndex = (nextIndex + itemCount) % itemCount;

                // We can keep this selection if:
                //   * we're not switching off of an enchanted item
                //   * we're not switching to an enchanted item
                //   * the next item wouldn't stack with the current item
                if (currentSpell.mType != Spell::Type_EnchantedItem)
                    break;

                const Spell& nextSpell = mSpellView->getModel()->getItem(nextIndex);
                if (nextSpell.mType != Spell::Type_EnchantedItem || !store.stacks(currentSpell.mItem, nextSpell.mItem))
                    break;
            }
        }
        // Otherwise, the first selection is always index 0
        else
            nextIndex = 0;

        // Only trigger the selection event if the selection is actually changing.
        // The itemCount check earlier ensures we have at least one spell to select.
        if (nextIndex != currentIndex)
        {
            const Spell& selectedSpell = mSpellView->getModel()->getItem(nextIndex);
            if (selectedSpell.mType == Spell::Type_EnchantedItem)
                onEnchantedItemSelected(selectedSpell.mItem, selectedSpell.mActive);
            else
                onSpellSelected(selectedSpell.mId);
        }
    }

    MyGUI::Widget* SpellWindow::getControllerFocusTooltipWidget() const
    {
        if (mEffectsFocusActive && mEffectsFocus < mEffectWidgets.size())
        {
            MyGUI::Widget* widget = mEffectWidgets[mEffectsFocus];
            if (widget && widget->getVisible())
                return widget;
        }

        return mSpellView ? mSpellView->getControllerFocusWidget() : nullptr;
    }

    MyGUI::Widget* SpellWindow::getEffectWidgetAt(const MyGUI::IntPoint& pos) const
    {
        for (MyGUI::Widget* widget : mEffectWidgets)
        {
            if (widget && widget->getVisible() && widget->getAbsoluteCoord().inside(pos))
                return widget;
        }

        return nullptr;
    }

    bool SpellWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
        }
        else
        {
            refreshEffectWidgets();
            const bool hasEffects = !mEffectWidgets.empty();

            if (mEffectsFocusActive)
            {
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
                {
                    moveEffectsFocus(-1);
                    return true;
                }
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
                {
                    moveEffectsFocus(1);
                    return true;
                }
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                {
                    setEffectsFocusActive(false);
                    return true;
                }
                if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_A)
                    return true;
            }
            else if (arg.button == SDL_CONTROLLER_BUTTON_X)
            {
                SpellModel* model = mSpellView ? mSpellView->getModel() : nullptr;
                const size_t focusIndex = mSpellView ? mSpellView->getControllerFocusIndex() : 0;
                if (model && focusIndex < model->getItemCount())
                {
                    const Spell& spell = model->getItem(static_cast<int>(focusIndex));
                    if (spell.mType != Spell::Type_EnchantedItem)
                    {
                        askDeleteSpell(spell.mId);
                        return true;
                    }
                }
            }
            else if (hasEffects && arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP
                && (mSpellView->getControllerButtonCount() == 0 || mSpellView->isControllerAtTop()))
            {
                setEffectsFocusActive(true);
                return true;
            }

            mSpellView->onControllerButton(arg.button);
        }

        return true;
    }

    void SpellWindow::setActiveControllerWindow(bool active)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->getMode() == MWGui::GM_Inventory)
        {
            MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            MyGUI::IntCoord coord = getFixedWindowCoord(viewSize);

            MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
            window->setCoord(coord.left, active ? coord.top : viewSize.height + 1, coord.width, coord.height);

            MWBase::Environment::get().getWindowManager()->setControllerTooltipVisible(
                active && Settings::gui().mControllerTooltips);
        }

        if (mEffectsFocusActive)
            mSpellView->setActiveControllerWindow(false);
        else
            mSpellView->setActiveControllerWindow(active);

        WindowBase::setActiveControllerWindow(active);
        updateEffectsHighlight();
    }

    void SpellWindow::refreshEffectWidgets()
    {
        for (MyGUI::Widget* widget : mEffectWidgets)
        {
            if (widget)
                MyGUI::Gui::getInstance().destroyWidget(widget);
        }
        mEffectWidgets.clear();
        mEffectIcons.clear();
        mEffectIconCoords.clear();

        if (mSpellIcons && mEffectBox)
        {
            std::vector<MyGUI::ImageBox*> icons;
            mSpellIcons->getVisibleWidgets(icons);

            mEffectWidgets.reserve(icons.size());
            mEffectIcons.reserve(icons.size());
            mEffectIconCoords.reserve(icons.size());
            for (MyGUI::ImageBox* icon : icons)
            {
                if (!icon || !icon->getVisible())
                    continue;

                MyGUI::IntCoord iconCoord = icon->getCoord();
                const int baseSize = 16;
                if (iconCoord.width != baseSize || iconCoord.height != baseSize)
                {
                    const int adjustX = (iconCoord.width - baseSize) / 2;
                    const int adjustY = (iconCoord.height - baseSize) / 2;
                    iconCoord.left += adjustX;
                    iconCoord.top += adjustY;
                    iconCoord.width = baseSize;
                    iconCoord.height = baseSize;
                    icon->setCoord(iconCoord);
                }
                MyGUI::Widget* hitbox = mEffectBox->createWidget<MyGUI::Widget>({}, iconCoord, MyGUI::Align::Default);
                hitbox->setNeedMouseFocus(true);
                hitbox->setNeedKeyFocus(true);
                hitbox->setDepth(icon->getDepth() + 1);
                hitbox->setAlpha(0.f);

                hitbox->setUserString("ToolTipType", "ActiveEffect");
                hitbox->setUserString("ActiveEffectName",
                    icon->isUserString("ActiveEffectName") ? icon->getUserString("ActiveEffectName") : "");
                hitbox->setUserString("ActiveEffectIcon",
                    icon->isUserString("ActiveEffectIcon") ? icon->getUserString("ActiveEffectIcon") : "");
                hitbox->setUserString("ActiveEffectDuration",
                    icon->isUserString("ActiveEffectDuration") ? icon->getUserString("ActiveEffectDuration") : "");
                hitbox->setUserString("ActiveEffectText",
                    icon->isUserString("ActiveEffectText") ? icon->getUserString("ActiveEffectText") : "");

                mEffectWidgets.push_back(hitbox);

                mEffectIcons.push_back(icon);
                mEffectIconCoords.push_back(iconCoord);
            }
        }

        if (mEffectsFocus >= mEffectWidgets.size())
            mEffectsFocus = mEffectWidgets.empty() ? 0 : mEffectWidgets.size() - 1;

        if (mEffectsFocusActive && mEffectWidgets.empty())
            setEffectsFocusActive(false);
    }

    void SpellWindow::setEffectsFocusActive(bool active)
    {
        if (mEffectsFocusActive == active)
            return;

        mEffectsFocusActive = active;

        if (active)
        {
            mPrevSpellFocus = mSpellView->getControllerFocusIndex();
            mSpellView->setActiveControllerWindow(false);
        }
        else
        {
            refreshEffectWidgets();
            mSpellView->setActiveControllerWindow(true);
            mSpellView->setControllerFocusIndex(mPrevSpellFocus);
        }

        updateEffectsHighlight();
    }

    void SpellWindow::updateEffectsHighlight()
    {
        if (mEffectIcons.empty())
            return;

        for (size_t i = 0; i < mEffectIcons.size(); ++i)
        {
            if (mEffectIcons[i])
                mEffectIcons[i]->setCoord(mEffectIconCoords[i]);
        }

        if (!mActiveControllerWindow || !mEffectsFocusActive || mEffectWidgets.empty())
            return;

        MyGUI::Widget* widget = mEffectWidgets[mEffectsFocus];
        if (!widget || !widget->getVisible())
            return;

        const size_t i = mEffectsFocus;
        if (i < mEffectIcons.size() && mEffectIcons[i])
        {
            const MyGUI::IntCoord baseCoord = mEffectIconCoords[i];
            const int focusSize = 18;
            const int growX = (focusSize - baseCoord.width) / 2;
            const int growY = (focusSize - baseCoord.height) / 2;
            const MyGUI::IntCoord focusCoord(baseCoord.left - growX, baseCoord.top - growY, focusSize, focusSize);
            mEffectIcons[i]->setCoord(focusCoord);
        }

        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getWindowManager()->restoreControllerTooltips();
    }

    bool SpellWindow::moveEffectsFocus(int delta)
    {
        if (mEffectWidgets.empty())
            return false;

        const size_t count = mEffectWidgets.size();
        const size_t prev = mEffectsFocus;
        if (delta < 0)
            mEffectsFocus = (mEffectsFocus + count - 1) % count;
        else if (delta > 0)
            mEffectsFocus = (mEffectsFocus + 1) % count;

        if (prev != mEffectsFocus)
        {
            updateEffectsHighlight();
        }

        return true;
    }

    void SpellWindow::warpToEffectWidget()
    {
        if (!mEffectsFocusActive || mEffectWidgets.empty())
            return;

        MyGUI::Widget* widget = mEffectWidgets[mEffectsFocus];
        if (!widget || !widget->getVisible())
            return;
    }

    MyGUI::IntCoord SpellWindow::getFixedWindowCoord(const MyGUI::IntSize& viewSize) const
    {
        const float scale = 0.85f;
        float width = viewSize.width * scale;
        float height = width * 10.f / 16.f;
        const float maxHeight = viewSize.height * scale;
        if (height > maxHeight)
        {
            height = maxHeight;
            width = height * 16.f / 10.f;
        }

        const int w = static_cast<int>(width);
        const int h = static_cast<int>(height);
        const int x = (viewSize.width - w) / 2;
        const int y = (viewSize.height - h) / 2;
        return { x, y, w, h };
    }
}
