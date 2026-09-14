#include "dialogue.hpp"

#include <cmath>

#include <MyGUI_Button.h>
#include <MyGUI_LanguageManager.h>
#include <MyGUI_ProgressBar.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_UString.h>
#include <MyGUI_Window.h>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/settings/values.hpp>
#include <components/translation/translation.hpp>
#include <components/widgets/box.hpp>
#include <components/widgets/list.hpp>

#include "../mwbase/dialoguemanager.hpp"
#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwdialogue/keywordsearch.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/player.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "bookpage.hpp"
#include "textcolours.hpp"

namespace MWGui
{
    void ResponseCallback::addResponse(std::string_view title, std::string_view text)
    {
        if (mResetHistory && Settings::gui().mXboxStyledDialog)
            mWindow->mHistoryContents.clear();
        mWindow->addResponse(title, text, mNeedMargin);
    }

    void ResponseCallback::updateTopics() const
    {
        mWindow->updateTopics();
    }

    PersuasionDialog::PersuasionDialog(std::unique_ptr<ResponseCallback> callback)
        : WindowModal("openmw_persuasion_dialog.layout")
        , mCallback(std::move(callback))
        , mInitialGoldLabelWidth(0)
        , mInitialMainWidgetWidth(0)
    {
        getWidget(mAdmireButton, "AdmireButton");
        getWidget(mIntimidateButton, "IntimidateButton");
        getWidget(mTauntButton, "TauntButton");
        getWidget(mBribe10Button, "Bribe10Button");
        getWidget(mBribe100Button, "Bribe100Button");
        getWidget(mBribe1000Button, "Bribe1000Button");
        getWidget(mGoldLabel, "GoldLabel");
        getWidget(mActionsBox, "ActionsBox");

        int totalHeight = 3;
        adjustAction(mAdmireButton, totalHeight);
        adjustAction(mIntimidateButton, totalHeight);
        adjustAction(mTauntButton, totalHeight);
        adjustAction(mBribe10Button, totalHeight);
        adjustAction(mBribe100Button, totalHeight);
        adjustAction(mBribe1000Button, totalHeight);
        totalHeight += 3;

        int diff = totalHeight - mActionsBox->getSize().height;
        if (diff > 0)
        {
            auto mainWidgetSize = mMainWidget->getSize();
            mMainWidget->setSize(mainWidgetSize.width, mainWidgetSize.height + diff);
        }

        mInitialGoldLabelWidth = mActionsBox->getSize().width - 8;
        mInitialMainWidgetWidth = mMainWidget->getSize().width;

        mAdmireButton->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);
        mIntimidateButton->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);
        mTauntButton->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);
        mBribe10Button->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);
        mBribe100Button->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);
        mBribe1000Button->eventMouseButtonClick += MyGUI::newDelegate(this, &PersuasionDialog::onPersuade);

        mDisableGamepadCursor = Settings::gui().mControllerMenus;
        mControllerButtons = {};
        mControllerButtons.mA = "#{Interface:Select}";
        mControllerButtons.mB = "#{Interface:Back}";

        if (Settings::gui().mControllerMenus)
        {
            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);
        }
    }

    void PersuasionDialog::adjustAction(MyGUI::Widget* action, int& totalHeight)
    {
        const int lineHeight = Settings::gui().mFontSize + 2;
        auto currentCoords = action->getCoord();
        action->setCoord(currentCoords.left, totalHeight, currentCoords.width, lineHeight);
        totalHeight += lineHeight;
    }

    void PersuasionDialog::onCancel(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
    }

    void PersuasionDialog::onPersuade(MyGUI::Widget* sender)
    {
        MWBase::MechanicsManager::PersuasionType type;
        if (sender == mAdmireButton)
            type = MWBase::MechanicsManager::PT_Admire;
        else if (sender == mIntimidateButton)
            type = MWBase::MechanicsManager::PT_Intimidate;
        else if (sender == mTauntButton)
            type = MWBase::MechanicsManager::PT_Taunt;
        else if (sender == mBribe10Button)
            type = MWBase::MechanicsManager::PT_Bribe10;
        else if (sender == mBribe100Button)
            type = MWBase::MechanicsManager::PT_Bribe100;
        else /*if (sender == mBribe1000Button)*/
            type = MWBase::MechanicsManager::PT_Bribe1000;

        MWBase::Environment::get().getDialogueManager()->persuade(type, mCallback.get());
        mCallback->updateTopics();

        setVisible(false);
    }

    void PersuasionDialog::onOpen()
    {
        center();

        MWWorld::Ptr player = MWMechanics::getPlayer();
        int playerGold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);

        mBribe10Button->setEnabled(playerGold >= 10);
        mBribe100Button->setEnabled(playerGold >= 100);
        mBribe1000Button->setEnabled(playerGold >= 1000);

        mGoldLabel->setCaptionWithReplacing("#{sGold}: " + MyGUI::utility::toString(playerGold));

        int diff = mGoldLabel->getRequestedSize().width - mInitialGoldLabelWidth;
        if (diff > 0)
            mMainWidget->setSize(mInitialMainWidgetWidth + diff, mMainWidget->getSize().height);
        else
            mMainWidget->setSize(mInitialMainWidgetWidth, mMainWidget->getSize().height);

        if (Settings::gui().mControllerMenus)
        {
            mControllerFocus = 0;
            mButtons.clear();
            mButtons.push_back(mAdmireButton);
            mButtons.push_back(mIntimidateButton);
            mButtons.push_back(mTauntButton);
            if (mBribe10Button->getEnabled())
                mButtons.push_back(mBribe10Button);
            if (mBribe100Button->getEnabled())
                mButtons.push_back(mBribe100Button);
            if (mBribe1000Button->getEnabled())
                mButtons.push_back(mBribe1000Button);

            for (size_t i = 0; i < mButtons.size(); i++)
                mButtons[i]->setStateSelected(i == 0);
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mButtons[mControllerFocus]);
            updateControllerFocusHighlight();
        }
        else if (mControllerFocusHighlight)
        {
            mControllerFocusHighlight->setVisible(false);
        }

        WindowModal::onOpen();
    }

    MyGUI::Widget* PersuasionDialog::getDefaultKeyFocus()
    {
        return mAdmireButton;
    }

    void PersuasionDialog::updateControllerFocusHighlight()
    {
        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        if (mButtons.empty() || mControllerFocus >= mButtons.size())
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focus = mButtons[mControllerFocus];
        if (!focus)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::IntCoord focusCoord = focus->getAbsoluteCoord();
        if (mActionsBox)
        {
            const MyGUI::IntCoord boxCoord = mActionsBox->getAbsoluteCoord();
            focusCoord.left = boxCoord.left;
            focusCoord.width = boxCoord.width;
        }
        MyGUI::Widget* highlightParent = mControllerFocusHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        mControllerFocusHighlight->setCoord(
            focusCoord.left - baseCoord.left, focusCoord.top - baseCoord.top, focusCoord.width, focusCoord.height);
        mControllerFocusHighlight->setVisible(true);
    }

    bool PersuasionDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            onPersuade(mButtons[mControllerFocus]);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCancel(nullptr);
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus, mButtons.size(), -1);
            setControllerFocus(mButtons, mControllerFocus, true);
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mButtons[mControllerFocus]);
            updateControllerFocusHighlight();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            setControllerFocus(mButtons, mControllerFocus, false);
            mControllerFocus = wrap(mControllerFocus, mButtons.size(), 1);
            setControllerFocus(mButtons, mControllerFocus, true);
            MWBase::Environment::get().getWindowManager()->setKeyFocusWidget(mButtons[mControllerFocus]);
            updateControllerFocusHighlight();
        }

        return true;
    }

    // --------------------------------------------------------------------------------------------------

    Response::Response(std::string_view text, std::string_view title, bool needMargin)
        : mTitle(title)
        , mNeedMargin(needMargin)
    {
        mText = text;
    }

    void Response::write(std::shared_ptr<BookTypesetter> typesetter, const MWDialogue::KeywordSearch& keywordSearch,
        std::unordered_map<std::string, std::unique_ptr<Link>>& topicLinks) const
    {
        using namespace MWDialogue;

        MWBase::WindowManager& windowManager = *MWBase::Environment::get().getWindowManager();
        const Translation::Storage& translationStorage = windowManager.getTranslationDataStorage();
        const TextColours& colors = windowManager.getTextColours();

        typesetter->sectionBreak(mNeedMargin ? 9 : 0);

        if (!mTitle.empty())
        {
            BookTypesetter::Style* title = typesetter->createStyle("DialogueBoldFont", colors.header, false);
            typesetter->write(title, mTitle);
            typesetter->sectionBreak();
        }

        struct Token
        {
            size_t mStart;
            size_t mEnd;
            Link* mTopic;
        };

        std::vector<KeywordSearch::Match> matches = keywordSearch.parseHyperText(mText, translationStorage);
        std::vector<Token> tokens;
        tokens.reserve(matches.size());
        std::string text;
        text.reserve(mText.size());

        // Generate the displayed text and a more convenient token list.
        // The matches we got provide positions in the original text and must be recalculated.
        KeywordSearch::Point pos = mText.begin();
        for (const KeywordSearch::Match& token : matches)
        {
            const std::string_view displayName(token.getDisplayName());
            text.append(pos, token.mBeg);
            text.append(displayName);
            pos = token.mEnd;

            auto value = topicLinks.find(token.mTopicId);
            if (value != topicLinks.end())
                tokens.emplace_back(text.size() - displayName.size(), text.size(), value->second.get());
        }
        text.append(pos, mText.end());

        typesetter->addContent(text);

        BookTypesetter::Style* textStyle = typesetter->createStyle("DialogueBoldFont", colors.normal, false);

        size_t i = 0;
        for (const Token& token : tokens)
        {
            if (i < token.mStart)
                typesetter->write(textStyle, i, token.mStart);

            auto id = reinterpret_cast<TypesetBook::InteractiveId>(token.mTopic);
            BookTypesetter::Style* linkStyle
                = typesetter->createHotStyle(textStyle, colors.link, colors.linkOver, colors.linkPressed, id);
            typesetter->write(linkStyle, token.mStart, token.mEnd);
            i = token.mEnd;
        }

        if (i < text.size())
            typesetter->write(textStyle, i, text.size());
    }

    Message::Message(std::string_view text)
    {
        mText = text;
    }

    void Message::write(std::shared_ptr<BookTypesetter> typesetter, const MWDialogue::KeywordSearch&,
        std::unordered_map<std::string, std::unique_ptr<Link>>&) const
    {
        const MyGUI::Colour& textColour = MWBase::Environment::get().getWindowManager()->getTextColours().notify;
        BookTypesetter::Style* title = typesetter->createStyle("DialogueBoldFont", textColour, false);
        typesetter->sectionBreak(9);
        typesetter->write(title, mText);
    }

    // --------------------------------------------------------------------------------------------------

    void Choice::activated()
    {
        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        eventChoiceActivated(mChoiceId);
    }

    void Topic::activated()
    {
        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        eventTopicActivated(mTopicId);
    }

    void Goodbye::activated()
    {
        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        eventActivated();
    }

    // --------------------------------------------------------------------------------------------------

    // Use tighter padding so dialogue topics are less spaced out in the list.
    static constexpr int sVerticalPadding = 2;
    static constexpr int sHistoryScrollbarReserve = 3;
    static constexpr float sControllerNavUpDelaySeconds = 0.25f;
    static constexpr float sControllerNavUpHoldThreshold = 0.85f;

    DialogueWindow::DialogueWindow()
        : WindowBase("openmw_dialogue_window.layout")
        , mIsCompanion(false)
        , mGoodbye(false)
        , mHistoryBox(nullptr)
        , mHistoryContainer(nullptr)
        , mDispositionBar(nullptr)
        , mDispositionText(nullptr)
        , mGoodbyeButton(nullptr)
        , mPersuasionDialog(std::make_unique<ResponseCallback>(this, true, true))
        , mCallback(std::make_unique<ResponseCallback>(this))
        , mGreetingCallback(std::make_unique<ResponseCallback>(this, false))
    {
        // Centre dialog
        center();

        mPersuasionDialog.setVisible(false);

        // History view
        getWidget(mHistory, "History");
        getWidget(mHistoryBox, "HistoryBox");
        getWidget(mHistoryContainer, "HistoryContainer");

        // Topics list
        getWidget(mTopicsList, "TopicsList");
        mTopicsList->eventItemSelected += MyGUI::newDelegate(this, &DialogueWindow::onSelectListItem);

        getWidget(mScrollBar, "VScroll");

        mScrollBar->eventScrollChangePosition += MyGUI::newDelegate(this, &DialogueWindow::onScrollbarMoved);
        mHistory->eventMouseWheel += MyGUI::newDelegate(this, &DialogueWindow::onMouseWheel);

        BookPage::ClickCallback callback = [this](TypesetBook::InteractiveId link) { notifyLinkClicked(link); };
        mHistory->adviseLinkClicked(std::move(callback));

        mMainWidget->castType<MyGUI::Window>()->eventWindowChangeCoord
            += MyGUI::newDelegate(this, &DialogueWindow::onWindowResize);

        mControllerScrollWidget = mHistory->getParent();
        mControllerButtons = {};
        mControllerButtons.mA = "#{Interface:Ask}";
        mControllerButtons.mB = "#{Interface:Goodbye}";
        updateControllerScrollButtons();
    }

    void DialogueWindow::onTradeComplete()
    {
        MyGUI::UString message = MyGUI::LanguageManager::getInstance().replaceTags("#{sBarterDialog5}");
        addResponse({}, message);
    }

    void DialogueWindow::onOpen()
    {
        resetFixedWindowGeometry();
        mCurrentWindowSize = mMainWidget->getSize();
        updateLayoutSizes();
        redrawTopicsList();
        updateHistory();
        if (Settings::gui().mControllerMenus && mTopicsList->getItemCount() > 0)
            setControllerFocus(mControllerFocus, true);

        mControllerNavUpDelay = 0.f;
        if (Settings::gui().mControllerMenus && Settings::gui().mControllerJoystickDpad)
        {
            const float axisY
                = MWBase::Environment::get().getInputManager()->getControllerAxisValue(SDL_CONTROLLER_AXIS_LEFTY);
            if (axisY <= -sControllerNavUpHoldThreshold)
                mControllerNavUpDelay = sControllerNavUpDelaySeconds;
        }
    }

    bool DialogueWindow::exit()
    {
        if ((MWBase::Environment::get().getDialogueManager()->isInChoice()))
        {
            return false;
        }
        else
        {
            resetReference();
            MWBase::Environment::get().getDialogueManager()->goodbyeSelected();
            mTopicsList->scrollToTop();
            return true;
        }
    }

    void DialogueWindow::onWindowResize(MyGUI::Window* sender)
    {
        // if the window has only been moved, not resized, we don't need to update
        if (mCurrentWindowSize == sender->getSize())
            return;

        updateLayoutSizes();
        redrawTopicsList();
        updateHistory();
        mCurrentWindowSize = sender->getSize();
    }

    void DialogueWindow::resetFixedWindowGeometry()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            MyGUI::IntSize viewSize = window->getLayer() ? window->getLayer()->getSize()
                                                         : MyGUI::RenderManager::getInstance().getViewSize();
            window->setCoord(getFixedWindowCoord(viewSize));
        }
    }

    MyGUI::IntCoord DialogueWindow::getFixedWindowCoord(const MyGUI::IntSize& viewSize) const
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

    void DialogueWindow::onMouseWheel(MyGUI::Widget* /*sender*/, int rel)
    {
        if (!mScrollBar->getVisible() || mScrollBar->getScrollRange() == 0)
            return;

        const float scaledStep = rel * 0.3f;
        const int scrollDelta = scaledStep > 0.f ? std::max(1, static_cast<int>(std::floor(scaledStep)))
                                                 : std::min(-1, static_cast<int>(std::ceil(scaledStep)));
        const int maxPosition = static_cast<int>(mScrollBar->getScrollRange() - 1);
        const int position
            = std::clamp<int>(static_cast<int>(mScrollBar->getScrollPosition()) - scrollDelta, 0, maxPosition);
        mScrollBar->setScrollPosition(position);
        onScrollbarMoved(mScrollBar, mScrollBar->getScrollPosition());
    }

    void DialogueWindow::onByeClicked(MyGUI::Widget* /*sender*/)
    {
        if (exit())
        {
            MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Dialogue);
        }
    }

    void DialogueWindow::onSelectListItem(const std::string& topic, int /*id*/)
    {
        MWBase::DialogueManager* dialogueManager = MWBase::Environment::get().getDialogueManager();

        if (mGoodbye || dialogueManager->isInChoice())
            return;

        ResponseCallback serviceRefusalCallback(this, true, true);
        ResponseCallback* serviceCallback
            = Settings::gui().mXboxStyledDialog ? &serviceRefusalCallback : mCallback.get();

        const MWWorld::Store<ESM::GameSetting>& gmst
            = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();

        const std::string& sPersuasion = gmst.find("sPersuasion")->mValue.getString();
        const std::string& sCompanionShare = gmst.find("sCompanionShare")->mValue.getString();
        const std::string& sBarter = gmst.find("sBarter")->mValue.getString();
        const std::string& sSpells = gmst.find("sSpells")->mValue.getString();
        const std::string& sTravel = gmst.find("sTravel")->mValue.getString();
        const std::string& sSpellMakingMenuTitle = gmst.find("sSpellMakingMenuTitle")->mValue.getString();
        const std::string& sEnchanting = gmst.find("sEnchanting")->mValue.getString();
        const std::string& sServiceTrainingTitle = gmst.find("sServiceTrainingTitle")->mValue.getString();
        const std::string& sRepair = gmst.find("sRepair")->mValue.getString();

        if (topic != sPersuasion && topic != sCompanionShare && topic != sBarter && topic != sSpells && topic != sTravel
            && topic != sSpellMakingMenuTitle && topic != sEnchanting && topic != sServiceTrainingTitle
            && topic != sRepair)
        {
            onTopicActivated(topic);
        }
        else if (topic == sPersuasion)
            mPersuasionDialog.setVisible(true);
        else if (topic == sCompanionShare)
            MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_Companion, mPtr);
        else if (!dialogueManager->checkServiceRefused(serviceCallback))
        {
            if (topic == sBarter
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Barter))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_Barter, mPtr);
            else if (topic == sSpells
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Spells))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_SpellBuying, mPtr);
            else if (topic == sTravel
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Travel))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_Travel, mPtr);
            else if (topic == sSpellMakingMenuTitle
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Spellmaking))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_SpellCreation, mPtr);
            else if (topic == sEnchanting
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Enchanting))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_Enchanting, mPtr);
            else if (topic == sServiceTrainingTitle
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Training))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_Training, mPtr);
            else if (topic == sRepair
                && !dialogueManager->checkServiceRefused(serviceCallback, MWBase::DialogueManager::Repair))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(GM_MerchantRepair, mPtr);
        }
        else
            updateTopics();
    }

    void DialogueWindow::setPtr(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
        {
            Log(Debug::Warning) << "Warning: can not talk with non-actor object.";
            return;
        }

        bool sameActor = (mPtr == actor);
        if (!sameActor)
        {
            // The history is not reset here
            mKeywords.clear();
            mTopicsList->clear();
            for (auto& link : mLinks)
                mDeleteLater.push_back(
                    std::move(link)); // Links are not deleted right away to prevent issues with event handlers
            mLinks.clear();
        }

        mPtr = actor;
        mGoodbye = false;
        mTopicsList->setEnabled(true);

        if (!MWBase::Environment::get().getDialogueManager()->startDialogue(actor, mGreetingCallback.get()))
        {
            // No greetings found. The dialogue window should not be shown.
            // If this is a companion, we must show the companion window directly (used by BM_bear_be_unique).
            MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_Dialogue);
            mPtr = MWWorld::Ptr();
            if (isCompanion(actor))
                MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Companion, actor);
            return;
        }

        setTitle(mPtr.getClass().getName(mPtr));

        updateTopics();
        updateTopicsPane(); // force update for new services

        if (Settings::gui().mControllerMenus && !sameActor)
        {
            setControllerFocus(mControllerFocus, false);
            // Reset focus to very top. Maybe change this to mTopicsList->getItemCount() - mKeywords.size()?
            mControllerFocus = 0;
            setControllerFocus(mControllerFocus, true);
        }

        updateDisposition();
        restock();
    }

    void DialogueWindow::restock()
    {
        MWMechanics::CreatureStats& sellerStats = mPtr.getClass().getCreatureStats(mPtr);
        float delay = MWBase::Environment::get()
                          .getESMStore()
                          ->get<ESM::GameSetting>()
                          .find("fBarterGoldResetDelay")
                          ->mValue.getFloat();

        // Gold is restocked every 24h
        if (MWBase::Environment::get().getWorld()->getTimeStamp() >= sellerStats.getLastRestockTime() + delay)
        {
            sellerStats.setGoldPool(mPtr.getClass().getBaseGold(mPtr));

            sellerStats.setLastRestockTime(MWBase::Environment::get().getWorld()->getTimeStamp());
        }
    }

    void DialogueWindow::deleteLater()
    {
        mDeleteLater.clear();
    }

    void DialogueWindow::onClose()
    {
        if (MWBase::Environment::get().getWindowManager()->containsMode(GM_Dialogue))
            return;
        // Reset history
        mHistoryContents.clear();
    }

    std::string DialogueWindow::getActorName() const
    {
        if (mPtr.isEmpty())
            return {};
        return std::string(mPtr.getClass().getName(mPtr));
    }

    int DialogueWindow::getDisposition() const
    {
        if (mPtr.isEmpty() || !mPtr.getClass().isNpc())
            return 0;
        return MWBase::Environment::get().getMechanicsManager()->getDerivedDisposition(mPtr);
    }

    int DialogueWindow::getDispositionBarWidth() const
    {
        if (mDispositionBar)
            return mDispositionBar->getWidth();
        if (!mMainWidget)
            return 0;
        return static_cast<int>(mMainWidget->getWidth() * (166.f / 588.f));
    }

    bool DialogueWindow::hasActor() const
    {
        return !mPtr.isEmpty();
    }

    bool DialogueWindow::setKeywords(const std::list<std::string>& keyWords)
    {
        if (mKeywords == keyWords && isCompanion() == mIsCompanion)
            return false;
        mIsCompanion = isCompanion();
        mKeywords = keyWords;
        updateTopicsPane();
        return true;
    }

    void DialogueWindow::redrawTopicsList()
    {
        updateLayoutSizes();
        mTopicsList->adjustSize();
        updateLayoutSizes();

        // The topics list has been regenerated so topic formatting needs to be updated
        updateTopicFormat();
    }

    void DialogueWindow::updateTopicsPane()
    {
        std::string focusedTopic;
        if (Settings::gui().mControllerMenus && mControllerFocus < mTopicsList->getItemCount())
            focusedTopic = mTopicsList->getItemNameAt(mControllerFocus);

        if (Settings::gui().mControllerMenus && Settings::gui().mControllerHighlightSelections)
            mTopicsList->setProperty("ControllerHighlightSkin", "ControllerHighlight");
        else
            mTopicsList->setProperty("ControllerHighlightSkin", "");

        mTopicsList->clear();
        for (auto& linkPair : mTopicLinks)
            mDeleteLater.push_back(std::move(linkPair.second));
        mTopicLinks.clear();
        mKeywordSearch.clear();

        int services = mPtr.getClass().getServices(mPtr);

        bool travel = (mPtr.getType() == ESM::NPC::sRecordId && !mPtr.get<ESM::NPC>()->mBase->getTransport().empty())
            || (mPtr.getType() == ESM::Creature::sRecordId
                && !mPtr.get<ESM::Creature>()->mBase->getTransport().empty());

        const MWWorld::Store<ESM::GameSetting>& gmst
            = MWBase::Environment::get().getESMStore()->get<ESM::GameSetting>();

        if (mPtr.getType() == ESM::NPC::sRecordId)
            mTopicsList->addItem(gmst.find("sPersuasion")->mValue.getString());

        if (services & ESM::NPC::AllItems)
            mTopicsList->addItem(gmst.find("sBarter")->mValue.getString());

        if (services & ESM::NPC::Spells)
            mTopicsList->addItem(gmst.find("sSpells")->mValue.getString());

        if (travel)
            mTopicsList->addItem(gmst.find("sTravel")->mValue.getString());

        if (services & ESM::NPC::Spellmaking)
            mTopicsList->addItem(gmst.find("sSpellmakingMenuTitle")->mValue.getString());

        if (services & ESM::NPC::Enchanting)
            mTopicsList->addItem(gmst.find("sEnchanting")->mValue.getString());

        if (services & ESM::NPC::Training)
            mTopicsList->addItem(gmst.find("sServiceTrainingTitle")->mValue.getString());

        if (services & ESM::NPC::Repair)
            mTopicsList->addItem(gmst.find("sRepair")->mValue.getString());

        if (isCompanion())
            mTopicsList->addItem(gmst.find("sCompanionShare")->mValue.getString());

        if (mTopicsList->getItemCount() > 0)
            mTopicsList->addSeparator();

        MWBase::WindowManager& windowManager = *MWBase::Environment::get().getWindowManager();
        const Translation::Storage& translationStorage = windowManager.getTranslationDataStorage();

        for (const auto& keyword : mKeywords)
        {
            std::string topicId = Misc::StringUtils::lowerCase(keyword);
            mTopicsList->addItem(keyword, sVerticalPadding);

            auto t = std::make_unique<Topic>(keyword);
            mKeywordSearch.seed(translationStorage.topicKeyword(keyword), topicId);
            t->eventTopicActivated += MyGUI::newDelegate(this, &DialogueWindow::onTopicActivated);
            mTopicLinks[topicId] = std::move(t);

            if (keyword == focusedTopic)
                mControllerFocus = mTopicsList->getItemCount() - 1;
        }

        redrawTopicsList();
        updateHistory();

        if (Settings::gui().mControllerMenus)
            setControllerFocus(mControllerFocus, true);
    }

    void DialogueWindow::updateHistory(bool scrollbar)
    {
        const bool oldScrollbarVisible = mScrollBar->getVisible();
        if (!scrollbar && mScrollBar->getVisible())
            mScrollBar->setVisible(false);
        if (scrollbar && !mScrollBar->getVisible())
            mScrollBar->setVisible(true);
        if (oldScrollbarVisible != mScrollBar->getVisible())
            updateLayoutSizes();

        std::shared_ptr<BookTypesetter> typesetter
            = BookTypesetter::create(mHistory->getWidth(), std::numeric_limits<int>::max());

        for (const auto& text : mHistoryContents)
            text->write(typesetter, mKeywordSearch, mTopicLinks);

        BookTypesetter::Style* body = typesetter->createStyle("DialogueBoldFont", MyGUI::Colour::White, false);

        typesetter->sectionBreak(9);
        // choices
        const TextColours& textColours = MWBase::Environment::get().getWindowManager()->getTextColours();
        mChoices = MWBase::Environment::get().getDialogueManager()->getChoices();
        mChoiceStyles.clear();
        mControllerChoice = -1; // -1 so you must make a choice (and can't accidentally pick the first answer)
        if (Settings::gui().mControllerMenus)
            mHistory->setFocusItem(nullptr);
        for (std::pair<std::string, int>& choice : mChoices)
        {
            auto link = std::make_unique<Choice>(choice.second);
            link->eventChoiceActivated += MyGUI::newDelegate(this, &DialogueWindow::onChoiceActivated);
            auto interactiveId = TypesetBook::InteractiveId(link.get());
            mLinks.push_back(std::move(link));

            typesetter->lineBreak();
            BookTypesetter::Style* questionStyle = typesetter->createHotStyle(
                body, textColours.answer, textColours.answerOver, textColours.answerPressed, interactiveId);
            typesetter->write(questionStyle, choice.first);
            mChoiceStyles.push_back(questionStyle);
        }

        mGoodbye = MWBase::Environment::get().getDialogueManager()->isGoodbye();
        if (mGoodbye)
        {
            auto link = std::make_unique<Goodbye>();
            link->eventActivated += MyGUI::newDelegate(this, &DialogueWindow::onGoodbyeActivated);
            auto interactiveId = TypesetBook::InteractiveId(link.get());
            mLinks.push_back(std::move(link));
            const std::string& goodbye = MWBase::Environment::get()
                                             .getESMStore()
                                             ->get<ESM::GameSetting>()
                                             .find("sGoodbye")
                                             ->mValue.getString();
            BookTypesetter::Style* questionStyle = typesetter->createHotStyle(
                body, textColours.answer, textColours.answerOver, textColours.answerPressed, interactiveId);
            typesetter->lineBreak();
            typesetter->write(questionStyle, goodbye);
        }

        std::shared_ptr<TypesetBook> book = typesetter->complete();
        mHistory->showPage(book, 0);
        size_t viewHeight = mHistory->getParent()->getHeight();
        if (!scrollbar && book->getSize().second > viewHeight)
            updateHistory(true);
        else if (scrollbar)
        {
            mHistory->setSize(MyGUI::IntSize(mHistory->getWidth(), book->getSize().second));
            // Scroll range should be >= 2 to enable scrolling and prevent a crash
            size_t range = std::max(book->getSize().second - viewHeight, size_t(2));
            mScrollBar->setScrollRange(range);
            const size_t targetPos = Settings::gui().mXboxStyledDialog ? 0 : range - 1;
            mScrollBar->setScrollPosition(targetPos);
            mScrollBar->setTrackSize(
                static_cast<int>(viewHeight / static_cast<float>(book->getSize().second) * mScrollBar->getLineSize()));
            onScrollbarMoved(mScrollBar, targetPos);
        }

        if (Settings::gui().mControllerMenus && !mChoiceStyles.empty())
        {
            mControllerChoice = 0;
            mHistory->setFocusItem(mChoiceStyles.front());
        }
        else if (!mScrollBar->getVisible())
        {
            onScrollbarMoved(mScrollBar, 0);
        }
        else
        {
            onScrollbarMoved(mScrollBar, mScrollBar->getScrollPosition());
        }

        bool topicsEnabled = !MWBase::Environment::get().getDialogueManager()->isInChoice() && !mGoodbye;
        mTopicsList->setEnabled(topicsEnabled);

        if (oldScrollbarVisible != mScrollBar->getVisible())
            updateControllerScrollButtons();
    }

    void DialogueWindow::notifyLinkClicked(TypesetBook::InteractiveId link)
    {
        reinterpret_cast<Link*>(link)->activated();
    }

    void DialogueWindow::onTopicActivated(const std::string& topicId)
    {
        if (mGoodbye)
            return;

        if (Settings::gui().mXboxStyledDialog)
        {
            mHistoryContents.clear();
            updateHistory();
        }

        MWBase::Environment::get().getDialogueManager()->keywordSelected(topicId, mCallback.get());
        updateTopics();
    }

    void DialogueWindow::onChoiceActivated(int id)
    {
        if (mGoodbye)
        {
            onGoodbyeActivated();
            return;
        }

        if (Settings::gui().mXboxStyledDialog)
        {
            mHistoryContents.clear();
            updateHistory();
        }
        MWBase::Environment::get().getDialogueManager()->questionAnswered(id, mCallback.get());
        updateTopics();
    }

    void DialogueWindow::onGoodbyeActivated()
    {
        MWBase::Environment::get().getDialogueManager()->goodbyeSelected();
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_Dialogue);
        resetReference();
    }

    void DialogueWindow::onScrollbarMoved(MyGUI::ScrollBar* sender, size_t pos)
    {
        mHistory->setPosition(0, static_cast<int>(pos) * -1);
    }

    void DialogueWindow::addResponse(std::string_view title, std::string_view text, bool needMargin)
    {
        mHistoryContents.push_back(std::make_unique<Response>(text, title, needMargin));
        updateHistory();
    }

    void DialogueWindow::addMessageBox(std::string_view text)
    {
        mHistoryContents.push_back(std::make_unique<Message>(text));
        updateHistory();
    }

    void DialogueWindow::updateDisposition()
    {
        if (!mDispositionBar || !mDispositionText)
            return;

        bool dispositionVisible = false;
        if (!mPtr.isEmpty() && mPtr.getClass().isNpc())
        {
            // If actor was a witness to a crime which was payed off,
            // restore original disposition immediately.
            MWMechanics::NpcStats& npcStats = mPtr.getClass().getNpcStats(mPtr);
            if (npcStats.getCrimeId() != -1 && npcStats.getCrimeDispositionModifier() != 0)
            {
                if (npcStats.getCrimeId() <= MWBase::Environment::get().getWorld()->getPlayer().getCrimeId())
                    npcStats.setCrimeDispositionModifier(0);
            }

            dispositionVisible = true;
            mDispositionBar->setProgressRange(100);
            mDispositionBar->setProgressPosition(
                MWBase::Environment::get().getMechanicsManager()->getDerivedDisposition(mPtr));
            mDispositionText->setCaption(
                MyGUI::utility::toString(MWBase::Environment::get().getMechanicsManager()->getDerivedDisposition(mPtr))
                + std::string("/100"));
        }

        if (mDispositionBar->getVisible() != dispositionVisible)
        {
            mDispositionBar->setVisible(dispositionVisible);
            const int offset = (mDispositionBar->getHeight() + 5) * (dispositionVisible ? 1 : -1);
            mTopicsList->setCoord(mTopicsList->getCoord() + MyGUI::IntCoord(0, offset, 0, -offset));
            redrawTopicsList();
        }
    }

    void DialogueWindow::onReferenceUnavailable()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Dialogue);
    }

    void DialogueWindow::onFrame(float dt)
    {
        checkReferenceAvailable();
        if (mPtr.isEmpty())
            return;

        if (mControllerNavUpDelay > 0.f)
        {
            mControllerNavUpDelay -= dt;
            if (mControllerNavUpDelay < 0.f)
                mControllerNavUpDelay = 0.f;
        }

        updateDisposition();
        deleteLater();

        if (mChoices != MWBase::Environment::get().getDialogueManager()->getChoices()
            || mGoodbye != MWBase::Environment::get().getDialogueManager()->isGoodbye())
            updateHistory();
    }

    void DialogueWindow::updateTopicFormat()
    {
        if (!Settings::gui().mColorTopicEnable)
            return;

        for (const std::string& keyword : mKeywords)
        {
            int flag = MWBase::Environment::get().getDialogueManager()->getTopicFlag(ESM::RefId::stringRefId(keyword));
            MyGUI::Button* button = mTopicsList->getItemWidget(keyword);
            const auto oldCaption = button->getCaption();
            const MyGUI::IntSize oldSize = button->getSize();

            bool changed = false;
            if (flag & MWBase::DialogueManager::TopicType::Specific)
            {
                button->changeWidgetSkin("MW_ListLine_Specific");
                changed = true;
            }
            else if (flag & MWBase::DialogueManager::TopicType::Exhausted)
            {
                button->changeWidgetSkin("MW_ListLine_Exhausted");
                changed = true;
            }

            if (changed)
            {
                button->setCaption(oldCaption);
                button->setTextAlign(MyGUI::Align::Left);
                MyGUI::ISubWidgetText* text = button->getSubWidgetText();
                if (text != nullptr)
                    text->setWordWrap(true);
                button->setSize(oldSize);
            }
        }
    }

    void DialogueWindow::updateLayoutSizes()
    {
        if (!mMainWidget || !mHistoryBox || !mHistoryContainer || !mTopicsList)
            return;

        const MyGUI::IntSize windowSize = mMainWidget->getSize();
        const int sideMargin = 8;
        const int topMargin = 8;
        const int bottomMargin = 16;
        const int gap = 9;
        const int innerHeight = std::max(0, windowSize.height - topMargin - bottomMargin);

        const int listWidthMax = 160;
        const int listWidth
            = std::max(0, std::min(listWidthMax, std::max(0, static_cast<int>(windowSize.width * 0.42f))) - 5);
        const int listRightMargin = 16;
        const int listX = std::max(sideMargin, windowSize.width - listRightMargin - listWidth);
        const int leftBoxWidth = std::max(0, listX - gap - sideMargin);

        mHistoryBox->setCoord(sideMargin, topMargin, leftBoxWidth, innerHeight);

        const int innerPadX = 7;
        const int innerPadY = 7;
        const int innerRightPad = 10;
        const int innerBottomPad = 8;
        const int scrollbarReserve
            = (mScrollBar && mScrollBar->getVisible()) ? (mScrollBar->getWidth() + sHistoryScrollbarReserve) : 0;
        const int historyWidth = std::max(0, leftBoxWidth - innerPadX - innerRightPad - scrollbarReserve);
        const int historyHeight = std::max(0, innerHeight - innerBottomPad);
        mHistoryContainer->setCoord(sideMargin + innerPadX, topMargin + innerPadY, historyWidth, historyHeight);
        if (mHistory)
            mHistory->setSize(MyGUI::IntSize(historyWidth, historyHeight));

        if (mScrollBar)
        {
            const int scrollWidth = mScrollBar->getWidth();
            const int scrollRightPadding = 0;
            const int scrollX = sideMargin + leftBoxWidth - scrollWidth - scrollRightPadding;
            mScrollBar->setCoord(scrollX, topMargin, scrollWidth, std::max(0, innerHeight - 1));
        }

        mTopicsList->setCoord(listX, topMargin, listWidth, innerHeight);
    }

    void DialogueWindow::updateControllerScrollButtons()
    {
        const bool scrollable = mScrollBar && mScrollBar->getVisible();
        mControllerButtons.mL2 = scrollable ? "#{Interface:ScrollUp}" : "";
        mControllerButtons.mR2 = scrollable ? "#{Interface:ScrollDown}" : "";
        if (Settings::gui().mControllerMenus)
            MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
    }

    void DialogueWindow::updateTopics()
    {
        // Topic formatting needs to be updated regardless of whether the topic list has changed
        if (!setKeywords(MWBase::Environment::get().getDialogueManager()->getAvailableTopics()))
            updateTopicFormat();
    }

    bool DialogueWindow::isCompanion()
    {
        return isCompanion(mPtr);
    }

    bool DialogueWindow::isCompanion(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty())
            return false;

        return !actor.getClass().getScript(actor).empty()
            && actor.getRefData().getLocals().getIntVar(actor.getClass().getScript(actor), "companion");
    }

    void DialogueWindow::setControllerFocus(size_t index, bool focused)
    {
        if (mTopicsList->getItemCount() == 0 || index >= mTopicsList->getItemCount())
            return;

        const std::string& focusKeyword = mTopicsList->getItemNameAt(mControllerFocus);
        if (focusKeyword.empty())
            return;

        MyGUI::Button* button = mTopicsList->getItemWidget(focusKeyword);
        button->setStateSelected(focused);
        setTopicHighlight(index, focused);

        if (focused)
        {
            // Scroll the side bar to keep the active item in view
            int offset = 0;
            for (int i = 6; i < static_cast<int>(index); i++)
            {
                const std::string& itemKeyword = mTopicsList->getItemNameAt(i);
                if (itemKeyword.empty())
                    offset += 18 + sVerticalPadding * 2;
                else
                    offset += mTopicsList->getItemWidget(itemKeyword)->getHeight() + sVerticalPadding * 2;
            }
            mTopicsList->setViewOffset(-offset);
        }
    }

    void DialogueWindow::setTopicHighlight(size_t index, bool visible)
    {
        if (!Settings::gui().mControllerMenus || !Settings::gui().mControllerHighlightSelections)
            return;

        mTopicsList->setItemHighlightVisible(index, visible);
    }

    bool DialogueWindow::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        const auto isSelectableTopicIndex = [this](size_t index) {
            return index < mTopicsList->getItemCount() && !mTopicsList->getItemNameAt(index).empty();
        };
        const auto findFirstSelectableTopic = [this, &isSelectableTopicIndex]() -> size_t {
            for (size_t i = 0; i < mTopicsList->getItemCount(); ++i)
            {
                if (isSelectableTopicIndex(i))
                    return i;
            }
            return MyGUI::ITEM_NONE;
        };
        const auto findNextSelectableTopic = [this, &isSelectableTopicIndex](size_t start, int direction) -> size_t {
            const size_t count = mTopicsList->getItemCount();
            if (count == 0)
                return MyGUI::ITEM_NONE;

            for (size_t step = 1; step <= count; ++step)
            {
                const size_t index = (start + count + (direction > 0 ? step : (count - (step % count)))) % count;
                if (isSelectableTopicIndex(index))
                    return index;
            }
            return MyGUI::ITEM_NONE;
        };

        if (mControllerNavUpDelay > 0.f && arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
            return true;

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (mChoices.size() > 0)
            {
                if (mChoices.size() == 1)
                    onChoiceActivated(mChoices[0].second);
                else if (mControllerChoice >= 0 && mControllerChoice < static_cast<int>(mChoices.size()))
                    onChoiceActivated(mChoices[mControllerChoice].second);
            }
            else if (isSelectableTopicIndex(mControllerFocus))
                onSelectListItem(mTopicsList->getItemNameAt(mControllerFocus), static_cast<int>(mControllerFocus));
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B && mChoices.empty())
        {
            onGoodbyeActivated();
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (mChoices.size() > 0)
            {
                // In-dialogue choice (red text)
                mControllerChoice = std::clamp(mControllerChoice - 1, 0, static_cast<int>(mChoices.size()) - 1);
                mHistory->setFocusItem(mChoiceStyles.at(mControllerChoice));
            }
            else
            {
                if (mTopicsList->getItemCount() == 0)
                    return true;
                if (!isSelectableTopicIndex(mControllerFocus))
                {
                    const size_t first = findFirstSelectableTopic();
                    if (first == MyGUI::ITEM_NONE)
                        return true;
                    mControllerFocus = first;
                }
                setControllerFocus(mControllerFocus, false);
                const size_t next = findNextSelectableTopic(mControllerFocus, -1);
                if (next == MyGUI::ITEM_NONE)
                    return true;
                mControllerFocus = next;
                setControllerFocus(next, true);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (mChoices.size() > 0)
            {
                // In-dialogue choice (red text)
                mControllerChoice = std::clamp(mControllerChoice + 1, 0, static_cast<int>(mChoices.size()) - 1);
                mHistory->setFocusItem(mChoiceStyles.at(mControllerChoice));
            }
            else
            {
                if (mTopicsList->getItemCount() == 0)
                    return true;
                if (!isSelectableTopicIndex(mControllerFocus))
                {
                    const size_t first = findFirstSelectableTopic();
                    if (first == MyGUI::ITEM_NONE)
                        return true;
                    mControllerFocus = first;
                }
                setControllerFocus(mControllerFocus, false);
                const size_t next = findNextSelectableTopic(mControllerFocus, 1);
                if (next == MyGUI::ITEM_NONE)
                    return true;
                mControllerFocus = next;
                setControllerFocus(next, true);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER && mChoices.size() == 0)
        {
            if (mTopicsList->getItemCount() == 0)
                return true;
            if (!isSelectableTopicIndex(mControllerFocus))
            {
                const size_t first = findFirstSelectableTopic();
                if (first == MyGUI::ITEM_NONE)
                    return true;
                mControllerFocus = first;
            }
            setControllerFocus(mControllerFocus, false);
            size_t next = mControllerFocus;
            for (int i = 0; i < 5; ++i)
            {
                next = findNextSelectableTopic(next, -1);
                if (next == MyGUI::ITEM_NONE)
                    return true;
            }
            mControllerFocus = next;
            setControllerFocus(mControllerFocus, true);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER && mChoices.size() == 0)
        {
            if (mTopicsList->getItemCount() == 0)
                return true;
            if (!isSelectableTopicIndex(mControllerFocus))
            {
                const size_t first = findFirstSelectableTopic();
                if (first == MyGUI::ITEM_NONE)
                    return true;
                mControllerFocus = first;
            }
            setControllerFocus(mControllerFocus, false);
            size_t next = mControllerFocus;
            for (int i = 0; i < 5; ++i)
            {
                next = findNextSelectableTopic(next, 1);
                if (next == MyGUI::ITEM_NONE)
                    return true;
            }
            mControllerFocus = next;
            setControllerFocus(mControllerFocus, true);
        }

        return true;
    }

    bool DialogueWindow::onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg)
    {
        if (arg.axis == SDL_CONTROLLER_AXIS_RIGHTY)
            return true;

        const bool isTrigger
            = arg.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT || arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT;
        if (!isTrigger)
            return false;

        if (!mScrollBar->getVisible())
            return true;

        const int direction = (arg.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) ? 1 : -1;
        onMouseWheel(nullptr, direction);
        return true;
    }
}
