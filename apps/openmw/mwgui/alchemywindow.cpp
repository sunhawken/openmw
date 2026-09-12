#include "alchemywindow.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <MyGUI_Button.h>
#include <MyGUI_ComboBox.h>
#include <MyGUI_ControllerManager.h>
#include <MyGUI_ControllerRepeatClick.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_ListBox.h>
#include <MyGUI_UString.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <components/esm3/loadappa.hpp>
#include <components/esm3/loadingr.hpp>
#include <components/esm3/loadmgef.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/alchemy.hpp"
#include "../mwmechanics/magiceffects.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"

#include <MyGUI_Macros.h>

#include "countdialog.hpp"
#include "inventoryitemmodel.hpp"
#include "itemview.hpp"
#include "itemwidget.hpp"
#include "sortfilteritemmodel.hpp"
#include "widgets.hpp"

namespace
{
    constexpr size_t kAlchemyNameLimit = 48;
    constexpr int kAlchemyWindowFixedWidth = 588;
    constexpr int kAlchemyWindowFixedHeight = 394;

    bool isChildOf(MyGUI::Widget* widget, MyGUI::Widget* parent)
    {
        for (MyGUI::Widget* current = widget; current; current = current->getParent())
        {
            if (current == parent)
                return true;
        }
        return false;
    }

    void enforceFixedSize(MyGUI::Widget* mainWidget)
    {
        if (!mainWidget)
            return;

        const MyGUI::IntSize sz = mainWidget->getSize();
        if (sz.width != kAlchemyWindowFixedWidth || sz.height != kAlchemyWindowFixedHeight)
            mainWidget->setSize(kAlchemyWindowFixedWidth, kAlchemyWindowFixedHeight);
    }
}

namespace MWGui
{
    AlchemyWindow::AlchemyWindow()
        : WindowBase(
            Settings::gui().mXboxAlchemyUi.get() ? "openmw_alchemy_window_xbox.layout" : "openmw_alchemy_window.layout")
        , mCurrentFilter(FilterType::ByName)
        , mUseXboxAlchemyUi(Settings::gui().mXboxAlchemyUi)
        , mModel(nullptr)
        , mSortModel(nullptr)
        , mAlchemy(std::make_unique<MWMechanics::Alchemy>())
        , mApparatus(4)
        , mIngredients(4)
    {
        getWidget(mCreateButton, "CreateButton");
        getWidget(mCancelButton, "CancelButton");
        getWidget(mIngredients[0], "Ingredient1");
        getWidget(mIngredients[1], "Ingredient2");
        getWidget(mIngredients[2], "Ingredient3");
        getWidget(mIngredients[3], "Ingredient4");
        getWidget(mApparatus[0], "Apparatus1");
        getWidget(mApparatus[1], "Apparatus2");
        getWidget(mApparatus[2], "Apparatus3");
        getWidget(mApparatus[3], "Apparatus4");
        getWidget(mEffectsBox, "CreatedEffects");
        getWidget(mBrewCountEdit, "BrewCount");
        getWidget(mIncreaseButton, "IncreaseButton");
        getWidget(mDecreaseButton, "DecreaseButton");
        getWidget(mNameEdit, "NameEdit");
        getWidget(mItemView, "ItemView");
        getWidget(mFilterValue, "FilterValue");
        getWidget(mFilterType, "FilterType");
        if (mUseXboxAlchemyUi)
        {
            getWidget(mXboxSummaryPanel, "XboxSummaryPanel");
            getWidget(mXboxIngredientPickerPanel, "XboxIngredientPickerPanel");
        }

        mBrewCountEdit->eventValueChanged += MyGUI::newDelegate(this, &AlchemyWindow::onCountValueChanged);
        mBrewCountEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AlchemyWindow::onAccept);
        mBrewCountEdit->setMinValue(1);
        mBrewCountEdit->setValue(1);

        mIncreaseButton->eventMouseButtonPressed += MyGUI::newDelegate(this, &AlchemyWindow::onIncreaseButtonPressed);
        mIncreaseButton->eventMouseButtonReleased += MyGUI::newDelegate(this, &AlchemyWindow::onCountButtonReleased);
        mDecreaseButton->eventMouseButtonPressed += MyGUI::newDelegate(this, &AlchemyWindow::onDecreaseButtonPressed);
        mDecreaseButton->eventMouseButtonReleased += MyGUI::newDelegate(this, &AlchemyWindow::onCountButtonReleased);

        mItemView->eventItemClicked += MyGUI::newDelegate(this, &AlchemyWindow::onSelectedItem);

        mIngredients[0]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onIngredientSelected);
        mIngredients[1]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onIngredientSelected);
        mIngredients[2]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onIngredientSelected);
        mIngredients[3]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onIngredientSelected);

        mApparatus[0]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onApparatusSelected);
        mApparatus[1]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onApparatusSelected);
        mApparatus[2]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onApparatusSelected);
        mApparatus[3]->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onApparatusSelected);

        mCreateButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onCreateButtonClicked);
        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::onCancelButtonClicked);

        mNameEdit->setMaxTextLength(kAlchemyNameLimit);
        mNameEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &AlchemyWindow::onAccept);
        mNameEdit->eventEditTextChange += MyGUI::newDelegate(this, &AlchemyWindow::onNameEdited);
        mFilterValue->eventComboChangePosition += MyGUI::newDelegate(this, &AlchemyWindow::onFilterChanged);
        mFilterValue->eventEditTextChange += MyGUI::newDelegate(this, &AlchemyWindow::onFilterEdited);
        mFilterType->eventMouseButtonClick += MyGUI::newDelegate(this, &AlchemyWindow::switchFilterType);

        if (Settings::gui().mControllerMenus)
        {
            mNameEdit->setEditStatic(true);
            mBrewCountEdit->setEditStatic(true);
            mNameEdit->setNeedKeyFocus(true);
            mBrewCountEdit->setNeedKeyFocus(true);
            mFilterType->setNeedKeyFocus(true);
            mFilterValue->setNeedKeyFocus(true);
            mFilterValue->setEditStatic(true);
            mItemView->setNeedKeyFocus(true);
            if (mUseXboxAlchemyUi)
            {
                for (ItemWidget* ingredient : mIngredients)
                    ingredient->setNeedKeyFocus(true);
            }

            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);

            updateControllerButtons();
        }

        if (!mUseXboxAlchemyUi)
            enforceFixedSize(mMainWidget);

        center();
    }

    void AlchemyWindow::onAccept(MyGUI::EditBox* sender)
    {
        onCreateButtonClicked(sender);

        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    void AlchemyWindow::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Alchemy);
    }

    void AlchemyWindow::onCreateButtonClicked(MyGUI::Widget* /*sender*/)
    {
        mAlchemy->setPotionName(mNameEdit->getCaption());
        int count = mAlchemy->countPotionsToBrew();
        count = std::min(count, mBrewCountEdit->getValue());
        createPotions(count);
    }

    void AlchemyWindow::createPotions(int count)
    {
        MWMechanics::Alchemy::Result result = mAlchemy->create(mNameEdit->getCaption(), count);
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        switch (result)
        {
            case MWMechanics::Alchemy::Result_NoName:
                winMgr->messageBox("#{sNotifyMessage37}");
                break;
            case MWMechanics::Alchemy::Result_NoMortarAndPestle:
                winMgr->messageBox("#{sNotifyMessage45}");
                break;
            case MWMechanics::Alchemy::Result_LessThanTwoIngredients:
                winMgr->messageBox("#{sNotifyMessage6a}");
                break;
            case MWMechanics::Alchemy::Result_Success:
                winMgr->playSound(ESM::RefId::stringRefId("potion success"));
                if (count == 1)
                    winMgr->messageBox("#{sPotionSuccess}");
                else
                    winMgr->messageBox(
                        "#{sPotionSuccess} " + mNameEdit->getCaption().asUTF8() + " (" + std::to_string(count) + ")");
                break;
            case MWMechanics::Alchemy::Result_NoEffects:
            case MWMechanics::Alchemy::Result_RandomFailure:
                winMgr->messageBox("#{sNotifyMessage8}");
                winMgr->playSound(ESM::RefId::stringRefId("potion fail"));
                break;
        }

        for (size_t i = 0; i < mIngredients.size(); ++i)
            if (mIngredients[i]->isUserString("ToolTipType"))
            {
                MWWorld::Ptr ingred = *mIngredients[i]->getUserData<MWWorld::Ptr>();
                if (ingred.getCellRef().getCount() == 0)
                    mAlchemy->removeIngredient(i);
            }

        const size_t selectedIndex = mFilterValue->getIndexSelected();
        std::string selectedFilter;
        if (selectedIndex != MyGUI::ITEM_NONE && selectedIndex < mFilterValue->getItemCount())
            selectedFilter = mFilterValue->getItemNameAt(selectedIndex);

        updateFilters();
        if (!selectedFilter.empty())
        {
            const size_t restoreIndex = mFilterValue->findItemIndexWith(selectedFilter);
            if (restoreIndex != MyGUI::ITEM_NONE)
                mFilterValue->setIndexSelected(restoreIndex);
        }
        update();
        if (mUseXboxAlchemyUi)
            switchXboxPage(XboxPage::Summary);
    }

    void AlchemyWindow::initFilter()
    {
        auto const& wm = MWBase::Environment::get().getWindowManager();
        std::string_view ingredient = wm->getGameSettingString("sIngredients", "Ingredients");

        if (mFilterType->getCaption() == ingredient)
        {
            if (Settings::gui().mControllerMenus)
                switchFilterType(mFilterType);
            else
                mCurrentFilter = FilterType::ByName;
        }
        else
            mCurrentFilter = FilterType::ByEffect;
        updateFilters();
        mFilterValue->clearIndexSelected();
        updateFilters();
    }

    void AlchemyWindow::onFrame(float /*duration*/)
    {
        if (!mUseXboxAlchemyUi)
            enforceFixedSize(mMainWidget);

        if (!Settings::gui().mControllerMenus)
            return;

        updateControllerFocusState();
        updateControllerFocusHighlight();
        updateXboxIngredientFocus();
        updateEditStaticState();
    }

    void AlchemyWindow::switchFilterType(MyGUI::Widget* sender)
    {
        auto const& wm = MWBase::Environment::get().getWindowManager();
        std::string_view ingredient = wm->getGameSettingString("sIngredients", "Ingredients");
        auto* button = sender->castType<MyGUI::Button>();

        if (button->getCaption() == ingredient)
        {
            button->setCaption(MyGUI::UString(wm->getGameSettingString("sMagicEffects", "Magic Effects")));
            mCurrentFilter = FilterType::ByEffect;
        }
        else
        {
            button->setCaption(MyGUI::UString(ingredient));
            mCurrentFilter = FilterType::ByName;
        }
        mSortModel->setNameFilter({});
        mSortModel->setEffectFilter({});
        mFilterValue->clearIndexSelected();
        updateFilters();
        mItemView->update();
    }

    void AlchemyWindow::updateFilters()
    {
        std::set<std::string> itemNames, itemEffects;
        for (size_t i = 0; i < mModel->getItemCount(); ++i)
        {
            MWWorld::Ptr item = mModel->getItem(static_cast<ItemModel::ModelIndex>(i)).mBase;
            if (item.getType() != ESM::Ingredient::sRecordId)
                continue;

            itemNames.emplace(item.getClass().getName(item));

            MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            auto const alchemySkill = player.getClass().getSkill(player, ESM::Skill::Alchemy);

            auto const effects = MWMechanics::Alchemy::effectsDescription(item, alchemySkill);
            itemEffects.insert(effects.begin(), effects.end());
        }

        mFilterValue->removeAllItems();
        auto const addItems = [&](auto const& container) {
            for (auto const& item : container)
                mFilterValue->addItem(item);
        };
        switch (mCurrentFilter)
        {
            case FilterType::ByName:
                addItems(itemNames);
                break;
            case FilterType::ByEffect:
                addItems(itemEffects);
                break;
        }
    }

    void AlchemyWindow::applyFilter(const std::string& filter)
    {
        switch (mCurrentFilter)
        {
            case FilterType::ByName:
                mSortModel->setNameFilter(filter);
                break;
            case FilterType::ByEffect:
                mSortModel->setEffectFilter(filter);
                break;
        }
        mItemView->update();
    }

    void AlchemyWindow::onFilterChanged(MyGUI::ComboBox* sender, size_t index)
    {
        if (index != MyGUI::ITEM_NONE)
            applyFilter(sender->getItemNameAt(index));
    }

    void AlchemyWindow::onFilterEdited(MyGUI::EditBox* sender)
    {
        applyFilter(sender->getCaption());
    }

    void AlchemyWindow::onOpen()
    {
        if (!mUseXboxAlchemyUi)
            enforceFixedSize(mMainWidget);

        if (Settings::gui().mControllerMenus)
            center();

        mAlchemy->clear();
        mAlchemy->setAlchemist(MWMechanics::getPlayer());

        auto model = std::make_unique<InventoryItemModel>(MWMechanics::getPlayer());
        mModel = model.get();
        auto sortModel = std::make_unique<SortFilterItemModel>(std::move(model));
        mSortModel = sortModel.get();
        mSortModel->setFilter(SortFilterItemModel::Filter_OnlyIngredients);
        mItemView->setModel(std::move(sortModel));
        mItemView->resetScrollBars();

        setNameCaption({});
        mBrewCountEdit->setValue(1);

        size_t index = 0;
        for (auto iter = mAlchemy->beginTools(); iter != mAlchemy->endTools() && index < mApparatus.size();
             ++iter, ++index)
        {
            const auto& widget = mApparatus[index];
            widget->setItem(*iter);
            widget->clearUserStrings();
            if (!iter->isEmpty())
            {
                widget->setUserString("ToolTipType", "ItemPtr");
                widget->setUserData(MWWorld::Ptr(*iter));
            }
        }

        update();
        initFilter();

        if (mUseXboxAlchemyUi)
        {
            mXboxFocusedIngredient = 0;
            mXboxSelectedIngredientSlot = 0;
            switchXboxPage(XboxPage::Summary);
        }

        if (Settings::gui().mControllerMenus)
        {
            if (mUseXboxAlchemyUi)
                setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
            else if (mItemView->getItemCount() > 0)
                setControllerFocusWidget(mItemView);
            else
                setControllerFocusWidget(mNameEdit);
        }
        else
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mNameEdit);
    }

    void AlchemyWindow::onIngredientSelected(MyGUI::Widget* sender)
    {
        size_t i = std::distance(mIngredients.begin(), std::find(mIngredients.begin(), mIngredients.end(), sender));
        if (mUseXboxAlchemyUi)
            openXboxIngredientPicker(i);
        else
        {
            mAlchemy->removeIngredient(i);
            update();
        }
    }

    void AlchemyWindow::onItemSelected(MWWorld::Ptr item)
    {
        mItemSelectionDialog->setVisible(false);

        int32_t index = item.get<ESM::Apparatus>()->mBase->mData.mType;
        const auto& widget = mApparatus[index];

        widget->setItem(item);

        if (item.isEmpty())
        {
            widget->clearUserStrings();
            return;
        }

        mAlchemy->addApparatus(item);

        widget->setUserString("ToolTipType", "ItemPtr");
        widget->setUserData(MWWorld::Ptr(item));

        MWBase::Environment::get().getWindowManager()->playSound(item.getClass().getDownSoundId(item));
        update();
    }

    void AlchemyWindow::onItemCancel()
    {
        mItemSelectionDialog->setVisible(false);
    }

    void AlchemyWindow::onApparatusSelected(MyGUI::Widget* sender)
    {
        size_t i = std::distance(mApparatus.begin(), std::find(mApparatus.begin(), mApparatus.end(), sender));
        if (sender->getUserData<MWWorld::Ptr>()->isEmpty())
        {
            std::string title;
            switch (i)
            {
                case ESM::Apparatus::AppaType::MortarPestle:
                    title = "#{sMortar}";
                    break;
                case ESM::Apparatus::AppaType::Alembic:
                    title = "#{sAlembic}";
                    break;
                case ESM::Apparatus::AppaType::Calcinator:
                    title = "#{sCalcinator}";
                    break;
                case ESM::Apparatus::AppaType::Retort:
                    title = "#{sRetort}";
                    break;
                default:
                    title = "#{sApparatus}";
            }

            mItemSelectionDialog = std::make_unique<ItemSelectionDialog>(title);
            mItemSelectionDialog->eventItemSelected += MyGUI::newDelegate(this, &AlchemyWindow::onItemSelected);
            mItemSelectionDialog->eventDialogCanceled += MyGUI::newDelegate(this, &AlchemyWindow::onItemCancel);
            mItemSelectionDialog->setVisible(true);
            mItemSelectionDialog->openContainer(MWMechanics::getPlayer());
            mItemSelectionDialog->getSortModel()->setApparatusTypeFilter(static_cast<int32_t>(i));
            mItemSelectionDialog->setFilter(SortFilterItemModel::Filter_OnlyAlchemyTools);
        }
        else
        {
            const auto& widget = mApparatus[i];
            mAlchemy->removeApparatus(i);

            if (widget->getChildCount())
                MyGUI::Gui::getInstance().destroyWidget(widget->getChildAt(0));

            widget->clearUserStrings();
            widget->setItem(MWWorld::Ptr());
            widget->setUserData(MWWorld::Ptr());
        }

        update();
    }

    void AlchemyWindow::onSelectedItem(int index)
    {
        MWWorld::Ptr item = mSortModel->getItem(index).mBase;
        const int res = mUseXboxAlchemyUi ? mAlchemy->setIngredient(mXboxSelectedIngredientSlot, item)
                                          : mAlchemy->addIngredient(item);

        if (res != -1)
        {
            update();
            if (mUseXboxAlchemyUi)
            {
                mXboxFocusedIngredient = mXboxSelectedIngredientSlot;
                switchXboxPage(XboxPage::Summary);
                if (Settings::gui().mControllerMenus)
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
            }

            const ESM::RefId& sound = item.getClass().getUpSoundId(item);
            MWBase::Environment::get().getWindowManager()->playSound(sound);
        }
    }

    void AlchemyWindow::update()
    {
        std::string suggestedName = mAlchemy->suggestPotionName();
        if (suggestedName != mSuggestedPotionName)
        {
            setNameCaptionWithReplacing(suggestedName);
            mSuggestedPotionName = std::move(suggestedName);
        }

        mSortModel->clearDragItems();

        MWMechanics::Alchemy::TIngredientsIterator it = mAlchemy->beginIngredients();
        for (int i = 0; i < 4; ++i)
        {
            ItemWidget* ingredient = mIngredients[i];

            MWWorld::Ptr item;
            if (it != mAlchemy->endIngredients())
            {
                item = *it;
                ++it;
            }

            if (!item.isEmpty())
                mSortModel->addDragItem(item, item.getCellRef().getCount());

            if (ingredient->getChildCount())
                MyGUI::Gui::getInstance().destroyWidget(ingredient->getChildAt(0));

            ingredient->clearUserStrings();

            ingredient->setItem(item);

            if (item.isEmpty())
                continue;

            ingredient->setUserString("ToolTipType", "ItemPtr");
            ingredient->setUserData(MWWorld::Ptr(item));

            ingredient->setCount(item.getCellRef().getCount());
        }

        mItemView->update();

        std::vector<MWMechanics::EffectKey> effectIds = mAlchemy->listEffects();
        Widgets::SpellEffectList list;
        unsigned int effectIndex = 0;
        for (const MWMechanics::EffectKey& effectKey : effectIds)
        {
            Widgets::SpellEffectParams params;
            params.mEffectID = effectKey.mId;
            const ESM::MagicEffect* magicEffect
                = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(effectKey.mId);
            if (magicEffect->mData.mFlags & ESM::MagicEffect::TargetSkill)
                params.mSkill = effectKey.mArg;
            else if (magicEffect->mData.mFlags & ESM::MagicEffect::TargetAttribute)
                params.mAttribute = effectKey.mArg;
            params.mIsConstant = true;
            params.mNoTarget = true;
            params.mNoMagnitude = true;

            params.mKnown = mAlchemy->knownEffect(effectIndex, MWBase::Environment::get().getWorld()->getPlayerPtr());

            list.push_back(params);
            ++effectIndex;
        }

        while (mEffectsBox->getChildCount())
            MyGUI::Gui::getInstance().destroyWidget(mEffectsBox->getChildAt(0));

        MyGUI::IntCoord coord(0, 0, mEffectsBox->getWidth(), 24);
        Widgets::MWEffectListPtr effectsWidget = mEffectsBox->createWidget<Widgets::MWEffectList>(
            "MW_StatName", coord, MyGUI::Align::Left | MyGUI::Align::Top);

        effectsWidget->setEffectList(list);

        std::vector<MyGUI::Widget*> effectItems;
        effectsWidget->createEffectWidgets(effectItems, mEffectsBox, coord, false, 0);
        effectsWidget->setCoord(coord);
    }

    void AlchemyWindow::addRepeatController(MyGUI::Widget* widget)
    {
        MyGUI::ControllerItem* item
            = MyGUI::ControllerManager::getInstance().createItem(MyGUI::ControllerRepeatClick::getClassTypeName());
        MyGUI::ControllerRepeatClick* controller = static_cast<MyGUI::ControllerRepeatClick*>(item);
        controller->eventRepeatClick += newDelegate(this, &AlchemyWindow::onRepeatClick);
        MyGUI::ControllerManager::getInstance().addItem(widget, controller);
    }

    void AlchemyWindow::onIncreaseButtonPressed(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        addRepeatController(sender);
        onIncreaseButtonTriggered();
    }

    void AlchemyWindow::onDecreaseButtonPressed(MyGUI::Widget* sender, int left, int top, MyGUI::MouseButton id)
    {
        addRepeatController(sender);
        onDecreaseButtonTriggered();
    }

    void AlchemyWindow::onRepeatClick(MyGUI::Widget* widget, MyGUI::ControllerItem* /*controller*/)
    {
        if (widget == mIncreaseButton)
            onIncreaseButtonTriggered();
        else if (widget == mDecreaseButton)
            onDecreaseButtonTriggered();
    }

    void AlchemyWindow::onCountButtonReleased(
        MyGUI::Widget* sender, int /*left*/, int /*top*/, MyGUI::MouseButton /*id*/)
    {
        MyGUI::ControllerManager::getInstance().removeItem(sender);
    }

    void AlchemyWindow::onCountValueChanged(int value)
    {
        mBrewCountEdit->setValue(std::abs(value));
    }

    void AlchemyWindow::onIncreaseButtonTriggered()
    {
        int currentCount = mBrewCountEdit->getValue();

        if (currentCount == std::numeric_limits<int>::max())
            return;

        mBrewCountEdit->setValue(currentCount + 1);
    }

    void AlchemyWindow::onDecreaseButtonTriggered()
    {
        int currentCount = mBrewCountEdit->getValue();
        if (currentCount > 1)
            mBrewCountEdit->setValue(currentCount - 1);
    }

    void AlchemyWindow::onBrewCountSelected(MyGUI::Widget* /*sender*/, std::size_t count)
    {
        mBrewCountEdit->setValue(static_cast<int>(count));
    }

    void AlchemyWindow::filterListButtonHandler(const SDL_ControllerButtonEvent& arg)
    {
        const bool hasItems = mItemView && mItemView->getItemCount() > 0;
        MyGUI::Widget* postListFocus = hasItems
            ? static_cast<MyGUI::Widget*>(mItemView)
            : (mUseXboxAlchemyUi ? static_cast<MyGUI::Widget*>(mFilterType) : static_cast<MyGUI::Widget*>(mNameEdit));
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            size_t index = mFilterValue->getIndexSelected();
            mFilterValue->setIndexSelected(index);
            onFilterChanged(mFilterValue, index);
            setControllerFocusWidget(postListFocus);

            winMgr->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_Y && mUseXboxAlchemyUi)
        {
            showControllerInfo();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            mFilterValue->clearIndexSelected();
            onFilterEdited(mFilterValue);
            setControllerFocusWidget(postListFocus);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowUp, 0, false);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            winMgr->injectKeyPress(MyGUI::KeyCode::ArrowDown, 0, false);
        }
    }

    MyGUI::ListBox* AlchemyWindow::getFilterListBox(MyGUI::Widget* focus) const
    {
        for (MyGUI::Widget* current = focus; current != nullptr; current = current->getParent())
        {
            if (auto* list = current->castType<MyGUI::ListBox>(false))
                return list;
            if (current == mFilterValue)
                break;
        }
        return nullptr;
    }

    void AlchemyWindow::ensureFilterListHighlight(MyGUI::ListBox* list)
    {
        if (!list)
            return;

        if (mFilterList != list)
        {
            mFilterList = list;
            if (mFilterListHighlight)
            {
                MyGUI::Gui::getInstance().destroyWidget(mFilterListHighlight);
                mFilterListHighlight = nullptr;
            }
        }

        if (!mFilterListHighlight)
        {
            if (MyGUI::Widget* client = list->getClientWidget())
            {
                mFilterListHighlight = client->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
                mFilterListHighlight->setNeedMouseFocus(false);
                mFilterListHighlight->setDepth(1);
                mFilterListHighlight->setVisible(false);
            }
        }
    }

    void AlchemyWindow::updateFilterListHighlight(MyGUI::ListBox* list)
    {
        if (!list)
            return;

        ensureFilterListHighlight(list);
        updateControllerListHighlight(list, mFilterListHighlight);
    }

    void AlchemyWindow::openVirtualKeyboard(MyGUI::EditBox* edit)
    {
        if (!edit)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (winMgr->isVirtualKeyboardVisible())
            return;

        edit->setEditStatic(false);
        mLastKeyboardEdit = edit;
        winMgr->setKeyFocusWidget(edit);
        winMgr->toggleVirtualKeyboard();
    }

    void AlchemyWindow::updateControllerButtons()
    {
        if (!Settings::gui().mControllerMenus)
            return;

        mControllerButtons = {};
        if (mUseXboxAlchemyUi)
        {
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mX = "#{Interface:Create}";
            mControllerButtons.mY = "#{Interface:Info}";
            mControllerButtons.mB = mXboxPage == XboxPage::Picker ? "#{Interface:Back}" : "#{Interface:Cancel}";
        }
        else
        {
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mX = "#{Interface:Create}";
            mControllerButtons.mY = "#{Interface:Info}";
            mControllerButtons.mB = "#{Interface:Cancel}";
        }

        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
    }

    void AlchemyWindow::setControllerFocusWidget(MyGUI::Widget* widget)
    {
        if (!widget)
            return;

        MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(widget);
        mLastControllerFocusWidget = widget;
        if (mUseXboxAlchemyUi)
        {
            const auto it = std::find(mIngredients.begin(), mIngredients.end(), widget);
            if (it != mIngredients.end())
                mXboxFocusedIngredient = static_cast<int>(std::distance(mIngredients.begin(), it));
        }
        const bool focusInItemView = isFocusInItemView(widget);
        mControllerItemViewFocus = focusInItemView;
        mItemView->setActiveControllerWindow(focusInItemView);
        if (focusInItemView)
            mItemView->refreshControllerFocus();
        updateXboxIngredientFocus();
        updateControllerFocusHighlight();
    }

    void AlchemyWindow::updateControllerFocusHighlight()
    {
        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        CountDialog* countDialog = winMgr->getCountDialog();
        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (countDialog && countDialog->isVisible() && mLastControllerFocusWidget)
            focus = mLastControllerFocusWidget;
        const bool shouldHighlight
            = focus == mNameEdit || focus == mBrewCountEdit || focus == mFilterType || focus == mFilterValue;
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

    void AlchemyWindow::updateControllerFocusState()
    {
        if (!Settings::gui().mControllerMenus)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        CountDialog* countDialog = winMgr->getCountDialog();
        if (countDialog && countDialog->isVisible())
            return;
        const bool keyboardVisible = winMgr->isVirtualKeyboardVisible();
        const bool keyboardClosed = mVirtualKeyboardWasVisible && !keyboardVisible;
        mVirtualKeyboardWasVisible = keyboardVisible;
        if (keyboardClosed && mLastKeyboardEdit)
        {
            if (mUseXboxAlchemyUi)
            {
                if (mXboxPage == XboxPage::Summary)
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
                else if (mItemView->getItemCount() > 0)
                    setControllerFocusWidget(mItemView);
                else
                    setControllerFocusWidget(mFilterType);
            }
            else
                setControllerFocusWidget(mLastKeyboardEdit);
            mLastKeyboardEdit = nullptr;
            return;
        }

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (!keyboardVisible && focus == nullptr)
        {
            if (mUseXboxAlchemyUi)
            {
                if (mXboxPage == XboxPage::Picker)
                {
                    if (mItemView->getItemCount() > 0)
                        setControllerFocusWidget(mItemView);
                    else
                        setControllerFocusWidget(mFilterType);
                }
                else
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
            }
            else if (mItemView->getItemCount() > 0)
                setControllerFocusWidget(mItemView);
            else
                setControllerFocusWidget(mNameEdit);
            return;
        }
        const bool focusInItemView = isFocusInItemView(focus);
        if (focusInItemView && mItemView->getItemCount() <= 0)
        {
            if (mUseXboxAlchemyUi)
                setControllerFocusWidget(mXboxPage == XboxPage::Picker
                        ? static_cast<MyGUI::Widget*>(mFilterType)
                        : static_cast<MyGUI::Widget*>(mIngredients[mXboxFocusedIngredient]));
            else
                setControllerFocusWidget(mNameEdit);
            return;
        }
        if (focusInItemView != mControllerItemViewFocus)
        {
            mControllerItemViewFocus = focusInItemView;
            mItemView->setActiveControllerWindow(focusInItemView);
            if (focusInItemView)
                mItemView->refreshControllerFocus();
        }
    }

    void AlchemyWindow::updateEditStaticState()
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool shouldStatic = !winMgr->isVirtualKeyboardVisible();
        if (mNameEdit->getEditStatic() != shouldStatic)
            mNameEdit->setEditStatic(shouldStatic);
        if (mBrewCountEdit->getEditStatic() != shouldStatic)
            mBrewCountEdit->setEditStatic(shouldStatic);
        if (mFilterValue->getEditStatic() != true)
            mFilterValue->setEditStatic(true);
    }

    void AlchemyWindow::resizeToLayoutSize(MyGUI::Widget* source)
    {
        if (!mMainWidget || !source)
            return;

        const MyGUI::IntSize targetSize = source->getSize();
        if (targetSize.width <= 0 || targetSize.height <= 0)
            return;

        if (mMainWidget->getSize() != targetSize)
            mMainWidget->setSize(targetSize);

        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
            window->setMinSize(targetSize);
    }

    void AlchemyWindow::setXboxIngredientFocus(int index)
    {
        if (!mUseXboxAlchemyUi)
            return;

        mXboxFocusedIngredient = std::clamp(index, 0, static_cast<int>(mIngredients.size()) - 1);
        updateXboxIngredientFocus();
    }

    void AlchemyWindow::updateXboxIngredientFocus()
    {
        if (!mUseXboxAlchemyUi || !Settings::gui().mControllerMenus)
            return;

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        const bool summaryFocus = mXboxPage == XboxPage::Summary && focus != nullptr
            && std::find(mIngredients.begin(), mIngredients.end(), focus) != mIngredients.end();
        for (size_t i = 0; i < mIngredients.size(); ++i)
            mIngredients[i]->setControllerFocus(summaryFocus && static_cast<int>(i) == mXboxFocusedIngredient);
    }

    void AlchemyWindow::switchXboxPage(XboxPage page)
    {
        if (!mUseXboxAlchemyUi)
            return;

        mXboxPage = page;
        if (mXboxSummaryPanel)
            mXboxSummaryPanel->setVisible(page == XboxPage::Summary);
        if (mXboxIngredientPickerPanel)
            mXboxIngredientPickerPanel->setVisible(page == XboxPage::Picker);
        if (mFilterListHighlight)
            mFilterListHighlight->setVisible(false);

        mControllerItemViewFocus = false;
        mItemView->setActiveControllerWindow(false);

        resizeToLayoutSize(page == XboxPage::Summary ? mXboxSummaryPanel : mXboxIngredientPickerPanel);
        center();
        updateControllerButtons();
        updateXboxIngredientFocus();
    }

    void AlchemyWindow::openXboxIngredientPicker(size_t slot)
    {
        if (!mUseXboxAlchemyUi)
            return;

        mXboxSelectedIngredientSlot = static_cast<int>(std::clamp<size_t>(slot, 0, mIngredients.size() - 1));
        mXboxFocusedIngredient = mXboxSelectedIngredientSlot;
        switchXboxPage(XboxPage::Picker);

        if (!Settings::gui().mControllerMenus)
            return;

        if (mIngredients[mXboxSelectedIngredientSlot]->isUserString("ToolTipType"))
        {
            const MWWorld::Ptr item = *mIngredients[mXboxSelectedIngredientSlot]->getUserData<MWWorld::Ptr>();
            mItemView->setActiveControllerWindow(true);
            setControllerFocusWidget(mItemView);
            mItemView->setControllerFocusToItem(item);
        }
        else if (mItemView->getItemCount() > 0)
            setControllerFocusWidget(mItemView);
        else
            setControllerFocusWidget(mFilterType);
    }

    void AlchemyWindow::closeXboxIngredientPicker()
    {
        if (!mUseXboxAlchemyUi)
            return;

        switchXboxPage(XboxPage::Summary);
        mXboxFocusedIngredient = std::clamp(mXboxSelectedIngredientSlot, 0, static_cast<int>(mIngredients.size()) - 1);
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
    }

    int AlchemyWindow::getLastFilledIngredientIndex() const
    {
        for (int i = static_cast<int>(mIngredients.size()) - 1; i >= 0; --i)
        {
            if (mIngredients[i] && mIngredients[i]->isUserString("ToolTipType"))
                return i;
        }

        return -1;
    }

    void AlchemyWindow::showControllerInfo()
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        winMgr->setControllerTooltipVisible(true);
        winMgr->restoreControllerTooltips();
    }

    void AlchemyWindow::onNameEdited(MyGUI::EditBox* sender)
    {
        const size_t length = sender->getCaption().size();
        if (mSuppressNameLimitMessage)
        {
            mNameHitLimit = length >= kAlchemyNameLimit;
            return;
        }

        if (length < kAlchemyNameLimit)
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

    void AlchemyWindow::setNameCaption(const MyGUI::UString& caption)
    {
        mSuppressNameLimitMessage = true;
        mNameEdit->setCaption(caption);
        mSuppressNameLimitMessage = false;
        mNameHitLimit = caption.size() >= kAlchemyNameLimit;
    }

    void AlchemyWindow::setNameCaptionWithReplacing(const std::string& caption)
    {
        mSuppressNameLimitMessage = true;
        mNameEdit->setCaptionWithReplacing(caption);
        mSuppressNameLimitMessage = false;
        mNameHitLimit = mNameEdit->getCaption().size() >= kAlchemyNameLimit;
    }

    bool AlchemyWindow::isFocusInItemView(MyGUI::Widget* focus) const
    {
        return focus && mItemView && isChildOf(focus, mItemView);
    }

    MyGUI::Widget* AlchemyWindow::getControllerFocusTooltipWidget() const
    {
        if (!mUseXboxAlchemyUi || mXboxPage != XboxPage::Summary || mXboxFocusedIngredient < 0
            || mXboxFocusedIngredient >= static_cast<int>(mIngredients.size()))
        {
            return nullptr;
        }

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (std::find(mIngredients.begin(), mIngredients.end(), focus) == mIngredients.end())
            return nullptr;

        return mIngredients[mXboxFocusedIngredient];
    }

    bool AlchemyWindow::onXboxControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        const bool isFilterListOpen
            = focus != nullptr && focus->getParent() != nullptr && focus->getParent()->getParent() == mFilterValue;

        if (isFilterListOpen)
        {
            filterListButtonHandler(arg);
            if (MyGUI::ListBox* list = getFilterListBox(focus))
                updateFilterListHighlight(list);
            return true;
        }
        else if (mFilterListHighlight)
            mFilterListHighlight->setVisible(false);

        const bool focusInItemView = isFocusInItemView(focus);

        if (arg.button == SDL_CONTROLLER_BUTTON_START)
        {
            openVirtualKeyboard(mNameEdit);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        {
            onDecreaseButtonTriggered();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            onIncreaseButtonTriggered();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onCreateButtonClicked(mCreateButton);
            return true;
        }

        if (mXboxPage == XboxPage::Summary)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_A)
            {
                if (focus == mNameEdit)
                {
                    openVirtualKeyboard(mNameEdit);
                    return true;
                }
                if (focus == mBrewCountEdit)
                {
                    CountDialog* dialog = MWBase::Environment::get().getWindowManager()->getCountDialog();
                    dialog->openCountDialog("Quantity", "Quantity", 100, this);
                    dialog->setCount(std::clamp(mBrewCountEdit->getValue(), 1, 100));
                    dialog->eventOkClicked.clear();
                    dialog->eventOkClicked += MyGUI::newDelegate(this, &AlchemyWindow::onBrewCountSelected);
                    mControllerItemViewFocus = false;
                    mItemView->setActiveControllerWindow(false);
                    updateControllerFocusHighlight();
                    return true;
                }
                openXboxIngredientPicker(mXboxFocusedIngredient);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_Y)
            {
                showControllerInfo();
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_B)
            {
                const int latestIngredient = getLastFilledIngredientIndex();
                if (latestIngredient >= 0)
                {
                    mAlchemy->removeIngredient(latestIngredient);
                    update();
                    mXboxFocusedIngredient = latestIngredient;
                    if (Settings::gui().mControllerMenus)
                        setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
                }
                else
                    onCancelButtonClicked(mCancelButton);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
            {
                if (focus == mBrewCountEdit)
                    setControllerFocusWidget(mNameEdit);
                else if (focus != mNameEdit)
                {
                    setXboxIngredientFocus(mXboxFocusedIngredient - 1);
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
                }
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
            {
                if (focus == mNameEdit)
                    setControllerFocusWidget(mBrewCountEdit);
                else if (focus != mBrewCountEdit)
                {
                    setXboxIngredientFocus(mXboxFocusedIngredient + 1);
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
                }
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            {
                if (focus != mNameEdit && focus != mBrewCountEdit)
                    setControllerFocusWidget(mNameEdit);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            {
                if (focus == mNameEdit)
                {
                    setXboxIngredientFocus(mXboxFocusedIngredient);
                    setControllerFocusWidget(mIngredients[mXboxFocusedIngredient]);
                }
                return true;
            }

            return true;
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            closeXboxIngredientPicker();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_Y)
        {
            showControllerInfo();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (focus == mFilterType)
            {
                switchFilterType(mFilterType);
                return true;
            }
            if (focus == mFilterValue && mFilterValue->getItemCount() > 0)
            {
                MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mFilterValue);
                MWBase::Environment::get().getWindowManager()->injectKeyPress(MyGUI::KeyCode::ArrowDown, 0, false);
                return true;
            }
        }

        if (focusInItemView)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN && mItemView->isControllerFocusBottomRow())
            {
                setControllerFocusWidget(mFilterType);
                return true;
            }

            mItemView->onControllerButton(arg.button);
            return true;
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (focus == mFilterValue)
                setControllerFocusWidget(mFilterType);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (focus == mFilterType)
                setControllerFocusWidget(mFilterValue);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if ((focus == mFilterType || focus == mFilterValue) && mItemView->getItemCount() > 0)
                setControllerFocusWidget(mItemView);
            return true;
        }

        return true;
    }

    bool AlchemyWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (mUseXboxAlchemyUi)
            return onXboxControllerButtonEvent(arg);

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        bool isFilterListOpen
            = focus != nullptr && focus->getParent() != nullptr && focus->getParent()->getParent() == mFilterValue;

        if (isFilterListOpen)
        {
            filterListButtonHandler(arg);
            if (MyGUI::ListBox* list = getFilterListBox(focus))
                updateFilterListHighlight(list);
            return true;
        }
        else if (mFilterListHighlight)
            mFilterListHighlight->setVisible(false);

        const bool focusInItemView = isFocusInItemView(focus);

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (focus == mNameEdit)
            {
                openVirtualKeyboard(mNameEdit);
                return true;
            }
            if (focus == mBrewCountEdit)
            {
                CountDialog* dialog = MWBase::Environment::get().getWindowManager()->getCountDialog();
                dialog->openCountDialog("Quantity", "Quantity", 100, this);
                dialog->setCount(std::clamp(mBrewCountEdit->getValue(), 1, 100));
                dialog->eventOkClicked.clear();
                dialog->eventOkClicked += MyGUI::newDelegate(this, &AlchemyWindow::onBrewCountSelected);
                mControllerItemViewFocus = false;
                mItemView->setActiveControllerWindow(false);
                updateControllerFocusHighlight();
                return true;
            }
            if (focus == mFilterType)
            {
                switchFilterType(mFilterType);
                return true;
            }
            if (focus == mFilterValue && mFilterValue->getItemCount() > 0)
            {
                MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mFilterValue);
                MWBase::Environment::get().getWindowManager()->injectKeyPress(MyGUI::KeyCode::ArrowDown, 0, false);
                return true;
            }
        }

        if (focusInItemView)
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP && mItemView->isControllerFocusTopRow())
            {
                setControllerFocusWidget(mNameEdit);
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN && mItemView->isControllerFocusBottomRow())
            {
                setControllerFocusWidget(mFilterType);
                return true;
            }
        }
        else
        {
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
            {
                if (focus == mBrewCountEdit)
                {
                    setControllerFocusWidget(mNameEdit);
                    return true;
                }
                if (focus == mFilterValue)
                {
                    setControllerFocusWidget(mFilterType);
                    return true;
                }
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
            {
                if (focus == mNameEdit)
                {
                    setControllerFocusWidget(mBrewCountEdit);
                    return true;
                }
                if (focus == mFilterType)
                {
                    setControllerFocusWidget(mFilterValue);
                    return true;
                }
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
            {
                if (focus == mNameEdit || focus == mBrewCountEdit)
                {
                    if (mItemView->getItemCount() > 0)
                        setControllerFocusWidget(mItemView);
                    else
                        setControllerFocusWidget(mFilterType);
                    return true;
                }
                return true;
            }
            if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            {
                if (focus == mFilterType || focus == mFilterValue)
                {
                    if (mItemView->getItemCount() > 0)
                        setControllerFocusWidget(mItemView);
                    else
                        setControllerFocusWidget(mNameEdit);
                    return true;
                }
                return true;
            }
        }

        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            if (!mNameEdit->getCaption().empty())
            {
                setNameCaption({});
                return true;
            }
            for (size_t i = mIngredients.size(); i > 0; --i)
            {
                if (mIngredients[i - 1]->isUserString("ToolTipType"))
                {
                    onIngredientSelected(mIngredients[i - 1]);
                    return true;
                }
            }
            onCancelButtonClicked(mCancelButton);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
            onCreateButtonClicked(mCreateButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
            onDecreaseButtonTriggered();
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
            onIncreaseButtonTriggered();
        else if (focusInItemView)
            mItemView->onControllerButton(arg.button);

        return true;
    }
}
