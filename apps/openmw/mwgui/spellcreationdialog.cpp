#include "spellcreationdialog.hpp"

#include <algorithm>
#include <format>
#include <sstream>

#include <MyGUI_Button.h>
#include <MyGUI_EditBox.h>
#include <MyGUI_Gui.h>
#include <MyGUI_ImageBox.h>
#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_ScrollBar.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/widgets/list.hpp>

#include <components/esm3/loadgmst.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/store.hpp"

#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/spellutil.hpp"

#include "class.hpp"
#include "textcolours.hpp"
#include "tooltips.hpp"

namespace
{

    bool sortMagicEffects(const ESM::MagicEffect* effect1, const ESM::MagicEffect* effect2)
    {
        return effect1->mName < effect2->mName;
    }

    MyGUI::IntCoord getCenteredWindowCoord(const MyGUI::IntSize& viewSize, const MyGUI::IntSize& windowSize)
    {
        const int w = windowSize.width;
        const int h = windowSize.height;
        const int x = (viewSize.width - w) / 2;
        const int y = (viewSize.height - h) / 2;
        return { x, y, w, h };
    }

    void init(ESM::ENAMstruct& effect)
    {
        effect.mArea = 0;
        effect.mDuration = 0;
        effect.mEffectID = ESM::RefId();
        effect.mMagnMax = 0;
        effect.mMagnMin = 0;
        effect.mRange = 0;
    }

}

namespace MWGui
{

    EditEffectDialog::EditEffectDialog()
        : WindowModal("openmw_edit_effect.layout")
        , mEditing(false)
        , mMagicEffect(nullptr)
        , mConstantEffect(false)
    {
        init(mEffect);
        init(mOldEffect);

        getWidget(mCancelButton, "CancelButton");
        getWidget(mOkButton, "OkButton");
        getWidget(mDeleteButton, "DeleteButton");
        getWidget(mRangeButton, "RangeButton");
        getWidget(mMagnitudeMinValue, "MagnitudeMinValue");
        getWidget(mMagnitudeMaxValue, "MagnitudeMaxValue");
        getWidget(mDurationValue, "DurationValue");
        getWidget(mAreaValue, "AreaValue");
        getWidget(mMagnitudeMinSlider, "MagnitudeMinSlider");
        getWidget(mMagnitudeMaxSlider, "MagnitudeMaxSlider");
        getWidget(mDurationSlider, "DurationSlider");
        getWidget(mAreaSlider, "AreaSlider");
        getWidget(mEffectImage, "EffectImage");
        getWidget(mEffectName, "EffectName");
        getWidget(mAreaText, "AreaText");
        getWidget(mDurationBox, "DurationBox");
        getWidget(mAreaBox, "AreaBox");
        getWidget(mMagnitudeBox, "MagnitudeBox");

        mRangeButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditEffectDialog::onRangeButtonClicked);
        mOkButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditEffectDialog::onOkButtonClicked);
        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditEffectDialog::onCancelButtonClicked);
        mDeleteButton->eventMouseButtonClick += MyGUI::newDelegate(this, &EditEffectDialog::onDeleteButtonClicked);

        mMagnitudeMinSlider->eventScrollChangePosition
            += MyGUI::newDelegate(this, &EditEffectDialog::onMagnitudeMinChanged);
        mMagnitudeMaxSlider->eventScrollChangePosition
            += MyGUI::newDelegate(this, &EditEffectDialog::onMagnitudeMaxChanged);
        mDurationSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &EditEffectDialog::onDurationChanged);
        mAreaSlider->eventScrollChangePosition += MyGUI::newDelegate(this, &EditEffectDialog::onAreaChanged);

        if (Settings::gui().mControllerMenus)
        {
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mB = "#{Interface:Cancel}";
            mControllerButtons.mX = "#{Interface:OK}";

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

    void EditEffectDialog::setConstantEffect(bool constant)
    {
        mConstantEffect = constant;
    }

    void EditEffectDialog::onOpen()
    {
        WindowModal::onOpen();
        center();
        updateRangeButtonWidth();
    }

    bool EditEffectDialog::exit()
    {
        if (mEditing)
            eventEffectModified(mOldEffect);
        else
            eventEffectRemoved(mEffect);
        return true;
    }

    void EditEffectDialog::newEffect(const ESM::MagicEffect* effect)
    {
        bool allowSelf = (effect->mData.mFlags & ESM::MagicEffect::CastSelf) != 0 || mConstantEffect;
        bool allowTouch = (effect->mData.mFlags & ESM::MagicEffect::CastTouch) && !mConstantEffect;

        setMagicEffect(effect);
        mEditing = false;

        mDeleteButton->setVisible(false);

        mEffect.mRange = ESM::RT_Self;
        if (!allowSelf)
            mEffect.mRange = ESM::RT_Touch;
        if (!allowTouch)
            mEffect.mRange = ESM::RT_Target;
        mEffect.mMagnMin = 1;
        mEffect.mMagnMax = 1;
        mEffect.mDuration = 1;
        mEffect.mArea = 0;
        mEffect.mSkill = ESM::RefId();
        mEffect.mAttribute = ESM::RefId();
        eventEffectAdded(mEffect);

        onRangeButtonClicked(mRangeButton);

        mMagnitudeMinSlider->setScrollPosition(0);
        mMagnitudeMaxSlider->setScrollPosition(0);
        mAreaSlider->setScrollPosition(0);
        mDurationSlider->setScrollPosition(0);

        mDurationValue->setCaption("1");
        mMagnitudeMinValue->setCaption("1");
        const std::string to{ MWBase::Environment::get().getWindowManager()->getGameSettingString("sTo", "-") };

        mMagnitudeMaxValue->setCaption(to + " 1");
        mAreaValue->setCaption("0");

        if (Settings::gui().mControllerMenus)
        {
            mRangeButton->setStateSelected(true);
            mDeleteButton->setStateSelected(false);
            mOkButton->setStateSelected(false);
            mCancelButton->setStateSelected(false);
            mControllerFocus = 0;
            updateControllerFocus(-1, mControllerFocus);
        }

        setVisible(true);
    }

    void EditEffectDialog::editEffect(ESM::ENAMstruct effect)
    {
        const ESM::MagicEffect* magicEffect
            = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(effect.mEffectID);

        setMagicEffect(magicEffect);
        mOldEffect = effect;
        mEffect = effect;
        mEditing = true;

        mDeleteButton->setVisible(true);

        mMagnitudeMinSlider->setScrollPosition(effect.mMagnMin - 1);
        mMagnitudeMaxSlider->setScrollPosition(effect.mMagnMax - 1);
        mAreaSlider->setScrollPosition(effect.mArea);
        mDurationSlider->setScrollPosition(effect.mDuration - 1);

        if (mEffect.mRange == ESM::RT_Self)
            mRangeButton->setCaptionWithReplacing("#{sRangeSelf}");
        else if (mEffect.mRange == ESM::RT_Target)
            mRangeButton->setCaptionWithReplacing("#{sRangeTarget}");
        else if (mEffect.mRange == ESM::RT_Touch)
            mRangeButton->setCaptionWithReplacing("#{sRangeTouch}");

        updateRangeButtonWidth();
        onMagnitudeMinChanged(mMagnitudeMinSlider, effect.mMagnMin - 1);
        onMagnitudeMaxChanged(mMagnitudeMinSlider, effect.mMagnMax - 1);
        onAreaChanged(mAreaSlider, effect.mArea);
        onDurationChanged(mDurationSlider, effect.mDuration - 1);
        eventEffectModified(mEffect);

        if (Settings::gui().mControllerMenus)
        {
            mRangeButton->setStateSelected(true);
            mDeleteButton->setStateSelected(false);
            mOkButton->setStateSelected(false);
            mCancelButton->setStateSelected(false);
            mControllerFocus = 0;
            updateControllerFocus(-1, mControllerFocus);
        }

        updateBoxes();
    }

    void EditEffectDialog::setMagicEffect(const ESM::MagicEffect* effect)
    {
        mEffectImage->setImageTexture(Misc::ResourceHelpers::correctIconPath(
            VFS::Path::toNormalized(effect->mIcon), *MWBase::Environment::get().getResourceSystem()->getVFS()));

        mEffectName->setCaption(effect->mName);

        mEffect.mEffectID = effect->mId;

        mMagicEffect = effect;

        updateBoxes();
    }

    void EditEffectDialog::updateBoxes()
    {
        static int startY = mMagnitudeBox->getPosition().top;
        int curY = startY;

        mMagnitudeBox->setVisible(false);
        mDurationBox->setVisible(false);
        mAreaBox->setVisible(false);

        if (!(mMagicEffect->mData.mFlags & ESM::MagicEffect::NoMagnitude))
        {
            mMagnitudeBox->setPosition(mMagnitudeBox->getPosition().left, curY);
            mMagnitudeBox->setVisible(true);
            curY += mMagnitudeBox->getSize().height;
        }
        if (!(mMagicEffect->mData.mFlags & ESM::MagicEffect::NoDuration) && mConstantEffect == false)
        {
            mDurationBox->setPosition(mDurationBox->getPosition().left, curY);
            mDurationBox->setVisible(true);
            curY += mDurationBox->getSize().height;
        }
        if (mEffect.mRange != ESM::RT_Self)
        {
            mAreaBox->setPosition(mAreaBox->getPosition().left, curY);
            mAreaBox->setVisible(true);
            // curY += mAreaBox->getSize().height;
        }

        if (Settings::gui().mControllerMenus)
        {
            mButtons.clear();
            mButtons.emplace_back(mRangeButton);
            if (mMagnitudeBox->getVisible())
            {
                mButtons.emplace_back(mMagnitudeMinValue);
                mButtons.emplace_back(mMagnitudeMaxValue);
            }
            if (mDurationBox->getVisible())
                mButtons.emplace_back(mDurationValue);
            if (mAreaBox->getVisible())
                mButtons.emplace_back(mAreaValue);
            if (mDeleteButton->getVisible())
                mButtons.emplace_back(mDeleteButton);
            mButtons.emplace_back(mOkButton);
            mButtons.emplace_back(mCancelButton);

            if (mBottomRowReturnButton)
            {
                const auto it = std::find(mButtons.begin(), mButtons.end(), mBottomRowReturnButton);
                if (it == mButtons.end())
                    mBottomRowReturnButton = nullptr;
            }
        }
    }

    void EditEffectDialog::onRangeButtonClicked(MyGUI::Widget* /*sender*/)
    {
        mEffect.mRange = (mEffect.mRange + 1) % 3;

        // cycle through range types until we find something that's allowed
        // does not handle the case where nothing is allowed (this should be prevented before opening the Add Effect
        // dialog)
        bool allowSelf = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastSelf) != 0 || mConstantEffect;
        bool allowTouch = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastTouch) && !mConstantEffect;
        bool allowTarget = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastTarget) && !mConstantEffect;
        if (mEffect.mRange == ESM::RT_Self && !allowSelf)
            mEffect.mRange = (mEffect.mRange + 1) % 3;
        if (mEffect.mRange == ESM::RT_Touch && !allowTouch)
            mEffect.mRange = (mEffect.mRange + 1) % 3;
        if (mEffect.mRange == ESM::RT_Target && !allowTarget)
            mEffect.mRange = (mEffect.mRange + 1) % 3;

        if (mEffect.mRange == ESM::RT_Self)
        {
            mAreaSlider->setScrollPosition(0);
            onAreaChanged(mAreaSlider, 0);
        }

        if (mEffect.mRange == ESM::RT_Self)
            mRangeButton->setCaptionWithReplacing("#{sRangeSelf}");
        else if (mEffect.mRange == ESM::RT_Target)
            mRangeButton->setCaptionWithReplacing("#{sRangeTarget}");
        else if (mEffect.mRange == ESM::RT_Touch)
            mRangeButton->setCaptionWithReplacing("#{sRangeTouch}");

        updateRangeButtonWidth();
        updateBoxes();
        eventEffectModified(mEffect);
    }

    void EditEffectDialog::updateRangeButtonWidth()
    {
        if (!mRangeButton)
            return;

        const MyGUI::UString currentCaption = mRangeButton->getCaption();
        int padding = 24;
        if (mRangeButton->isUserString("TextPadding"))
        {
            const std::string paddingText(mRangeButton->getUserString("TextPadding"));
            std::istringstream paddingStream(paddingText);
            int padWidth = 0;
            int padHeight = 0;
            if (paddingStream >> padWidth >> padHeight)
                padding = padWidth;
        }

        const int currentWidth = mRangeButton->getTextSize().width + padding;
        if (!shouldForceRangeWidth())
        {
            mRangeButton->setSize(currentWidth, mRangeButton->getSize().height);
            return;
        }
        const std::string_view targetText
            = MWBase::Environment::get().getWindowManager()->getGameSettingString("sRangeTarget", "Target");
        mRangeButton->setCaption(MyGUI::UString(targetText));
        const int targetWidth = mRangeButton->getTextSize().width + std::max(padding, 0);
        mRangeButton->setCaption(currentCaption);

        if (mRangeButton->getSize().width < targetWidth)
            mRangeButton->setSize(targetWidth, mRangeButton->getSize().height);
    }

    bool EditEffectDialog::shouldForceRangeWidth() const
    {
        if (!mMagicEffect)
            return false;

        const bool allowSelf = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastSelf) != 0 || mConstantEffect;
        const bool allowTouch = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastTouch) && !mConstantEffect;
        const bool allowTarget = (mMagicEffect->mData.mFlags & ESM::MagicEffect::CastTarget) && !mConstantEffect;
        const int allowed = static_cast<int>(allowSelf) + static_cast<int>(allowTouch) + static_cast<int>(allowTarget);
        return allowed > 1;
    }

    void EditEffectDialog::onDeleteButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);

        eventEffectRemoved(mEffect);
    }

    void EditEffectDialog::onOkButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
    }

    void EditEffectDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        setVisible(false);
        exit();
    }

    void EditEffectDialog::setSkill(ESM::RefId skill)
    {
        mEffect.mSkill = skill;
        eventEffectModified(mEffect);
    }

    void EditEffectDialog::setAttribute(ESM::RefId attribute)
    {
        mEffect.mAttribute = attribute;
        eventEffectModified(mEffect);
    }

    void EditEffectDialog::onMagnitudeMinChanged(MyGUI::ScrollBar* sender, size_t pos)
    {
        mMagnitudeMinValue->setCaption(MyGUI::utility::toString(pos + 1));
        mEffect.mMagnMin = static_cast<int32_t>(pos + 1);

        // trigger the check again (see below)
        onMagnitudeMaxChanged(mMagnitudeMaxSlider, mMagnitudeMaxSlider->getScrollPosition());
        eventEffectModified(mEffect);
    }

    void EditEffectDialog::onMagnitudeMaxChanged(MyGUI::ScrollBar* sender, size_t pos)
    {
        // make sure the max value is actually larger or equal than the min value
        size_t magnMin
            = std::abs(mEffect.mMagnMin); // should never be < 0, this is just here to avoid the compiler warning
        if (pos + 1 < magnMin)
        {
            pos = mEffect.mMagnMin - 1;
            sender->setScrollPosition(pos);
        }

        mEffect.mMagnMax = static_cast<int32_t>(pos + 1);
        const std::string to{ MWBase::Environment::get().getWindowManager()->getGameSettingString("sTo", "-") };

        mMagnitudeMaxValue->setCaption(to + " " + MyGUI::utility::toString(pos + 1));

        eventEffectModified(mEffect);
    }

    void EditEffectDialog::onDurationChanged(MyGUI::ScrollBar* sender, size_t pos)
    {
        mDurationValue->setCaption(MyGUI::utility::toString(pos + 1));
        mEffect.mDuration = static_cast<int32_t>(pos + 1);
        eventEffectModified(mEffect);
    }

    void EditEffectDialog::onAreaChanged(MyGUI::ScrollBar* sender, size_t pos)
    {
        mAreaValue->setCaption(MyGUI::utility::toString(pos));
        mEffect.mArea = static_cast<int32_t>(pos);
        eventEffectModified(mEffect);
    }

    bool EditEffectDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        int prevFocus = mControllerFocus;
        mControllerFocus = std::clamp(mControllerFocus, 0, static_cast<int>(mButtons.size()) - 1);
        MyGUI::TextBox* button = mButtons[mControllerFocus];

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (button == mRangeButton)
                onRangeButtonClicked(mRangeButton);
            else if (button == mCancelButton)
                onCancelButtonClicked(mCancelButton);
            else if (button == mOkButton)
                onOkButtonClicked(mOkButton);
            else if (button == mDeleteButton)
                onDeleteButtonClicked(mDeleteButton);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_B)
            onCancelButtonClicked(mCancelButton);
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onOkButtonClicked(mOkButton);
            MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (mControllerFocus == 0)
                mControllerFocus = static_cast<int>(mButtons.size()) - 2;
            else if (button == mCancelButton && mDeleteButton->getVisible())
                mControllerFocus -= 3;
            else if (button == mCancelButton || (button == mOkButton && mDeleteButton->getVisible()))
                mControllerFocus -= 2;
            else
                mControllerFocus = std::max(mControllerFocus - 1, 0);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (button == mDeleteButton || button == mOkButton || button == mCancelButton)
                mControllerFocus = 0;
            else
            {
                const int bottomStart = [this]() -> int {
                    for (size_t i = 0; i < mButtons.size(); ++i)
                    {
                        MyGUI::TextBox* entry = mButtons[i];
                        if (entry == mDeleteButton || entry == mOkButton || entry == mCancelButton)
                            return static_cast<int>(i);
                    }
                    return -1;
                }();
                if (bottomStart > 0 && mControllerFocus == bottomStart - 1 && mBottomRowReturnButton)
                {
                    const auto it = std::find(mButtons.begin(), mButtons.end(), mBottomRowReturnButton);
                    if (it != mButtons.end())
                        mControllerFocus = static_cast<int>(std::distance(mButtons.begin(), it));
                    else
                        mControllerFocus++;
                }
                else
                    mControllerFocus++;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        {
            if (button == mMagnitudeMinValue)
            {
                mMagnitudeMinSlider->setScrollPosition(0);
                onMagnitudeMinChanged(nullptr, mMagnitudeMinSlider->getScrollPosition());
            }
            else if (button == mMagnitudeMaxValue)
            {
                mMagnitudeMaxSlider->setScrollPosition(mMagnitudeMinSlider->getScrollPosition());
                onMagnitudeMaxChanged(nullptr, mMagnitudeMaxSlider->getScrollPosition());
            }
            else if (button == mDurationValue)
            {
                mDurationSlider->setScrollPosition(0);
                onDurationChanged(nullptr, mDurationSlider->getScrollPosition());
            }
            else if (button == mAreaValue)
            {
                mAreaSlider->setScrollPosition(0);
                onAreaChanged(nullptr, mAreaSlider->getScrollPosition());
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            if (button == mMagnitudeMinValue)
            {
                mMagnitudeMinSlider->setScrollPosition(mMagnitudeMaxSlider->getScrollPosition());
                onMagnitudeMinChanged(nullptr, mMagnitudeMinSlider->getScrollPosition());
            }
            else if (button == mMagnitudeMaxValue)
            {
                mMagnitudeMaxSlider->setScrollPosition(mMagnitudeMaxSlider->getScrollRange() - 1);
                onMagnitudeMaxChanged(nullptr, mMagnitudeMaxSlider->getScrollPosition());
            }
            else if (button == mDurationValue)
            {
                mDurationSlider->setScrollPosition(mDurationSlider->getScrollRange() - 1);
                onDurationChanged(nullptr, mDurationSlider->getScrollPosition());
            }
            else if (button == mAreaValue)
            {
                mAreaSlider->setScrollPosition(mAreaSlider->getScrollRange() - 1);
                onAreaChanged(nullptr, mAreaSlider->getScrollPosition());
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (button == mRangeButton)
                onRangeButtonClicked(mRangeButton);
            else if (button == mCancelButton)
                mControllerFocus--;
            else if (button == mOkButton && mDeleteButton->getVisible())
                mControllerFocus--;
            else if (button == mMagnitudeMinValue)
            {
                const size_t pos = mMagnitudeMinSlider->getScrollPosition();
                mMagnitudeMinSlider->setScrollPosition(pos > 0 ? pos - 1 : 0);
                onMagnitudeMinChanged(nullptr, mMagnitudeMinSlider->getScrollPosition());
            }
            else if (button == mMagnitudeMaxValue)
            {
                const size_t minPos = mMagnitudeMinSlider->getScrollPosition();
                const size_t maxPos = mMagnitudeMaxSlider->getScrollPosition();
                const size_t newPos = std::max(maxPos > 0 ? maxPos - 1 : 0, minPos);
                mMagnitudeMaxSlider->setScrollPosition(newPos);
                onMagnitudeMaxChanged(nullptr, mMagnitudeMaxSlider->getScrollPosition());
            }
            else if (button == mDurationValue)
            {
                const size_t pos = mDurationSlider->getScrollPosition();
                mDurationSlider->setScrollPosition(pos > 0 ? pos - 1 : 0);
                onDurationChanged(nullptr, mDurationSlider->getScrollPosition());
            }
            else if (button == mAreaValue)
            {
                const size_t pos = mAreaSlider->getScrollPosition();
                mAreaSlider->setScrollPosition(pos > 0 ? pos - 1 : 0);
                onAreaChanged(nullptr, mAreaSlider->getScrollPosition());
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            if (button == mRangeButton)
                onRangeButtonClicked(mRangeButton);
            else if (button == mDeleteButton)
                mControllerFocus++;
            else if (button == mOkButton)
                mControllerFocus++;
            else if (button == mMagnitudeMinValue)
            {
                const size_t maxPos = mMagnitudeMaxSlider->getScrollPosition();
                const size_t pos = mMagnitudeMinSlider->getScrollPosition();
                mMagnitudeMinSlider->setScrollPosition(std::min(pos + 1, maxPos));
                onMagnitudeMinChanged(nullptr, mMagnitudeMinSlider->getScrollPosition());
            }
            else if (button == mMagnitudeMaxValue)
            {
                const size_t range = mMagnitudeMaxSlider->getScrollRange();
                const size_t minPos = mMagnitudeMinSlider->getScrollPosition();
                const size_t pos = mMagnitudeMaxSlider->getScrollPosition();
                const size_t maxAllowed = range > 0 ? range - 1 : 0;
                mMagnitudeMaxSlider->setScrollPosition(std::max(minPos, std::min(pos + 1, maxAllowed)));
                onMagnitudeMaxChanged(nullptr, mMagnitudeMaxSlider->getScrollPosition());
            }
            else if (button == mDurationValue)
            {
                const size_t range = mDurationSlider->getScrollRange();
                const size_t pos = mDurationSlider->getScrollPosition();
                const size_t maxAllowed = range > 0 ? range - 1 : 0;
                mDurationSlider->setScrollPosition(std::min(pos + 1, maxAllowed));
                onDurationChanged(nullptr, mDurationSlider->getScrollPosition());
            }
            else if (button == mAreaValue)
            {
                const size_t range = mAreaSlider->getScrollRange();
                const size_t pos = mAreaSlider->getScrollPosition();
                const size_t maxAllowed = range > 0 ? range - 1 : 0;
                mAreaSlider->setScrollPosition(std::min(pos + 1, maxAllowed));
                onAreaChanged(nullptr, mAreaSlider->getScrollPosition());
            }
        }

        if (prevFocus != mControllerFocus)
            updateControllerFocus(prevFocus, mControllerFocus);

        return true;
    }

    void EditEffectDialog::updateControllerFocus(int prevFocus, int newFocus)
    {
        const TextColours& textColours{ MWBase::Environment::get().getWindowManager()->getTextColours() };
        auto resetValueText = [&]() {
            if (mMagnitudeMinValue)
                mMagnitudeMinValue->setTextColour(textColours.normal);
            if (mMagnitudeMaxValue)
                mMagnitudeMaxValue->setTextColour(textColours.normal);
            if (mDurationValue)
                mDurationValue->setTextColour(textColours.normal);
            if (mAreaValue)
                mAreaValue->setTextColour(textColours.normal);
        };
        resetValueText();
        auto getSliderHighlightCoord = [](MyGUI::ScrollBar* slider) -> std::optional<MyGUI::IntCoord> {
            if (!slider)
                return std::nullopt;

            if (MyGUI::Widget* background = slider->findWidget("Background"))
                return background->getAbsoluteCoord();

            MyGUI::IntCoord sliderCoord = slider->getAbsoluteCoord();
            int leftMargin = 0;
            int rightMargin = 0;
            if (slider->isUserString("TrackRangeMargins"))
            {
                const std::string marginText(slider->getUserString("TrackRangeMargins"));
                std::istringstream marginStream(marginText);
                marginStream >> leftMargin >> rightMargin;
            }

            sliderCoord.left += leftMargin;
            sliderCoord.width = std::max(0, sliderCoord.width - leftMargin - rightMargin);
            return sliderCoord;
        };

        if (prevFocus >= 0 && prevFocus < static_cast<int>(mButtons.size()))
        {
            MyGUI::TextBox* button = mButtons[prevFocus];
            if (button == mMagnitudeMinValue || button == mMagnitudeMaxValue || button == mDurationValue
                || button == mAreaValue)
            {
                button->setTextColour(textColours.normal);
            }
            else
            {
                static_cast<MyGUI::Button*>(button)->setStateSelected(false);
            }
        }

        if (newFocus >= 0 && newFocus < static_cast<int>(mButtons.size()))
        {
            MyGUI::TextBox* button = mButtons[newFocus];
            if (button == mMagnitudeMinValue || button == mMagnitudeMaxValue || button == mDurationValue
                || button == mAreaValue)
            {
                button->setTextColour(textColours.link);
            }
            else
            {
                static_cast<MyGUI::Button*>(button)->setStateSelected(true);
            }

            if (button == mDeleteButton || button == mOkButton || button == mCancelButton)
                mBottomRowReturnButton = button;
        }

        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        if (newFocus < 0 || newFocus >= static_cast<int>(mButtons.size()))
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focus = mButtons[newFocus];
        std::optional<MyGUI::IntCoord> sliderCoord;
        if (focus == mMagnitudeMinValue)
            sliderCoord = getSliderHighlightCoord(mMagnitudeMinSlider);
        else if (focus == mMagnitudeMaxValue)
            sliderCoord = getSliderHighlightCoord(mMagnitudeMaxSlider);
        else if (focus == mDurationValue)
            sliderCoord = getSliderHighlightCoord(mDurationSlider);
        else if (focus == mAreaValue)
            sliderCoord = getSliderHighlightCoord(mAreaSlider);

        const MyGUI::IntCoord focusCoord = sliderCoord ? *sliderCoord : focus->getAbsoluteCoord();
        MyGUI::Widget* highlightParent = mControllerFocusHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        mControllerFocusHighlight->setCoord(
            focusCoord.left - baseCoord.left, focusCoord.top - baseCoord.top, focusCoord.width, focusCoord.height);
        mControllerFocusHighlight->setVisible(true);
    }

    // ------------------------------------------------------------------------------------------------

    SpellCreationDialog::SpellCreationDialog()
        : WindowBase("openmw_spellcreation_dialog.layout")
        , EffectEditorBase(EffectEditorBase::Spellmaking)
    {
        getWidget(mNameEdit, "NameEdit");
        getWidget(mMagickaCost, "MagickaCost");
        getWidget(mSuccessChance, "SuccessChance");
        getWidget(mAvailableEffectsList, "AvailableEffects");
        getWidget(mUsedEffectsView, "UsedEffects");
        getWidget(mPriceLabel, "PriceLabel");
        getWidget(mPlayerGold, "PlayerGold");
        getWidget(mBuyButton, "BuyButton");
        getWidget(mCancelButton, "CancelButton");

        mCancelButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SpellCreationDialog::onCancelButtonClicked);
        mBuyButton->eventMouseButtonClick += MyGUI::newDelegate(this, &SpellCreationDialog::onBuyButtonClicked);
        mNameEdit->eventEditSelectAccept += MyGUI::newDelegate(this, &SpellCreationDialog::onAccept);

        setWidgets(mAvailableEffectsList, mUsedEffectsView);

        if (Settings::gui().mControllerMenus)
        {
            mNameEdit->setEditStatic(true);
            mNameEdit->setNeedKeyFocus(true);

            MyGUI::Widget* highlightParent = mMainWidget->getClientWidget();
            if (!highlightParent)
                highlightParent = mMainWidget;
            mControllerFocusHighlight = highlightParent->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerFocusHighlight->setNeedMouseFocus(false);
            mControllerFocusHighlight->setDepth(1);
            mControllerFocusHighlight->setVisible(false);

            mControllerButtons = {};
            mControllerButtons.mA = "#{Interface:Select}";
            mControllerButtons.mX = "#{Interface:Buy}";
            mControllerButtons.mY = "#{Interface:Info}";
            mControllerButtons.mB = "#{Interface:Cancel}";
        }
    }

    void SpellCreationDialog::setPtr(const MWWorld::Ptr& actor)
    {
        if (actor.isEmpty() || !actor.getClass().isActor())
            throw std::runtime_error("Invalid argument in SpellCreationDialog::setPtr");

        mPtr = actor;
        mNameEdit->setCaption({});

        MWWorld::Ptr player = MWMechanics::getPlayer();
        int playerGold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);
        mPlayerGold->setCaptionWithReplacing(MyGUI::utility::toString(playerGold));

        startEditing();
        if (Settings::gui().mControllerMenus)
            setControllerFocusWidget(mNameEdit);
    }

    void SpellCreationDialog::onCancelButtonClicked(MyGUI::Widget* /*sender*/)
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(MWGui::GM_SpellCreation);
    }

    void SpellCreationDialog::onBuyButtonClicked(MyGUI::Widget* /*sender*/)
    {
        if (mEffects.size() <= 0)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage30}");
            return;
        }

        if (mNameEdit->getCaption().empty())
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage10}");
            return;
        }

        if (mMagickaCost->getCaption() == "0")
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sEnchantmentMenu8}");
            return;
        }

        MWWorld::Ptr player = MWMechanics::getPlayer();
        int playerGold = player.getClass().getContainerStore(player).count(MWWorld::ContainerStore::sGoldId);

        int price = MyGUI::utility::parseInt(mPriceLabel->getCaption());
        if (price > playerGold)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage18}");
            return;
        }

        mSpell.mName = mNameEdit->getCaption();

        player.getClass().getContainerStore(player).remove(MWWorld::ContainerStore::sGoldId, price);

        // add gold to NPC trading gold pool
        MWMechanics::CreatureStats& npcStats = mPtr.getClass().getCreatureStats(mPtr);
        npcStats.setGoldPool(npcStats.getGoldPool() + price);

        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Mysticism Hit"));

        const ESM::Spell* spell = MWBase::Environment::get().getESMStore()->insert(mSpell);

        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        MWMechanics::Spells& spells = stats.getSpells();
        spells.add(spell->mId);

        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_SpellCreation);
    }

    void SpellCreationDialog::onAccept(MyGUI::EditBox* sender)
    {
        onBuyButtonClicked(sender);

        // To do not spam onAccept() again and again
        MWBase::Environment::get().getWindowManager()->injectKeyRelease(MyGUI::KeyCode::None);
    }

    void SpellCreationDialog::onOpen()
    {
        if (MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>(false))
        {
            const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
            window->setCoord(getCenteredWindowCoord(viewSize, window->getSize()));
        }
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (Settings::gui().mControllerMenus)
        {
            mKeyboardWasVisible = false;
            setControllerFocusWidget(mNameEdit);
        }
        else
            winMgr->setKeyFocusWidget(mNameEdit);
    }

    void SpellCreationDialog::onFrame(float dt)
    {
        checkReferenceAvailable();

        if (!Settings::gui().mControllerMenus)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool keyboardVisible = winMgr->isVirtualKeyboardVisible();
        if (mKeyboardWasVisible && !keyboardVisible)
            setControllerFocusWidget(mNameEdit);
        mKeyboardWasVisible = keyboardVisible;

        if (mControllerEffectsFocusActive)
            setControllerEffectsFocus(true);

        updateControllerFocusHighlight();
        updateEditStaticState();
    }

    void SpellCreationDialog::onReferenceUnavailable()
    {
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_Dialogue);
        MWBase::Environment::get().getWindowManager()->removeGuiMode(GM_SpellCreation);
    }

    void SpellCreationDialog::notifyEffectsChanged()
    {
        if (mEffects.empty())
        {
            mMagickaCost->setCaption("0");
            mPriceLabel->setCaption("0");
            mSuccessChance->setCaption("0");
            if (Settings::gui().mControllerMenus)
            {
                if (mControllerEffectsFocusActive)
                    setControllerEffectsFocus(true);
                else if (!MWBase::Environment::get().getWindowManager()->isVirtualKeyboardVisible())
                    setControllerFocusWidget(mNameEdit);
            }
            return;
        }

        float y = 0;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();

        for (const ESM::ENAMstruct& effect : mEffects)
        {
            y += std::max(
                1.f, MWMechanics::calcEffectCost(effect, nullptr, MWMechanics::EffectCostMethod::PlayerSpell));

            if (effect.mRange == ESM::RT_Target)
                y *= 1.5;
        }

        mSpell.mEffects.populate(mEffects);
        mSpell.mData.mCost = int(y);
        mSpell.mData.mType = ESM::Spell::ST_Spell;
        mSpell.mData.mFlags = 0;

        mMagickaCost->setCaption(MyGUI::utility::toString(int(y)));

        float fSpellMakingValueMult = store.get<ESM::GameSetting>().find("fSpellMakingValueMult")->mValue.getFloat();

        int price = std::max(1, static_cast<int>(y * fSpellMakingValueMult));
        price = MWBase::Environment::get().getMechanicsManager()->getBarterOffer(mPtr, price, true);

        mPriceLabel->setCaption(MyGUI::utility::toString(int(price)));

        float chance = MWMechanics::calcSpellBaseSuccessChance(&mSpell, MWMechanics::getPlayer(), nullptr);

        int intChance = std::min(100, int(chance));
        mSuccessChance->setCaption(MyGUI::utility::toString(intChance));

        if (Settings::gui().mControllerMenus)
        {
            if (mControllerEffectsFocusActive)
                setControllerEffectsFocus(true);
            else if (!MWBase::Environment::get().getWindowManager()->isVirtualKeyboardVisible())
                setControllerFocusWidget(mNameEdit);
        }
    }

    bool SpellCreationDialog::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            onCancelButtonClicked(mCancelButton);
            return true;
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            onBuyButtonClicked(mBuyButton);
            return true;
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (!mControllerEffectsFocusActive && MyGUI::InputManager::getInstance().getKeyFocusWidget() == mNameEdit)
            {
                openVirtualKeyboard(mNameEdit);
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (!mControllerEffectsFocusActive && MyGUI::InputManager::getInstance().getKeyFocusWidget() == mNameEdit)
                return true;
            if (mControllerEffectsFocusActive && isControllerEffectsAtTop())
            {
                if (isControllerEffectsRightColumn())
                {
                    setControllerEffectsRightColumn(false);
                    return true;
                }

                setControllerFocusWidget(mNameEdit);
                return true;
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (!mControllerEffectsFocusActive && MyGUI::InputManager::getInstance().getKeyFocusWidget() == mNameEdit)
            {
                setControllerEffectsRightColumn(false);
                setControllerEffectsFocusActive(true);
                return true;
            }
        }

        return EffectEditorBase::onControllerButtonEvent(arg);
    }

    void SpellCreationDialog::openVirtualKeyboard(MyGUI::EditBox* edit)
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

    void SpellCreationDialog::setControllerFocusWidget(MyGUI::Widget* widget)
    {
        if (!widget)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (mControllerEffectsFocusActive)
            setControllerEffectsFocusActive(false);
        else
            setControllerEffectsFocus(false);
        winMgr->setKeyFocusWidget(widget);
        if (Settings::gui().mControllerMenus)
        {
            // Spell creation is fully controller-driven; keep the cursor hidden.
            winMgr->setCursorVisible(false);
            // Keep the hidden mouse aligned with controller focus so stale hover styles/tooltips don't stick.
            MWBase::Environment::get().getInputManager()->warpMouseToWidget(widget);
        }
        updateControllerFocusHighlight();
    }

    void SpellCreationDialog::setControllerEffectsFocusActive(bool active)
    {
        if (mControllerEffectsFocusActive == active)
            return;

        mControllerEffectsFocusActive = active;
        setControllerEffectsFocus(active);

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        if (active)
        {
            winMgr->setControllerTooltipVisible(Settings::gui().mControllerTooltips);
        }
        if (Settings::gui().mControllerMenus)
            winMgr->setCursorVisible(false);

        updateControllerFocusHighlight();
    }

    void SpellCreationDialog::updateControllerFocusHighlight()
    {
        if (!mControllerFocusHighlight || !Settings::gui().mControllerMenus)
            return;

        if (mControllerEffectsFocusActive)
        {
            mControllerFocusHighlight->setVisible(false);
            return;
        }

        MyGUI::Widget* focus = MyGUI::InputManager::getInstance().getKeyFocusWidget();
        if (focus != mNameEdit)
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

    void SpellCreationDialog::updateEditStaticState()
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();
        const bool shouldStatic = !winMgr->isVirtualKeyboardVisible();
        if (mNameEdit->getEditStatic() != shouldStatic)
            mNameEdit->setEditStatic(shouldStatic);
    }

    MyGUI::Widget* SpellCreationDialog::getControllerFocusTooltipWidget() const
    {
        if (mControllerEffectsFocusActive)
            return getControllerEffectsTooltipWidget();
        return nullptr;
    }

    // ------------------------------------------------------------------------------------------------

    EffectEditorBase::EffectEditorBase(Type type)
        : mAvailableEffectsList(nullptr)
        , mUsedEffectsView(nullptr)
        , mAddEffectDialog()
        , mSelectedEffect(0)
        , mSelectedKnownEffectId(ESM::RefId())
        , mConstantEffect(false)
        , mType(type)
    {
        mAddEffectDialog.eventEffectAdded += MyGUI::newDelegate(this, &EffectEditorBase::onEffectAdded);
        mAddEffectDialog.eventEffectModified += MyGUI::newDelegate(this, &EffectEditorBase::onEffectModified);
        mAddEffectDialog.eventEffectRemoved += MyGUI::newDelegate(this, &EffectEditorBase::onEffectRemoved);

        mAddEffectDialog.setVisible(false);
    }

    EffectEditorBase::~EffectEditorBase() = default;

    void EffectEditorBase::startEditing()
    {
        // get the list of magic effects that are known to the player

        MWWorld::Ptr player = MWMechanics::getPlayer();
        MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
        MWMechanics::Spells& spells = stats.getSpells();

        std::vector<const ESM::MagicEffect*> knownEffects;

        for (const ESM::Spell* spell : spells)
        {
            // only normal spells count
            if (spell->mData.mType != ESM::Spell::ST_Spell)
                continue;

            for (const ESM::IndexedENAMstruct& effectInfo : spell->mEffects.mList)
            {
                const ESM::MagicEffect* effect = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(
                    effectInfo.mData.mEffectID);

                // skip effects that do not allow spellmaking/enchanting
                int requiredFlags
                    = (mType == Spellmaking) ? ESM::MagicEffect::AllowSpellmaking : ESM::MagicEffect::AllowEnchanting;
                if (!(effect->mData.mFlags & requiredFlags))
                    continue;

                if (std::find(knownEffects.begin(), knownEffects.end(), effect) == knownEffects.end())
                    knownEffects.push_back(effect);
            }
        }

        std::sort(knownEffects.begin(), knownEffects.end(), sortMagicEffects);

        mAvailableEffectsList->clear();

        int i = 0;
        for (const auto effect : knownEffects)
        {
            mAvailableEffectsList->addItem(effect->mName);
            mButtonMapping[i] = effect->mId;
            ++i;
        }
        if (Settings::gui().mControllerMenus)
            mAvailableEffectsList->setProperty("ControllerHighlightSkin", "ControllerHighlight");
        else
            mAvailableEffectsList->setProperty("ControllerHighlightSkin", "");

        mAvailableEffectsList->adjustSize();
        mAvailableEffectsList->scrollToTop();

        mAvailableButtons.clear();
        for (const auto effect : knownEffects)
        {
            MyGUI::Button* w = mAvailableEffectsList->getItemWidget(effect->mName);
            mAvailableButtons.emplace_back(w);

            ToolTips::createMagicEffectToolTip(w, effect->mId);
        }

        mEffects.clear();
        updateEffectsView();

        if (Settings::gui().mControllerMenus)
        {
            mAvailableFocus = 0;
            mEffectFocus = 0;
            mRightColumn = false;
            if (!mAvailableButtons.empty() && mType == Enchanting)
            {
                MWBase::WindowManager& winMgr = *MWBase::Environment::get().getWindowManager();
                mAvailableButtons[0]->setStateSelected(true);
                if (Settings::gui().mControllerMenus)
                    mAvailableEffectsList->setItemHighlightVisible(0, true);
                if (winMgr.getControllerTooltipVisible())
                    MWBase::Environment::get().getInputManager()->warpMouseToWidget(mAvailableButtons[0]);
            }
        }
    }

    void EffectEditorBase::setWidgets(Gui::MWList* availableEffectsList, MyGUI::ScrollView* usedEffectsView)
    {
        mAvailableEffectsList = availableEffectsList;
        mUsedEffectsView = usedEffectsView;

        mAvailableEffectsList->eventWidgetSelected
            += MyGUI::newDelegate(this, &EffectEditorBase::onAvailableEffectClicked);
    }

    void EffectEditorBase::onSelectAttribute()
    {
        const ESM::MagicEffect* effect
            = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(mSelectedKnownEffectId);

        mAddEffectDialog.newEffect(effect);
        mAddEffectDialog.setAttribute(mSelectAttributeDialog->getAttributeId());
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSelectAttributeDialog));
    }

    void EffectEditorBase::onSelectSkill()
    {
        const ESM::MagicEffect* effect
            = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(mSelectedKnownEffectId);

        mAddEffectDialog.newEffect(effect);
        mAddEffectDialog.setSkill(mSelectSkillDialog->getSkillId());
        MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSelectSkillDialog));
    }

    void EffectEditorBase::onAttributeOrSkillCancel()
    {
        if (mSelectSkillDialog != nullptr)
            MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSelectSkillDialog));
        if (mSelectAttributeDialog != nullptr)
            MWBase::Environment::get().getWindowManager()->removeDialog(std::move(mSelectAttributeDialog));
    }

    void EffectEditorBase::onAvailableEffectClicked(MyGUI::Widget* sender)
    {
        if (mEffects.size() >= 8)
        {
            MWBase::Environment::get().getWindowManager()->messageBox("#{sNotifyMessage28}");
            return;
        }

        int buttonId = *sender->getUserData<int>();
        mSelectedKnownEffectId = mButtonMapping[buttonId];

        const ESM::MagicEffect* effect
            = MWBase::Environment::get().getESMStore()->get<ESM::MagicEffect>().find(mSelectedKnownEffectId);

        bool allowSelf = (effect->mData.mFlags & ESM::MagicEffect::CastSelf) != 0 || mConstantEffect;
        bool allowTouch = (effect->mData.mFlags & ESM::MagicEffect::CastTouch) && !mConstantEffect;
        bool allowTarget = (effect->mData.mFlags & ESM::MagicEffect::CastTarget) && !mConstantEffect;

        if (!allowSelf && !allowTouch && !allowTarget)
            return; // TODO: Show an error message popup?

        if (effect->mData.mFlags & ESM::MagicEffect::TargetSkill)
        {
            mSelectSkillDialog = std::make_unique<SelectSkillDialog>();
            mSelectSkillDialog->eventCancel += MyGUI::newDelegate(this, &SpellCreationDialog::onAttributeOrSkillCancel);
            mSelectSkillDialog->eventItemSelected += MyGUI::newDelegate(this, &SpellCreationDialog::onSelectSkill);
            mSelectSkillDialog->setVisible(true);
        }
        else if (effect->mData.mFlags & ESM::MagicEffect::TargetAttribute)
        {
            mSelectAttributeDialog = std::make_unique<SelectAttributeDialog>();
            mSelectAttributeDialog->eventCancel
                += MyGUI::newDelegate(this, &SpellCreationDialog::onAttributeOrSkillCancel);
            mSelectAttributeDialog->eventItemSelected
                += MyGUI::newDelegate(this, &SpellCreationDialog::onSelectAttribute);
            mSelectAttributeDialog->setVisible(true);
        }
        else
        {
            for (const ESM::ENAMstruct& effectInfo : mEffects)
            {
                if (effectInfo.mEffectID == mSelectedKnownEffectId)
                {
                    MWBase::Environment::get().getWindowManager()->messageBox("#{sOnetypeEffectMessage}");
                    return;
                }
            }

            mAddEffectDialog.newEffect(effect);
        }
    }

    void EffectEditorBase::onEffectModified(ESM::ENAMstruct effect)
    {
        mEffects[mSelectedEffect] = effect;

        updateEffectsView();
    }

    void EffectEditorBase::onEffectRemoved(ESM::ENAMstruct effect)
    {
        mEffects.erase(mEffects.begin() + mSelectedEffect);
        updateEffectsView();
    }

    void EffectEditorBase::updateEffectsView()
    {
        MyGUI::EnumeratorWidgetPtr oldWidgets = mUsedEffectsView->getEnumerator();
        MyGUI::Gui::getInstance().destroyWidgets(oldWidgets);

        constexpr int minEffectHeight = 24;
        const int viewWidth = std::max(1, mUsedEffectsView->getWidth());
        const int viewHeight = std::max(1, mUsedEffectsView->getHeight());
        constexpr int scrollbarWidth = 18;

        mEffectButtons.clear();
        mEffectHighlights.clear();
        int i = 0;
        for (const ESM::ENAMstruct& effectInfo : mEffects)
        {
            Widgets::SpellEffectParams params;
            params.mEffectID = effectInfo.mEffectID;
            params.mSkill = effectInfo.mSkill;
            params.mAttribute = effectInfo.mAttribute;
            params.mDuration = effectInfo.mDuration;
            params.mMagnMin = effectInfo.mMagnMin;
            params.mMagnMax = effectInfo.mMagnMax;
            params.mRange = effectInfo.mRange;
            params.mArea = effectInfo.mArea;
            params.mIsConstant = mConstantEffect;

            MyGUI::Button* button = mUsedEffectsView->createWidget<MyGUI::Button>(
                {}, MyGUI::IntCoord(0, 0, viewWidth, minEffectHeight), MyGUI::Align::Default);
            button->setUserData(i);
            button->eventMouseButtonClick += MyGUI::newDelegate(this, &SpellCreationDialog::onEditEffect);
            button->setNeedMouseFocus(true);
            ToolTips::createMagicEffectToolTip(button, effectInfo.mEffectID);
            if (mType == Enchanting || mType == Spellmaking)
                button->setUserString("ForceCollapsedTooltip", "true");

            Widgets::MWSpellEffectPtr effect = button->createWidget<Widgets::MWSpellEffect>(
                "MW_EffectImage", MyGUI::IntCoord(0, 0, viewWidth, minEffectHeight), MyGUI::Align::Default);

            effect->setNeedMouseFocus(false);
            effect->setSpellEffect(params);
            effect->fitToWidth(viewWidth, true);

            MyGUI::Widget* highlight = nullptr;
            if (Settings::gui().mControllerMenus)
            {
                highlight = mUsedEffectsView->createWidget<MyGUI::Widget>(
                    "ControllerHighlight", MyGUI::IntCoord(0, 0, viewWidth, minEffectHeight), MyGUI::Align::Default);
                highlight->setNeedMouseFocus(false);
                highlight->setDepth(1);
                highlight->setVisible(false);
            }

            ++i;

            mEffectButtons.emplace_back(std::pair(effect, button));
            mEffectHighlights.emplace_back(highlight);
        }

        const auto relayoutRows = [&](int rowWidth) -> int {
            rowWidth = std::max(1, rowWidth);
            int y = 0;
            for (std::size_t index = 0; index < mEffectButtons.size(); ++index)
            {
                Widgets::MWSpellEffectPtr effect = mEffectButtons[index].first;
                MyGUI::Button* button = mEffectButtons[index].second;
                const int effectHeight = effect->fitToWidth(rowWidth, true);
                effect->setCoord(0, 0, rowWidth, effectHeight);
                button->setCoord(0, y, rowWidth, effectHeight);
                if (index < mEffectHighlights.size() && mEffectHighlights[index])
                    mEffectHighlights[index]->setCoord(0, y, rowWidth, effectHeight);
                y += effectHeight;
            }
            return y;
        };

        int rowWidth = viewWidth;
        int contentHeight = relayoutRows(rowWidth);
        if (contentHeight > viewHeight)
        {
            rowWidth = std::max(1, viewWidth - scrollbarWidth);
            contentHeight = relayoutRows(rowWidth);
        }

        // Canvas size must be expressed with VScroll disabled, otherwise MyGUI would expand the scroll area when the
        // scrollbar is hidden.
        mUsedEffectsView->setVisibleHScroll(false);
        mUsedEffectsView->setVisibleVScroll(false);
        mUsedEffectsView->setCanvasSize(MyGUI::IntSize(viewWidth, std::max(viewHeight, contentHeight)));
        mUsedEffectsView->setVisibleVScroll(true);

        if (Settings::gui().mControllerMenus)
            scrollUsedEffectsToFocused();

        notifyEffectsChanged();
    }

    void EffectEditorBase::onEffectAdded(ESM::ENAMstruct effect)
    {
        mEffects.push_back(effect);
        mSelectedEffect = static_cast<int>(mEffects.size() - 1);

        updateEffectsView();
    }

    void EffectEditorBase::onEditEffect(MyGUI::Widget* sender)
    {
        int id = *sender->getUserData<int>();

        mSelectedEffect = id;

        mAddEffectDialog.editEffect(mEffects[id]);
        mAddEffectDialog.setVisible(true);
    }

    void EffectEditorBase::setConstantEffect(bool constant)
    {
        mAddEffectDialog.setConstantEffect(constant);
        if (!mConstantEffect && constant)
            for (ESM::ENAMstruct& effect : mEffects)
                effect.mRange = ESM::RT_Self;
        mConstantEffect = constant;
    }

    bool EffectEditorBase::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            if (!mRightColumn && mAvailableFocus < mAvailableButtons.size())
            {
                onAvailableEffectClicked(mAvailableButtons[mAvailableFocus]);
                winMgr->playSound(ESM::RefId::stringRefId("Menu Click"));
            }
            else if (mRightColumn && mEffectFocus < mEffectButtons.size())
            {
                onEditEffect(mEffectButtons[mEffectFocus].second);
                winMgr->playSound(ESM::RefId::stringRefId("Menu Click"));
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            if (mRightColumn && !mEffectButtons.empty())
            {
                if (mEffectFocus < mEffectButtons.size())
                {
                    mEffectButtons[mEffectFocus].first->setStateSelected(false);
                    if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                        mEffectHighlights[mEffectFocus]->setVisible(false);
                }
                mEffectFocus = wrap(mEffectFocus, mEffectButtons.size(), -1);
                mEffectButtons[mEffectFocus].first->setStateSelected(true);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(true);
            }
            else if (!mRightColumn && !mAvailableButtons.empty())
            {
                if (mAvailableFocus < mAvailableButtons.size())
                {
                    mAvailableButtons[mAvailableFocus]->setStateSelected(false);
                    if (Settings::gui().mControllerMenus)
                        mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, false);
                }
                mAvailableFocus = wrap(mAvailableFocus, mAvailableButtons.size(), -1);
                mAvailableButtons[mAvailableFocus]->setStateSelected(true);
                if (Settings::gui().mControllerMenus)
                    mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, true);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (mRightColumn && !mEffectButtons.empty())
            {
                if (mEffectFocus < mEffectButtons.size())
                {
                    mEffectButtons[mEffectFocus].first->setStateSelected(false);
                    if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                        mEffectHighlights[mEffectFocus]->setVisible(false);
                }
                mEffectFocus = wrap(mEffectFocus, mEffectButtons.size(), 1);
                mEffectButtons[mEffectFocus].first->setStateSelected(true);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(true);
            }
            else if (!mRightColumn && !mAvailableButtons.empty())
            {
                if (mAvailableFocus < mAvailableButtons.size())
                {
                    mAvailableButtons[mAvailableFocus]->setStateSelected(false);
                    if (Settings::gui().mControllerMenus)
                        mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, false);
                }
                mAvailableFocus = wrap(mAvailableFocus, mAvailableButtons.size(), 1);
                mAvailableButtons[mAvailableFocus]->setStateSelected(true);
                if (Settings::gui().mControllerMenus)
                    mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, true);
            }
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT && mRightColumn)
        {
            mRightColumn = false;
            if (mEffectFocus < mEffectButtons.size())
            {
                mEffectButtons[mEffectFocus].first->setStateSelected(false);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(false);
            }
            if (mAvailableFocus < mAvailableButtons.size())
            {
                mAvailableButtons[mAvailableFocus]->setStateSelected(true);
                if (Settings::gui().mControllerMenus)
                    mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, true);
            }

            winMgr->setControllerTooltipVisible(Settings::gui().mControllerTooltips);
        }
        else if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT && !mRightColumn && mEffectButtons.size() > 0)
        {
            mRightColumn = true;
            if (mAvailableFocus < mAvailableButtons.size())
            {
                mAvailableButtons[mAvailableFocus]->setStateSelected(false);
                if (Settings::gui().mControllerMenus)
                    mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, false);
            }
            if (mEffectFocus < mEffectButtons.size())
            {
                mEffectButtons[mEffectFocus].first->setStateSelected(true);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(true);
            }

            winMgr->setControllerTooltipVisible(false);
        }
        else
            return true;

        // Scroll the list to keep the active item in view
        if (mAvailableFocus <= 5)
            mAvailableEffectsList->setViewOffset(0);
        else
        {
            const int lineHeight = Settings::gui().mFontSize + 3;
            mAvailableEffectsList->setViewOffset(-lineHeight * static_cast<int>(mAvailableFocus - 5));
        }
        if (mRightColumn)
            scrollUsedEffectsToFocused();

        if (!mRightColumn && mAvailableFocus < mAvailableButtons.size())
        {
            // Warp the mouse to the selected spell to show the tooltip
            if (winMgr->getControllerTooltipVisible())
                MWBase::Environment::get().getInputManager()->warpMouseToWidget(mAvailableButtons[mAvailableFocus]);
        }

        return true;
    }

    void EffectEditorBase::scrollUsedEffectsToFocused()
    {
        if (!mUsedEffectsView || !mRightColumn || mEffectFocus >= mEffectButtons.size())
            return;

        const MyGUI::IntCoord itemCoord = mEffectButtons[mEffectFocus].second->getAbsoluteCoord();
        const MyGUI::IntCoord viewCoord = mUsedEffectsView->getAbsoluteCoord();

        int newOffset = mUsedEffectsView->getViewOffset().top;
        if (itemCoord.top < viewCoord.top)
            newOffset += viewCoord.top - itemCoord.top;
        else if (itemCoord.top + itemCoord.height > viewCoord.top + viewCoord.height)
            newOffset -= (itemCoord.top + itemCoord.height) - (viewCoord.top + viewCoord.height);
        else
            return;

        const int minOffset = std::min(0, mUsedEffectsView->getHeight() - mUsedEffectsView->getCanvasSize().height);
        newOffset = std::clamp(newOffset, minOffset, 0);
        mUsedEffectsView->setViewOffset(MyGUI::IntPoint(0, newOffset));
    }

    void EffectEditorBase::setControllerEffectsFocus(bool active)
    {
        if (!Settings::gui().mControllerMenus)
            return;

        if (!active)
        {
            if (!mRightColumn && mAvailableFocus < mAvailableButtons.size())
            {
                mAvailableButtons[mAvailableFocus]->setStateSelected(false);
                mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, false);
            }
            if (mRightColumn && mEffectFocus < mEffectButtons.size())
            {
                mEffectButtons[mEffectFocus].first->setStateSelected(false);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(false);
            }
            return;
        }

        if (!mRightColumn && mAvailableFocus < mAvailableButtons.size())
        {
            mAvailableButtons[mAvailableFocus]->setStateSelected(true);
            mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, true);
        }
        else if (mRightColumn && mEffectFocus < mEffectButtons.size())
        {
            mEffectButtons[mEffectFocus].first->setStateSelected(true);
            if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                mEffectHighlights[mEffectFocus]->setVisible(true);
            scrollUsedEffectsToFocused();
        }
    }

    void EffectEditorBase::setControllerEffectsRightColumn(bool rightColumn)
    {
        if (!Settings::gui().mControllerMenus || !mAvailableEffectsList)
        {
            mRightColumn = rightColumn;
            return;
        }

        if (mRightColumn == rightColumn)
            return;

        MWBase::WindowManager* winMgr = MWBase::Environment::get().getWindowManager();

        if (rightColumn)
        {
            if (mAvailableFocus < mAvailableButtons.size())
            {
                mAvailableButtons[mAvailableFocus]->setStateSelected(false);
                mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, false);
            }
            if (mEffectFocus < mEffectButtons.size())
            {
                mEffectButtons[mEffectFocus].first->setStateSelected(true);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(true);
                scrollUsedEffectsToFocused();
            }
            winMgr->setControllerTooltipVisible(false);
        }
        else
        {
            if (mEffectFocus < mEffectButtons.size())
            {
                mEffectButtons[mEffectFocus].first->setStateSelected(false);
                if (mEffectFocus < mEffectHighlights.size() && mEffectHighlights[mEffectFocus])
                    mEffectHighlights[mEffectFocus]->setVisible(false);
            }
            if (mAvailableFocus < mAvailableButtons.size())
            {
                mAvailableButtons[mAvailableFocus]->setStateSelected(true);
                mAvailableEffectsList->setItemHighlightVisible(mAvailableFocus, true);
            }
            winMgr->setControllerTooltipVisible(Settings::gui().mControllerTooltips);
        }

        mRightColumn = rightColumn;
        winMgr->setCursorVisible(!winMgr->getControllerTooltipVisible());
    }

    bool EffectEditorBase::isControllerEffectsAtTop() const
    {
        if (mRightColumn)
            return mEffectButtons.empty() || mEffectFocus == 0;
        return mAvailableButtons.empty() || mAvailableFocus == 0;
    }

    bool EffectEditorBase::isControllerEffectsRightColumn() const
    {
        return mRightColumn;
    }

    MyGUI::Widget* EffectEditorBase::getControllerEffectsTooltipWidget() const
    {
        if (mRightColumn)
        {
            if (mEffectFocus < mEffectButtons.size())
                return mEffectButtons[mEffectFocus].second;
            return nullptr;
        }

        if (mAvailableFocus < mAvailableButtons.size())
            return mAvailableButtons[mAvailableFocus];
        return nullptr;
    }
}
