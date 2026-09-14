#include "enchantingdialog.hpp"

#include <iomanip>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollView.h>
#include <MyGUI_UString.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <components/misc/strings/format.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/list.hpp>

#include <components/esm3/loadgmst.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"

#include "../mwmechanics/actorutil.hpp"

#include "itemselection.hpp"
#include "itemwidget.hpp"

#include "sortfilteritemmodel.hpp"

namespace
{
    constexpr size_t kEnchantmentNameLimit = 48;

    MyGUI::IntCoord getCenteredWindowCoord(const MyGUI::IntSize& viewSize, const MyGUI::IntSize& windowSize)
    {
        const int w = windowSize.width;
        const int h = windowSize.height;
        const int x = (viewSize.width - w) / 2;
        const int y = (viewSize.height - h) / 2;
        return { x, y, w, h };
    }
}

namespace MWGui
{

    EnchantingDialog::EnchantingDialog()
        : WindowBase("openmw_enchanting_dialog.layout")
        , EffectEditorBase(EffectEditorBase::Enchanting)
    {
        getWidget(mName, "NameEdit");
        getWidget(mCancelButton, "CancelButton");
        getWidget(mAvailableEffectsList, "AvailableEffects");
        getWidget(mUsedEffectsView, "UsedEffects");
        getWidget(mItemBox, "ItemBox");
        getWidget(mSoulBox, "SoulBox");
        getWidget(mEnchantmentPoints, "Enchantment");
        getWidget(mCastCost, "CastCost");
        getWidget(mCharge, "Charge");
        getWidget(mSuccessChance, "SuccessChance");
        getWidget(mChanceLayout, "ChanceLayout");
        getWidget(mTypeButton, "TypeButton");
        getWidget(mBuyButton, "BuyButton");
        getWidget(mPrice, "PriceLabel");
        getWidget(mPriceText, "PriceTextLabel");

        setWidgets(mAvailableEffectsList, mUsedEffectsView);

        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EnchantingDialog::onCancelButtonClicked);
        mItemBox->eventMouseButtonClick += MyGUI::newDelegate(this, &EnchantingDialog::onSelectItem);
        mSoulBox->eventMouseButtonClick += MyGUI::newDelegate(this, &EnchantingDialog::onSelectSoul);
        mBuyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EnchantingDialog::onBuyButtonClicked);
        mTypeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EnchantingDialog::onTypeButtonClicked);
        mName->setMaxTextLength(kEnchantmentNameLimit);
        mName->eventEditSelectAccept += MyGUI::newDelegate(this, &EnchantingDialog::onAccept);
        mName->eventEditTextChange += MyGUI::newDelegate(this, &EnchantingDialog::onNameEdited);
        mTypeButton->clearUserStrings();

        if (Settings::gui().mControllerMenus)
        {
            mName->setEditStatic(true);
            mName->setNeedKeyFocus(true);
            mItemBox->setNeedKeyFocus(true);
            mSoulBox->setNeedKeyFocus(true);
            mTypeButton->setNeedKeyFocus(true);

            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);
        }

        mControllerButtons = {};
        mControllerButtons.mA = "#{Interface:Select}";
        mControllerButtons.mX = "#{Interface:Buy}";
        mControllerButtons.mY = "#{Interface:Info}";
        mControllerButtons.mB = "#{Interface:Cancel}";
    }

    void EnchantingDialog::onOpen()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            window->setCoord(getCenteredWindowCoord(viewSize, window->getSize()));
        }
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mItemBox);
        else
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mName);
    }

    void EnchantingDialog::onFrame(float /*dt*/)
    {
        checkReferenceAvailable();

        if (!Settings::gui().mControllerMenus)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool keyboardVisible = winMgr->isVirtualKeyboardVisible();
        if (mKeyboardWasVisible && !keyboardVisible)
            setControllerFocusWidget(mName);
        mKeyboardWasVisible = keyboardVisible;

        if (mControllerEffectsFocusActive)
            setControllerEffectsFocus(true);

        updateControllerFocusHighlight();
        updateEditStaticState();
    }

    void EnchantingDialog::setSoulGem(const MWWorld::Ptr& gem)
    {
        if (gem.isEmpty())
        {
            mSoulBox->setItem(MWWorld::Ptr());
            mSoulBox->clearUserStrings();
            mEnchanting.setSoulGem(MWWorld::Ptr());
        }
        else
        {
            mSoulBox->setItem(gem);
            mSoulBox->setUserString("ToolTipType", "ItemPtr");
            mSoulBox->setUserData(MWWorld::Ptr(gem));
            mEnchanting.setSoulGem(gem);
        }
    }

    void EnchantingDialog::setItem(const MWWorld::Ptr& item)
    {
        if (item.isEmpty())
        {
            mItemBox->setItem(MWWorld::Ptr());
            mItemBox->clearUserStrings();
            mEnchanting.setOldItem(MWWorld::Ptr());
        }
        else
        {
            std::string_view name = item.getClass().getName(item);
            setNameCaption(MyGUI::UString(name));
            mItemBox->setItem(item);
            mItemBox->setUserString("ToolTipType", "ItemPtr");
            mItemBox->setUserData(MWWorld::Ptr(item));
            mEnchanting.setOldItem(item);
        }
    }

    void EnchantingDialog::updateLabels()
    {
        mEnchantmentPoints->setCaption(std::to_string(static_cast<int>(mEnchanting.getEnchantPoints(false))) + " / "
            + std::to_string(mEnchanting.getMaxEnchantValue()));
        mCharge->setCaption(std::to_string(mEnchanting.getGemCharge()));
        mSuccessChance->setCaption(std::to_string(std::clamp(mEnchanting.getEnchantChance(), 0, 100)));
        mCastCost->setCaption(std::to_string(mEnchanting.getEffectiveCastCost()));
        mPrice->setCaption(std::to_string(mEnchanting.getEnchantPrice()));

        switch (mEnchanting.getCastStyle())
        {
            case ESM::Enchantment::CastOnce:
                mTypeButton->setCaption(MyGUI::UString(
                    MWBase::Environment::get().getWindowManager()->getGameSettingString("sItemCastOnce", "Cast Once")));
                setConstantEffect(false);
                break;
            case ESM::Enchantment::WhenStrikes:
                mTypeButton->setCaption(
                    MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString(
                        "sItemCastWhenStrikes", "When Strikes")));
                setConstantEffect(false);
                break;
            case ESM::Enchantment::WhenUsed:
                mTypeButton->setCaption(
                    MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString(
                        "sItemCastWhenUsed", "When Used")));
                setConstantEffect(false);
                break;
            case ESM::Enchantment::ConstantEffect:
                mTypeButton->setCaption(
                    MyGUI::UString(MWBase::Environment::get().getWindowManager()->getGameSettingString(
                        "sItemCastConstant", "Cast Constant")));
                setConstantEffect(true);
                break;
        }
    }

    void EnchantingDialog::setPtr(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || (ptr.getType() != ESM::REC_MISC && !ptr.getClass().isActor()))
            throw std::runtime_error("Invalid argument in EnchantingDialog::setPtr");

        setNameCaption({});

        if (ptr.getClass().isActor())
        {
            mEnchanting.setSelfEnchanting(false);
            mEnchanting.setEnchanter(ptr);
            mBuyButton->setCaptionWithReplacing("#{sBuy}");
            mControllerButtons.mX = "#{Interface:Buy}";
            mChanceLayout->setVisible(false);
            mPtr = ptr;
            setSoulGem(MWWorld::Ptr());
            mPrice->setVisible(true);
            mPriceText->setVisible(true);
        }
        else
        {
            mEnchanting.setSelfEnchanting(true);
            mEnchanting.setEnchanter(MWMechanics::getPlayer());
            mBuyButton->setCaptionWithReplacing("#{sCreate}");
            mControllerButtons.mX = "#{Interface:Create}";
            mChanceLayout->setVisible(Settings::game().mShowEnchantChance);
            mPtr = MWMechanics::getPlayer();
            setSoulGem(ptr);
            mPrice->setVisible(false);
            mPriceText->setVisible(false);
        }

        setItem(MWWorld::Ptr());
        startEditing();
        updateLabels();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mName);
    }

    void EnchantingDialog::onReferenceUnavailable()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Enchanting);
        resetReference();
    }

    void EnchantingDialog::resetReference()
    {
        ReferenceInterface::resetReference();
        setItem(MWWorld::Ptr());
        setSoulGem(MWWorld::Ptr());
        mPtr = MWWorld::Ptr();
        mEnchanting.setEnchanter(MWWorld::Ptr());
    }

    void EnchantingDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Enchanting);
    }

    void EnchantingDialog::onSelectItem(MyGUI::Widget* /*sender*/)
    {
        if (mEnchanting.getOldItem().isEmpty())
        {
            mItemSelectionDialog = std::make_unique<ItemSelectionDialog>("#{sEnchantItems}");
            mItemSelectionDialog->eventItemSelected += MyGUI::newDelegate(this, &EnchantingDialog::onItemSelected);
            mItemSelectionDialog->eventDialogCanceled += MyGUI::newDelegate(this, &EnchantingDialog::onItemCancel);
            if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
            {
                const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
                const MyGUI::IntCoord dialogCoord = getCenteredWindowCoord(viewSize, window->getSize());
                mItemSelectionDialog->setCoord(
                    dialogCoord.left, dialogCoord.top, dialogCoord.width, dialogCoord.height);
            }
            mItemSelectionDialog->setVisible(true);
            mItemSelectionDialog->openContainer(MWMechanics::getPlayer());
            mItemSelectionDialog->setFilter(SortFilterItemModel::Filter_OnlyEnchantable);
            setVisibleNoStateChange(false);
        }
        else
        {
            setItem(MWWorld::Ptr());
            updateLabels();
        }
    }

    void EnchantingDialog::onItemSelected(MWWorld::Ptr item)
    {
        mItemSelectionDialog->setVisible(false);
        setVisibleNoStateChange(true);
        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();

        setItem(item);
        MWBase::Environment::get().getWindowManager()->playSound(item.getClass().getDownSoundId(item));
        mEnchanting.nextCastStyle();
        updateLabels();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mItemBox);
    }

    void EnchantingDialog::onItemCancel()
    {
        mItemSelectionDialog->setVisible(false);
        setVisibleNoStateChange(true);
        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mItemBox);
    }

    void EnchantingDialog::onSoulSelected(MWWorld::Ptr item)
    {
        mItemSelectionDialog->setVisible(false);
        setVisibleNoStateChange(true);
        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();

        mEnchanting.setSoulGem(item);
        if (mEnchanting.getGemCharge() == 0)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage32}");
            return;
        }

        setSoulGem(item);
        MWBase::Environment::get().getWindowManager()->playSound(item.getClass().getDownSoundId(item));
        updateLabels();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mSoulBox);
    }

    void EnchantingDialog::onSoulCancel()
    {
        mItemSelectionDialog->setVisible(false);
        setVisibleNoStateChange(true);
        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mSoulBox);
    }

    void EnchantingDialog::onSelectSoul(MyGUI::Widget* /*sender*/)
    {
        if (mEnchanting.getGem().isEmpty())
        {
            mItemSelectionDialog = std::make_unique<ItemSelectionDialog>("#{sSoulGemsWithSouls}");
            mItemSelectionDialog->eventItemSelected += MyGUI::newDelegate(this, &EnchantingDialog::onSoulSelected);
            mItemSelectionDialog->eventDialogCanceled += MyGUI::newDelegate(this, &EnchantingDialog::onSoulCancel);
            if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
            {
                const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
                const MyGUI::IntCoord dialogCoord = getCenteredWindowCoord(viewSize, window->getSize());
                mItemSelectionDialog->setCoord(
                    dialogCoord.left, dialogCoord.top, dialogCoord.width, dialogCoord.height);
            }
            mItemSelectionDialog->setVisible(true);
            mItemSelectionDialog->openContainer(MWMechanics::getPlayer());
            mItemSelectionDialog->setFilter(SortFilterItemModel::Filter_OnlyChargedSoulstones);
            setVisibleNoStateChange(false);

            // MWBase::Environment::get().getWindowManager()->messageBox("#{sInventorySelectNoSoul}");
        }
        else
        {
            setSoulGem(MWWorld::Ptr());
            mEnchanting.nextCastStyle();
            updateLabels();
            updateEffectsView();
        }
    }

    void EnchantingDialog::notifyEffectsChanged()
    {
        mEffectList.populate(mEffects);
        mEnchanting.setEffect(mEffectList);
        updateLabels();
    }

    void EnchantingDialog::onTypeButtonClicked(MyGUI::Widget* /*sender*/)
    {
        mEnchanting.nextCastStyle();
        updateLabels();
        updateEffectsView();
    }

    void EnchantingDialog::onAccept(MyGUI::EditBox* sender)
    {
        onBuyButtonClicked(sender);

        // To do not spam onAccept() again and again
        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    void EnchantingDialog::openVirtualKeyboard(MyGUI::EditBox* edit)
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

    void EnchantingDialog::setControllerFocusWidget(MyGUI::Widget* widget)
    {
        if (!widget)
            return;

        if (widget == mItemBox || widget == mSoulBox)
            mNameReturnFocus = widget;

        if (mControllerEffectsFocusActive)
            setControllerEffectsFocusActive(false);
        else
            setControllerEffectsFocus(false);
        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(widget);
        updateControllerFocusHighlight();
    }

    void EnchantingDialog::setControllerEffectsFocusActive(bool active)
    {
        if (mControllerEffectsFocusActive == active)
            return;

        mControllerEffectsFocusActive = active;
        setControllerEffectsFocus(active);

        if (active)
        {
            MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
            winMgr->setControllerTooltipVisible(Settings::gui().mControllerTooltips);
            winMgr->setCursorVisible(!winMgr->getControllerTooltipVisible());
        }

        updateControllerFocusHighlight();
    }

    void EnchantingDialog::updateControllerFocusHighlight()
    {
        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        if (mControllerEffectsFocusActive)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        const bool shouldHighlight = focus == mName || focus == mItemBox || focus == mSoulBox || focus == mTypeButton;
        if (!shouldHighlight)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        const MyGUI::IntCoord focusCoord = focus->getAbsoluteCoord();
        MyGUI::Widget* highlightParent = mControllerFocusHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        mControllerFocusHighlight->setCoord(
            focusCoord.left - baseCoord.left, focusCoord.top - baseCoord.top, focusCoord.width, focusCoord.height);
        mControllerFocusHighlight->setVisible(true);
    }

    void EnchantingDialog::updateEditStaticState()
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool shouldStatic = !winMgr->isVirtualKeyboardVisible();
        if (mName->getEditStatic() != shouldStatic)
            mName->setEditStatic(shouldStatic);
    }

    void EnchantingDialog::onNameEdited(MyGUI::EditBox* sender)
    {
        const size_t length = sender->getCaption().size();
        if (mSuppressNameLimitMessage)
        {
            mNameHitLimit = length >= kEnchantmentNameLimit;
            return;
        }

        if (length < kEnchantmentNameLimit)
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

    void EnchantingDialog::setNameCaption(const MyGUI::UString& caption)
    {
        mSuppressNameLimitMessage = true;
        mName->setCaption(caption);
        mSuppressNameLimitMessage = false;
        mNameHitLimit = caption.size() >= kEnchantmentNameLimit;
    }

    MyGUI::Widget* EnchantingDialog::getControllerFocusTooltipWidget() const
    {
        if (mControllerEffectsFocusActive)
            return getControllerEffectsTooltipWidget();

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (focus == mName || focus == mItemBox || focus == mSoulBox || focus == mTypeButton)
            return focus;

        return nullptr;
    }

    void EnchantingDialog::onBuyButtonClicked(MyGUI::Widget* /*sender*/)
    {
        if (mEffects.size() <= 0)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sEnchantmentMenu11}");
            return;
        }

        if (mName->getCaption().empty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage10}");
            return;
        }

        if (mEnchanting.soulEmpty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage52}");
            return;
        }

        if (mEnchanting.itemEmpty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage11}");
            return;
        }

        if (static_cast<int>(mEnchanting.getEnchantPoints(false)) > mEnchanting.getMaxEnchantValue())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage29}");
            return;
        }

        mEnchanting.setNewItemName(mName->getCaption());
        mEnchanting.setEffect(mEffectList);

        MWWorld::Ptr player = MWMechanics::getPlayer();
        int playerGold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);
        if (mPtr != player && mEnchanting.getEnchantPrice() > playerGold)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage18}");
            return;
        }

        // check if the player is attempting to use a soulstone or item that was stolen from this actor
        if (mPtr != player)
        {
            for (int i = 0; i < 2; ++i)
            {
                MWWorld::Ptr item = (i == 0) ? mEnchanting.getOldItem() : mEnchanting.getGem();
                if (MWBase::Environment::get().getMechanicsManager()->isItemStolenFrom(
                        item.getCellRef().getRefId(), mPtr))
                {
                    std::string msg = MWBase::Environment::get()
                                          .getESMStore()
                                          ->get<ESM::GameSetting>()
                                          .find("sNotifyMessage49")
                                          ->mValue.getString();
                    msg = Misc::StringUtils::format(msg, item.getClass().getName(item));
                    MWBase::Environment::get().getWindowManager()->messageBox(msg);

                    MWBase::Environment::get().getMechanicsManager()->confiscateStolenItemToOwner(
                        player, item, mPtr, 1);

                    MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Enchanting);
                    MWBase::Environment::get().getWindowManager()->exitCurrentGuiMode();
                    return;
                }
            }
        }

        if (mEnchanting.create())
        {
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("enchant success"));
            MWBase::Environment::get().getWindowManager()->messageBox("#{sEnchantmentMenu12}");
            MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Enchanting);
        }
        else
        {
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("enchant fail"));
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage34}");
            if (!mEnchanting.getGem().isEmpty() && !mEnchanting.getGem().getCellRef().getCount())
            {
                setSoulGem(MWWorld::Ptr());
                mEnchanting.nextCastStyle();
                updateLabels();
                updateEffectsView();
            }
        }
    }

    bool EnchantingDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();

        if (mControllerEffectsFocusActive)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP && isControllerEffectsAtTop())
            {
                setControllerEffectsFocusActive(false);
                if (isControllerEffectsRightColumn())
                {
                    mTypeReturnToEffects = true;
                    mTypeReturnEffectsRightColumn = true;
                    mTypeReturnFocus = nullptr;
                    setControllerFocusWidget(mTypeButton);
                }
                else
                    setControllerFocusWidget(mItemBox);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && mEffects.empty())
            {
                setControllerEffectsFocusActive(false);
                mTypeReturnToEffects = true;
                mTypeReturnEffectsRightColumn = false;
                mTypeReturnFocus = nullptr;
                setControllerFocusWidget(mTypeButton);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP || arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN
                || arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT || arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT
                || arg.button == SDL_CONTROLLER_BUTTON_A)
                return EffectEditorBase::onControllerButtonEvent(arg);
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (focus == mName)
            {
                openVirtualKeyboard(mName);
                return true;
            }
            if (focus == mItemBox)
            {
                onSelectItem(mItemBox);
                return true;
            }
            if (focus == mSoulBox)
            {
                onSelectSoul(mSoulBox);
                return true;
            }
            if (focus == mTypeButton)
            {
                onTypeButtonClicked(mTypeButton);
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (focus == mName)
            {
                MyGUI::Widget* target = mNameReturnFocus ? mNameReturnFocus : mItemBox;
                setControllerFocusWidget(target);
                return true;
            }
            if (focus == mItemBox || focus == mSoulBox || focus == mTypeButton)
            {
                if (focus == mTypeButton)
                {
                    if (mEffects.empty())
                        return true;
                    setControllerEffectsRightColumn(true);
                }
                else
                {
                    setControllerEffectsRightColumn(false);
                }
                setControllerEffectsFocusActive(true);
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (focus == mItemBox || focus == mSoulBox)
            {
                mNameReturnFocus = focus;
                setControllerFocusWidget(mName);
                return true;
            }
            if (focus == mTypeButton)
                return true;
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (focus == mSoulBox)
            {
                setControllerFocusWidget(mItemBox);
                return true;
            }
            if (focus == mTypeButton)
            {
                if (mEffects.empty() && mTypeReturnToEffects)
                {
                    mTypeReturnToEffects = false;
                    setControllerEffectsRightColumn(mTypeReturnEffectsRightColumn);
                    setControllerEffectsFocusActive(true);
                    return true;
                }
                if (mEffects.empty() && mTypeReturnFocus)
                {
                    setControllerFocusWidget(mTypeReturnFocus);
                    return true;
                }
                setControllerFocusWidget(mSoulBox);
                return true;
            }
            if (focus == mItemBox)
            {
                setControllerFocusWidget(mName);
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (focus == mName)
            {
                mTypeReturnToEffects = false;
                mTypeReturnFocus = mName;
                setControllerFocusWidget(mTypeButton);
                return true;
            }
            if (focus == mItemBox)
            {
                setControllerFocusWidget(mSoulBox);
                return true;
            }
            if (focus == mSoulBox)
            {
                mTypeReturnToEffects = false;
                mTypeReturnFocus = mSoulBox;
                setControllerFocusWidget(mTypeButton);
                return true;
            }
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            if (!mName->getCaption().empty())
            {
                mName->setCaption({});
                return true;
            }
            onCancelButtonClicked(mCancelButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
            onBuyButtonClicked(mBuyButton);
        return true;
    }
}
