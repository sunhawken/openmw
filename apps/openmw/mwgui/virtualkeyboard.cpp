#include "virtualkeyboard.hpp"

#include <MyGUI_InputManager.h>
#include <MyGUI_RenderManager.h>
#include <MyGUI_UString.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

#include <algorithm>
#include <cctype>
#include <memory>

#include <unicode/locid.h>
#include <unicode/unistr.h>

#include <components/esm/refid.hpp>
#include <components/files/istreamptr.hpp>
#include <components/l10n/messagebundles.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/windowmanager.hpp"

namespace MWGui
{
    namespace
    {
        // Disable mouse focus for the keyboard tree.
        void disableMouseFocus(MyGUI::Widget* parent)
        {
            if (!parent)
                return;

            parent->setNeedMouseFocus(false);
            MyGUI::EnumeratorWidgetPtr enumerator = parent->getEnumerator();
            while (enumerator.next())
                enumerator.current()->setNeedMouseFocus(false);
        }

        std::vector<std::string> splitKeyboardRow(std::string_view row, std::size_t expected)
        {
            std::vector<std::string> result;
            MyGUI::UString ustring(row);
            const MyGUI::UString::utf32string utf32 = ustring.asUTF32();
            result.reserve(std::max<std::size_t>(expected, utf32.size()));
            for (MyGUI::UString::utf32string::const_iterator it = utf32.begin(); it != utf32.end(); ++it)
            {
                MyGUI::UString entry;
                entry.push_back(*it);
                result.push_back(entry.asUTF8());
            }
            result.resize(expected);
            return result;
        }

        std::vector<std::string> makeShiftRow(const std::vector<std::string>& baseRow)
        {
            std::vector<std::string> result;
            result.reserve(baseRow.size());
            for (const std::string& entry : baseRow)
            {
                if (entry.empty())
                {
                    result.push_back(entry);
                    continue;
                }

                icu::UnicodeString upper = icu::UnicodeString::fromUTF8(entry);
                upper.toUpper();
                std::string upperUtf8;
                upper.toUTF8String(upperUtf8);
                result.push_back(std::move(upperUtf8));
            }
            return result;
        }

        std::string getMessageOrEmpty(const L10n::MessageBundles& bundles, std::string_view key)
        {
            std::string value = bundles.formatMessage(key, {}, {});
            if (value == key)
                return {};
            return value;
        }

        struct KeyboardFileInfo
        {
            std::string mBaseName;
            std::string mDefaultLang;
        };

        std::string normalizeLocaleName(std::string_view locale)
        {
            std::string normalized(locale);
            std::replace(normalized.begin(), normalized.end(), '-', '_');
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return normalized;
        }

        std::string makeKeyboardFileName(std::string_view baseName)
        {
            std::string fileName(baseName);
            constexpr std::string_view extension = ".yaml";
            if (!fileName.empty() && !Misc::StringUtils::ciEndsWith(fileName, extension))
                fileName += extension;
            return fileName;
        }

        std::string getKeyboardValue(const VFS::Manager* vfs, std::string_view baseName, std::string_view key)
        {
            if (!vfs || baseName.empty())
                return {};

            VFS::Path::Normalized path("mygui");
            path /= "Keyboard";
            path /= makeKeyboardFileName(baseName);
            const Files::IStreamPtr stream = vfs->find(path);
            if (!stream)
                return {};

            std::vector<icu::Locale> preferredLocales;
            preferredLocales.emplace_back("keyboard");
            icu::Locale fallbackLocale("en");
            L10n::MessageBundles bundles(preferredLocales, fallbackLocale);
            bundles.load(*stream, preferredLocales.front());
            return getMessageOrEmpty(bundles, key);
        }

        std::vector<KeyboardFileInfo> listKeyboardFiles(const VFS::Manager* vfs)
        {
            std::vector<KeyboardFileInfo> files;
            if (!vfs)
                return files;

            const std::string_view keyboardPath = "mygui/Keyboard";
            for (const auto& path : vfs->getRecursiveDirectoryIterator(keyboardPath))
            {
                if (path.extension() != "yaml")
                    continue;
                KeyboardFileInfo info;
                info.mBaseName = std::string(path.stem());
                info.mDefaultLang = getKeyboardValue(vfs, info.mBaseName, "defaultLang");
                files.push_back(std::move(info));
            }

            std::sort(files.begin(), files.end(), [](const KeyboardFileInfo& left, const KeyboardFileInfo& right) {
                return Misc::StringUtils::ciLess(left.mBaseName, right.mBaseName);
            });
            return files;
        }

        std::string selectKeyboardFile(
            const VFS::Manager* vfs, std::string_view requestedFile, std::string_view fallbackFile)
        {
            auto hasKeyboardFile = [vfs](std::string_view baseName) {
                if (!vfs || baseName.empty())
                    return false;
                VFS::Path::Normalized path("mygui");
                path /= "Keyboard";
                path /= makeKeyboardFileName(baseName);
                return vfs->exists(path);
            };

            if (!requestedFile.empty())
            {
                if (hasKeyboardFile(requestedFile))
                    return std::string(requestedFile);
                const std::string normalized = normalizeLocaleName(requestedFile);
                if (hasKeyboardFile(normalized))
                    return normalized;
            }

            const std::vector<KeyboardFileInfo> files = listKeyboardFiles(vfs);
            std::vector<std::string> preferredLocales = Settings::general().mPreferredLocales;
            if (preferredLocales.empty())
                preferredLocales.emplace_back("en");

            for (const std::string& locale : preferredLocales)
            {
                const std::string normalized = normalizeLocaleName(locale);
                const std::string::size_type underscore = normalized.find('_');
                const std::string base
                    = underscore == std::string::npos ? normalized : normalized.substr(0, underscore);
                for (const KeyboardFileInfo& file : files)
                {
                    const std::string fileLang = normalizeLocaleName(file.mDefaultLang);
                    if (!fileLang.empty() && (fileLang == normalized || fileLang == base))
                        return file.mBaseName;
                }
            }

            if (hasKeyboardFile(fallbackFile))
                return std::string(fallbackFile);

            if (!files.empty())
                return files.front().mBaseName;

            return std::string();
        }

        std::unique_ptr<L10n::MessageBundles> loadKeyboardBundles(std::string_view fileBaseName)
        {
            const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
            if (!vfs || fileBaseName.empty())
                return {};

            VFS::Path::Normalized path("mygui");
            path /= "Keyboard";
            path /= makeKeyboardFileName(fileBaseName);
            const Files::IStreamPtr stream = vfs->find(path);
            if (!stream)
                return {};

            std::vector<icu::Locale> preferredLocales;
            preferredLocales.emplace_back("keyboard");
            icu::Locale fallbackLocale("en");
            auto bundles = std::make_unique<L10n::MessageBundles>(preferredLocales, fallbackLocale);
            bundles->load(*stream, preferredLocales.front());
            return bundles;
        }

    }

    VirtualKeyboard::VirtualKeyboard()
        : WindowModal("openmw_virtual_keyboard.layout")
    {
        mDisableGamepadCursor = true;
        disableMouseFocus(mMainWidget);

        getWidget(mInputBar, "InputBar");
        mInputBar->setEditReadOnly(true);
        mInputBar->setNeedKeyFocus(false);

        if (Settings::gui().mControllerMenus)
            updateControllerButtons();

        mNavRows.clear();
        mNavRows.resize(5);
        mTextKeys.clear();

        auto registerLayoutKey = [this](std::string_view name) {
            size_t index = registerTextKey(name, "", "");
            mTextKeys.push_back(index);
            return index;
        };

        mNavRows[0] = {
            registerLayoutKey("Key00"),
            registerLayoutKey("Key01"),
            registerLayoutKey("Key02"),
            registerLayoutKey("Key03"),
            registerLayoutKey("Key04"),
            registerLayoutKey("Key05"),
            registerLayoutKey("Key06"),
            registerLayoutKey("Key07"),
            registerLayoutKey("Key08"),
            registerLayoutKey("Key09"),
            registerLayoutKey("Key10"),
        };

        mNavRows[1] = {
            registerLayoutKey("Key11"),
            registerLayoutKey("Key12"),
            registerLayoutKey("Key13"),
            registerLayoutKey("Key14"),
            registerLayoutKey("Key15"),
            registerLayoutKey("Key16"),
            registerLayoutKey("Key17"),
            registerLayoutKey("Key18"),
            registerLayoutKey("Key19"),
            registerLayoutKey("Key20"),
            registerLayoutKey("Key21"),
        };

        mNavRows[2] = {
            registerLayoutKey("Key22"),
            registerLayoutKey("Key23"),
            registerLayoutKey("Key24"),
            registerLayoutKey("Key25"),
            registerLayoutKey("Key26"),
            registerLayoutKey("Key27"),
            registerLayoutKey("Key28"),
            registerLayoutKey("Key29"),
            registerLayoutKey("Key30"),
            registerLayoutKey("Key31"),
            registerLayoutKey("Key32"),
        };

        mKeyShift = registerActionKey("KeyShift", Action::Shift, "");
        mKeyBackspace = registerActionKey("KeyBackspace", Action::Backspace, "");
        mKeyMore = registerActionKey("KeyMore", Action::More, "");
        mKeySpace = registerActionKey("KeySpace", Action::Space, "");
        mKeyDone = registerActionKey("KeyDone", Action::Done, "");

        mNavRows[3] = { mKeySpace, mKeyMore };
        mNavRows[4] = { mKeyShift, mKeyBackspace, mKeyDone };

        mRowMemory.assign(mNavRows.size(), 0);
        mRowMemoryValid.assign(mNavRows.size(), false);

        if (useControllerSelectionHighlight())
        {
            mControllerHighlight = mMainWidget->createWidget<MyGUI::Widget>(
                "ControllerHighlight", MyGUI::IntCoord(0, 0, 0, 0), MyGUI::Align::Default);
            mControllerHighlight->setNeedMouseFocus(false);
            mControllerHighlight->setDepth(1);
            mControllerHighlight->setVisible(false);
        }

        reloadLayout();
        ensureRowsVisible();
        selectNav(0, 0);
        syncInputBar();
    }

    void VirtualKeyboard::setVisible(bool visible)
    {
        WindowBase::setVisible(visible);
        if (visible)
        {
            mShifted = false;
            mCursorBlinkTimer = 0.f;
            mCursorBlinkVisible = true;
            mUseSecondaryLayout = false;
            mRightTriggerPressed = false;
            std::fill(mRowMemoryValid.begin(), mRowMemoryValid.end(), false);
            mSpaceReturnValid = false;
            mNextReturnValid = false;
            reloadLayout();
            ensureRowsVisible();
            selectNav(0, 0);
            centerInMenuArea();
            syncInputBar();
        }
    }

    void VirtualKeyboard::onFrame(float dt)
    {
        // Blink the caret in the input bar.
        mCursorBlinkTimer += dt;
        if (mCursorBlinkTimer >= 0.5f)
        {
            mCursorBlinkTimer = 0.f;
            mCursorBlinkVisible = !mCursorBlinkVisible;
        }
        syncInputBar();
    }

    int VirtualKeyboard::getHeight() const
    {
        MyGUI::Window* window = mMainWidget->castType<MyGUI::Window>();
        return window->getHeight();
    }

    void VirtualKeyboard::setTargetEdit(MyGUI::EditBox* edit)
    {
        mTargetEdit = edit;
        syncInputBar();
    }

    void VirtualKeyboard::clearTargetEdit()
    {
        mTargetEdit = nullptr;
        syncInputBar();
    }

    size_t VirtualKeyboard::registerTextKey(
        std::string_view widgetName, std::string_view text, std::string_view shifted)
    {
        MyGUI::Button* button = nullptr;
        getWidget(button, widgetName);
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &VirtualKeyboard::onKeyClicked);

        KeySpec key{ button, std::string(text), std::string(shifted), Action::Text };
        mKeyLookup[button] = mKeys.size();
        size_t index = mKeys.size();
        mKeys.push_back(std::move(key));
        return index;
    }

    size_t VirtualKeyboard::registerActionKey(std::string_view widgetName, Action action, std::string_view caption)
    {
        MyGUI::Button* button = nullptr;
        getWidget(button, widgetName);
        button->eventMouseButtonClick += MyGUI::newDelegate(this, &VirtualKeyboard::onKeyClicked);
        button->setCaptionWithReplacing(MyGUI::UString(caption));

        KeySpec key{ button, std::string(caption), std::string(caption), action };
        mKeyLookup[button] = mKeys.size();
        size_t index = mKeys.size();
        mKeys.push_back(std::move(key));
        return index;
    }

    void VirtualKeyboard::onKeyClicked(MyGUI::Widget* sender)
    {
        auto it = mKeyLookup.find(sender);
        if (it == mKeyLookup.end())
            return;

        MWBase::Environment::get().getWindowManager()->playSound(ESM::RefId::stringRefId("Menu Click"));

        KeySpec& key = mKeys[it->second];
        for (size_t row = 0; row < mNavRows.size(); ++row)
        {
            for (size_t col = 0; col < mNavRows[row].size(); ++col)
            {
                if (mNavRows[row][col] == it->second)
                {
                    selectNav(row, col);
                    break;
                }
            }
        }

        switch (key.mAction)
        {
            case Action::Text:
                injectText(mShifted ? key.mShiftText : key.mText);
                mShifted = false;
                updateKeyCaptions();
                break;
            case Action::Backspace:
                injectBackspace();
                mShifted = false;
                updateKeyCaptions();
                break;
            case Action::Shift:
                mShifted = !mShifted;
                updateKeyCaptions();
                break;
            case Action::Done:
                MWBase::Environment::get().getWindowManager()->toggleVirtualKeyboard();
                mShifted = false;
                updateKeyCaptions();
                break;
            case Action::More:
                toggleMorePage();
                break;
            case Action::Space:
                injectText(" ");
                mShifted = false;
                updateKeyCaptions();
                break;
        }
    }

    void VirtualKeyboard::updateKeyCaptions()
    {
        for (size_t index : mTextKeys)
        {
            KeySpec& key = mKeys[index];
            key.mButton->setCaption(mShifted ? key.mShiftText : key.mText);
        }
    }

    void VirtualKeyboard::updateActionCaptions()
    {
        mKeys[mKeyShift].mButton->setCaption(mCaptionShift);
        mKeys[mKeyBackspace].mButton->setCaption(mCaptionBackspace);
        mKeys[mKeyMore].mButton->setCaption(mPage == 0 ? mCaptionNext : mCaptionPrev);
        mKeys[mKeySpace].mButton->setCaption(mCaptionSpace);
        mKeys[mKeyDone].mButton->setCaption(mCaptionDone);
    }

    void VirtualKeyboard::toggleMorePage()
    {
        mPage = mPage == 0 ? 1 : 0;
        mShifted = false;
        applyLayout();
        updateActionCaptions();
        selectNav(mNavRow, mNavCol);
        updateControllerButtons();
    }

    void VirtualKeyboard::updateControllerButtons()
    {
        if (!Settings::gui().mControllerMenus)
            return;

        mControllerButtons.mDpad.clear();
        mControllerButtons.mA = "#{Interface:KeyboardSelect}";
        mControllerButtons.mB = "#{Interface:Back}";
        mControllerButtons.mX = "#{Interface:KeyboardDelete}";
        mControllerButtons.mY = "#{Interface:KeyboardSpace}";

        if (mPage == 0)
            mControllerButtons.mR2 = "#{Interface:KeyboardNext}";
        else
            mControllerButtons.mR2 = "#{Interface:KeyboardPrev}";

        if (hasSecondaryLayout())
            mControllerButtons.mR1 = "#{Interface:KeyboardLanguage}";
        else
            mControllerButtons.mR1.clear();

        mControllerButtons.mL1.clear();
        mControllerButtons.mL2.clear();

        MWBase::Environment::get().getWindowManager()->updateControllerButtonsOverlay();
    }

    bool VirtualKeyboard::hasSecondaryLayout() const
    {
        return !Settings::gui().mVirtualKeyboardLanguageSecondary.get().empty();
    }

    void VirtualKeyboard::toggleKeyboardLayout()
    {
        if (!hasSecondaryLayout())
            return;
        mUseSecondaryLayout = !mUseSecondaryLayout;
        reloadLayout();
        ensureRowsVisible();
        selectNav(0, 0);
        syncInputBar();
    }

    void VirtualKeyboard::reloadLayout()
    {
        mLayouts.clear();
        if (!hasSecondaryLayout())
            mUseSecondaryLayout = false;

        const std::string requestedFile = mUseSecondaryLayout ? Settings::gui().mVirtualKeyboardLanguageSecondary
                                                              : Settings::gui().mVirtualKeyboardLanguage;
        const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
        const std::string fileBaseName = selectKeyboardFile(vfs, requestedFile, "en");
        std::unique_ptr<L10n::MessageBundles> customBundles = loadKeyboardBundles(fileBaseName);
        if (!customBundles)
        {
            std::vector<icu::Locale> preferredLocales;
            icu::Locale fallbackLocale("en");
            customBundles = std::make_unique<L10n::MessageBundles>(preferredLocales, fallbackLocale);
        }
        const L10n::MessageBundles& bundles = *customBundles;

        mCaptionShift = getMessageOrEmpty(bundles, "Shift");
        mCaptionBackspace = getMessageOrEmpty(bundles, "Backspace");
        mCaptionDone = getMessageOrEmpty(bundles, "Done");
        mCaptionSpace = getMessageOrEmpty(bundles, "Space");
        mCaptionNext = getMessageOrEmpty(bundles, "Next");
        if (mCaptionNext.empty())
            mCaptionNext = getMessageOrEmpty(bundles, "More");
        mCaptionPrev = getMessageOrEmpty(bundles, "Prev");

        if (mCaptionShift.empty())
            mCaptionShift = "Shift";
        if (mCaptionBackspace.empty())
            mCaptionBackspace = "Backspace";
        if (mCaptionDone.empty())
            mCaptionDone = "Done";
        if (mCaptionSpace.empty())
            mCaptionSpace = "Space";
        if (mCaptionNext.empty())
            mCaptionNext = "Next";
        if (mCaptionPrev.empty())
            mCaptionPrev = "Prev";

        std::string layoutsValue = getMessageOrEmpty(bundles, "Layouts");
        std::vector<std::string> layoutNames;
        if (!layoutsValue.empty())
        {
            Misc::StringUtils::split(layoutsValue, layoutNames, ",");
            for (std::string& name : layoutNames)
                Misc::StringUtils::trim(name);
        }
        if (layoutNames.empty())
            layoutNames.push_back("qwerty");

        for (const std::string& layout : layoutNames)
        {
            if (layout.empty())
                continue;
            LayoutSpec spec;
            spec.mName = layout;
            for (std::size_t row = 0; row < 3; ++row)
            {
                const std::string rowIndex = std::to_string(row + 1);
                spec.mPage1Rows[row] = getMessageOrEmpty(bundles, layout + "Row" + rowIndex);
                spec.mPage2Rows[row] = getMessageOrEmpty(bundles, layout + "Page2Row" + rowIndex);

                const std::string shiftKey = layout + "Row" + rowIndex + "Shift";
                const std::string page2ShiftKey = layout + "Page2Row" + rowIndex + "Shift";
                spec.mPage1ShiftRows[row] = getMessageOrEmpty(bundles, shiftKey);
                spec.mPage2ShiftRows[row] = getMessageOrEmpty(bundles, page2ShiftKey);
            }
            mLayouts.push_back(std::move(spec));
        }
        if (mLayouts.empty())
            mLayouts.push_back(LayoutSpec{ "qwerty", {}, {}, {}, {} });

        const std::string wantedLayout = mUseSecondaryLayout ? Settings::gui().mVirtualKeyboardLayoutSecondary
                                                             : Settings::gui().mVirtualKeyboardLayout;
        mLayoutIndex = 0;
        if (!wantedLayout.empty())
        {
            for (std::size_t i = 0; i < mLayouts.size(); ++i)
            {
                if (Misc::StringUtils::ciEqual(mLayouts[i].mName, wantedLayout))
                {
                    mLayoutIndex = i;
                    break;
                }
            }
        }
        else if (!mLayouts.empty())
        {
            mLayoutIndex = 0;
        }

        mPage = 0;
        mShifted = false;
        updateActionCaptions();
        applyLayout();
        updateControllerButtons();
    }

    void VirtualKeyboard::applyLayout()
    {
        if (mLayouts.empty() || mTextKeys.size() != 33)
            return;

        const LayoutSpec& layout = mLayouts[mLayoutIndex];

        // Choose which rows to display
        std::array<std::string, 3> baseRows = layout.mPage1Rows;
        std::array<std::string, 3> shiftRows = layout.mPage1ShiftRows;

        const bool page2 = (mPage == 1);
        if (page2)
        {
            baseRows = layout.mPage2Rows;
            shiftRows = layout.mPage2ShiftRows;
        }

        // Page 1: 3 rows (33 keys). Page 2: 2 rows (22 keys).
        const std::size_t rowsToRender = page2 ? 2 : 3;
        const std::size_t keysToRender = rowsToRender * 11;

        // Fill the requested keys
        std::size_t textIndex = 0;
        for (std::size_t row = 0; row < rowsToRender; ++row)
        {
            const std::vector<std::string> baseKeys = splitKeyboardRow(baseRows[row], 11);
            std::vector<std::string> shiftKeys = splitKeyboardRow(shiftRows[row], 11);
            if (shiftRows[row].empty())
                shiftKeys = makeShiftRow(baseKeys);

            for (std::size_t col = 0; col < 11 && textIndex < keysToRender; ++col)
            {
                const size_t keyIndex = mTextKeys[textIndex++];
                KeySpec& key = mKeys[keyIndex];
                key.mText = baseKeys[col];
                key.mShiftText = shiftKeys[col];
                key.mButton->setEnabled(!key.mText.empty());

                // Optional: hide unused keys when page2 has fewer rows
                key.mButton->setVisible(true);
            }
        }

        // Disable (and optionally hide) the remaining keys so page2 only shows 2 rows.
        for (; textIndex < mTextKeys.size(); ++textIndex)
        {
            const size_t keyIndex = mTextKeys[textIndex];
            KeySpec& key = mKeys[keyIndex];
            key.mText.clear();
            key.mShiftText.clear();
            key.mButton->setEnabled(false);

            // If you want them gone visually (recommended):
            key.mButton->setVisible(false);
        }

        updateKeyCaptions();

        if (!isNavRowUsable(mNavRow))
        {
            const size_t targetRow = findNextNavRow(mNavRow, 1);
            selectNav(targetRow, mapColumn(mNavRow, targetRow));
        }
    }

    void VirtualKeyboard::ensureRowsVisible()
    {
        MyGUI::Widget* keyRows = nullptr;
        MyGUI::Widget* row0 = nullptr;
        getWidget(keyRows, "KeyRows");
        getWidget(row0, "Row0");

        const int rowHeight = row0->getHeight();
        const int rowCount = static_cast<int>(mNavRows.size());
        const int spacing = 4;
        const int requiredKeyRowsHeight = rowCount * rowHeight + (rowCount - 1) * spacing;
        if (requiredKeyRowsHeight > keyRows->getHeight())
            keyRows->setSize(keyRows->getWidth(), requiredKeyRowsHeight);

        const MyGUI::IntCoord keyRowsCoord = keyRows->getCoord();
        const int requiredMainHeight = keyRowsCoord.top + keyRows->getHeight();
        if (requiredMainHeight > mMainWidget->getHeight())
            mMainWidget->setSize(mMainWidget->getWidth(), requiredMainHeight);
    }

    void VirtualKeyboard::centerInMenuArea()
    {
        const MyGUI::IntSize viewSize = MyGUI::RenderManager::getInstance().getViewSize();
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        const int availableHeight = windowManager->getControllerMenuHeight();

        MyGUI::IntCoord coord = mMainWidget->getCoord();
        coord.left = (viewSize.width - coord.width) / 2;
        coord.top = (availableHeight - coord.height) / 2;
        if (coord.top < 0)
            coord.top = 0;
        mMainWidget->setCoord(coord);
    }

    void VirtualKeyboard::syncInputBar()
    {
        if (!mTargetEdit)
        {
            mInputBar->setCaption({});
            return;
        }

        std::string caption = mTargetEdit->getCaption().asUTF8();
        if (mCursorBlinkVisible)
            caption += "|";
        mInputBar->setCaption(MyGUI::UString(caption));
    }

    void VirtualKeyboard::injectText(std::string_view text)
    {
        if (!mTargetEdit)
            return;
        MyGUI::InputManager::getInstance().setKeyFocusWidget(mTargetEdit);
        MyGUI::UString ustring(text);
        MyGUI::UString::utf32string utf32string = ustring.asUTF32();
        for (MyGUI::UString::utf32string::const_iterator it = utf32string.begin(); it != utf32string.end(); ++it)
            MyGUI::InputManager::getInstance().injectKeyPress(MyGUI::KeyCode::None, *it);
        syncInputBar();
    }

    void VirtualKeyboard::injectBackspace()
    {
        if (!mTargetEdit)
            return;
        MyGUI::InputManager::getInstance().setKeyFocusWidget(mTargetEdit);
        MyGUI::InputManager::getInstance().injectKeyPress(MyGUI::KeyCode::Backspace);
        MyGUI::InputManager::getInstance().injectKeyRelease(MyGUI::KeyCode::Backspace);
        syncInputBar();
    }

    void VirtualKeyboard::selectNav(size_t row, size_t col)
    {
        if (!isNavRowUsable(row))
            return;

        if (row == 2)
            mSpaceReturnValid = false;

        col = std::min(col, mNavRows[row].size() - 1);
        if (mNavRow < mNavRows.size())
        {
            size_t prevIndex = mNavRows[mNavRow][mNavCol];
            mKeys[prevIndex].mButton->setStateSelected(false);
        }

        mNavRow = row;
        mNavCol = col;
        size_t index = mNavRows[mNavRow][mNavCol];
        if (!mKeys[index].mButton->getVisible() || !mKeys[index].mButton->getEnabled())
        {
            bool found = false;
            for (size_t i = 0; i < mNavRows[mNavRow].size(); ++i)
            {
                size_t candidate = mNavRows[mNavRow][i];
                if (mKeys[candidate].mButton->getVisible() && mKeys[candidate].mButton->getEnabled())
                {
                    mNavCol = i;
                    index = candidate;
                    found = true;
                    break;
                }
            }
            if (!found)
                return;
        }
        mKeys[index].mButton->setStateSelected(true);
        updateControllerHighlight();
    }

    void VirtualKeyboard::updateControllerHighlight()
    {
        if (!mControllerHighlight)
            return;

        if (!Settings::gui().mControllerMenus || !Settings::gui().mControllerHighlightSelections)
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        if (mNavRow >= mNavRows.size() || mNavRows[mNavRow].empty())
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        const size_t index = mNavRows[mNavRow][mNavCol];
        MyGUI::Button* button = mKeys[index].mButton;
        if (!button || !button->getVisible() || !button->getEnabled())
        {
            mControllerHighlight->setVisible(false);
            return;
        }

        const MyGUI::IntCoord buttonCoord = button->getAbsoluteCoord();
        MyGUI::Widget* highlightParent = mControllerHighlight->getParent();
        const MyGUI::IntCoord baseCoord
            = highlightParent ? highlightParent->getAbsoluteCoord() : mMainWidget->getAbsoluteCoord();
        mControllerHighlight->setCoord(
            buttonCoord.left - baseCoord.left, buttonCoord.top - baseCoord.top, buttonCoord.width, buttonCoord.height);
        mControllerHighlight->setVisible(true);
    }

    size_t VirtualKeyboard::mapColumn(size_t fromRow, size_t toRow) const
    {
        if (fromRow >= mNavRows.size() || toRow >= mNavRows.size())
            return 0;

        const size_t fromSize = mNavRows[fromRow].size();
        const size_t toSize = mNavRows[toRow].size();
        if (toSize == 0)
            return 0;
        if (fromSize <= 1)
            return 0;

        // Keep navigation centered when row sizes differ.
        const size_t numerator = mNavCol * (toSize - 1);
        const size_t denominator = fromSize - 1;
        size_t mapped = (numerator + (denominator / 2)) / denominator;
        return std::min(mapped, toSize - 1);
    }

    bool VirtualKeyboard::isNavRowUsable(size_t row) const
    {
        if (row >= mNavRows.size() || mNavRows[row].empty())
            return false;

        for (size_t index : mNavRows[row])
        {
            if (mKeys[index].mButton->getVisible() && mKeys[index].mButton->getEnabled())
                return true;
        }

        return false;
    }

    size_t VirtualKeyboard::findNextNavRow(size_t startRow, int delta) const
    {
        if (mNavRows.empty())
            return startRow;

        const size_t rowCount = mNavRows.size();
        size_t row = startRow;
        for (size_t i = 0; i < rowCount; ++i)
        {
            const int next = static_cast<int>(row) + delta;
            row = static_cast<size_t>(
                (next % static_cast<int>(rowCount) + static_cast<int>(rowCount)) % static_cast<int>(rowCount));
            if (isNavRowUsable(row))
                return row;
        }

        return startRow;
    }

    bool VirtualKeyboard::onControllerThumbstickEvent(const SDL_ControllerAxisEvent& arg)
    {
        if (arg.axis != SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            return false;

        const bool pressed = arg.value >= 32000;
        if (pressed && !mRightTriggerPressed)
        {
            mRightTriggerPressed = true;
            toggleMorePage();
        }
        else if (!pressed && arg.value <= 16000)
        {
            mRightTriggerPressed = false;
        }

        return true;
    }

    bool VirtualKeyboard::onControllerButtonEvent(const SDL_ControllerButtonEvent& arg)
    {
        const size_t memoryRow = [this]() {
            for (size_t row = 2; row > 0; --row)
            {
                if (isNavRowUsable(row))
                    return row;
            }
            return static_cast<size_t>(0);
        }();

        auto rememberMemoryRow = [this, memoryRow]() {
            if (mNavRow == memoryRow && memoryRow < mRowMemory.size())
            {
                mRowMemory[memoryRow] = mNavCol;
                mRowMemoryValid[memoryRow] = true;
            }
        };
        auto selectRow = [this, memoryRow](size_t targetRow, bool useMemory) {
            size_t targetCol = mapColumn(mNavRow, targetRow);
            if (useMemory && targetRow == memoryRow && memoryRow < mRowMemory.size() && mRowMemoryValid[memoryRow])
                targetCol = mRowMemory[memoryRow];
            if (useMemory && targetRow == memoryRow && targetRow < mNavRows.size() && !mNavRows[targetRow].empty())
            {
                targetCol = std::min(targetCol, mNavRows[targetRow].size() - 1);
                if (targetCol >= 8 && mNavRows[targetRow].size() > 7)
                    targetCol = 7;
            }
            selectNav(targetRow, targetCol);
        };

        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_LEFT)
        {
            if (mNavCol > 0)
                selectNav(mNavRow, mNavCol - 1);
            else
                selectNav(mNavRow, mNavRows[mNavRow].size() - 1);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
        {
            selectNav(mNavRow, (mNavCol + 1) % mNavRows[mNavRow].size());
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_UP)
        {
            size_t targetRow = findNextNavRow(mNavRow, -1);
            bool useMemory = (mNavRow == 3 && targetRow == memoryRow);
            if (mNavRow < mNavRows.size() && !mNavRows[mNavRow].empty())
            {
                const size_t currentIndex = mNavRows[mNavRow][mNavCol];
                if (mNavRow == 4 && targetRow == 3)
                {
                    if (currentIndex == mKeyShift)
                    {
                        mSpaceReturnCol = 0;
                        mSpaceReturnValid = true;
                    }
                    else if (currentIndex == mKeyBackspace)
                    {
                        mSpaceReturnCol = 1;
                        mSpaceReturnValid = true;
                    }
                }
                if (mNavRow == 3 && targetRow == memoryRow && currentIndex == mKeyMore)
                {
                    size_t targetCol = std::min<size_t>(8, mNavRows[targetRow].size() - 1);
                    if (mNextReturnValid)
                        targetCol = std::min(mNextReturnCol, mNavRows[targetRow].size() - 1);
                    selectNav(targetRow, targetCol);
                    return true;
                }
                if (currentIndex == mKeyBackspace)
                {
                    selectNav(targetRow, 0);
                    return true;
                }
            }
            selectRow(targetRow, useMemory);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
        {
            if (mNavRow == memoryRow)
                rememberMemoryRow();
            if (mNavRow < mNavRows.size() && !mNavRows[mNavRow].empty())
            {
                const size_t currentIndex = mNavRows[mNavRow][mNavCol];
                if (currentIndex == mKeyBackspace)
                {
                    for (size_t row = 0; row < mNavRows.size(); ++row)
                    {
                        for (size_t col = 0; col < mNavRows[row].size(); ++col)
                        {
                            if (mNavRows[row][col] == mKeySpace)
                            {
                                selectNav(row, col);
                                return true;
                            }
                        }
                    }
                }
            }
            size_t targetRow = findNextNavRow(mNavRow, 1);
            if (mNavRow == memoryRow && targetRow == 3)
            {
                // Prefer Space for columns 6-8 when moving from the last text row to the Space/More row.
                if (mNavCol >= 5 && mNavCol <= 7)
                {
                    mNextReturnValid = false;
                    selectNav(targetRow, 0);
                }
                else
                {
                    mNextReturnValid = mNavCol >= 8;
                    if (mNextReturnValid)
                        mNextReturnCol = mNavCol;
                    selectRow(targetRow, false);
                }
            }
            else if (mNavRow == 3 && targetRow == 4)
            {
                if (mNavRow < mNavRows.size() && !mNavRows[mNavRow].empty())
                {
                    const size_t currentIndex = mNavRows[mNavRow][mNavCol];
                    if (currentIndex == mKeySpace && mSpaceReturnValid)
                    {
                        const size_t returnCol = std::min(mSpaceReturnCol, mNavRows[targetRow].size() - 1);
                        selectNav(targetRow, returnCol);
                        return true;
                    }
                }
                // When moving from Space/More to the action row, choose Shift for row-3 cols 1-3, otherwise Backspace.
                if (mNavCol == 0 && memoryRow < mRowMemoryValid.size() && mRowMemoryValid[memoryRow])
                {
                    if (mRowMemory[memoryRow] <= 2)
                        selectNav(targetRow, 0);
                    else
                        selectNav(targetRow, 1);
                }
                else
                {
                    selectRow(targetRow, false);
                }
            }
            else
            {
                selectRow(targetRow, false);
            }
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)
        {
            if (hasSecondaryLayout())
                toggleKeyboardLayout();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER)
        {
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_A)
        {
            size_t index = mNavRows[mNavRow][mNavCol];
            onKeyClicked(mKeys[index].mButton);
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_B)
        {
            MWBase::Environment::get().getWindowManager()->toggleVirtualKeyboard();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_X)
        {
            injectBackspace();
            return true;
        }
        if (arg.button == SDL_CONTROLLER_BUTTON_Y)
        {
            injectText(" ");
            mShifted = false;
            updateKeyCaptions();
            return true;
        }
        return false;
    }
}
