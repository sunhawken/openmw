#include "layout.hpp"

#include <MyGUI_Gui.h>
#include <MyGUI_LayoutManager.h>
#include <MyGUI_TextBox.h>
#include <MyGUI_UString.h>
#include <MyGUI_Widget.h>
#include <MyGUI_Window.h>

namespace MWGui
{
    void Layout::initialise(std::string_view layout)
    {
        constexpr char mainWindow[] = "_Main";
        mLayoutName = layout;

        mPrefix = MyGUI::utility::toString(this, "_");
        mListWindowRoot = MyGUI::LayoutManager::getInstance().loadLayout(mLayoutName, mPrefix);

        const std::string mainName = mPrefix + mainWindow;
        for (MyGUI::Widget* widget : mListWindowRoot)
        {
            // Normal MyGUI behavior prefixes every named widget. Accept the unprefixed
            // root too so an older/different MyGUI LayoutManager cannot make the entire
            // Options window fatal merely because it did not prefix the root name.
            if (widget->getName() == mainName || widget->getName() == mainWindow)
                mMainWidget = widget;

            // Force the alignment to update immediately
            widget->_setAlign(widget->getSize(), widget->getParentSize());
        }

        // Some MyGUI builds can return a single unnamed/top-level window even though
        // its children were loaded correctly. A layout owned by OpenMW is required to
        // have exactly one top-level root, so accepting that sole root is safe and much
        // friendlier than crashing before the Options menu opens.
        if (!mMainWidget && mListWindowRoot.size() == 1)
            mMainWidget = mListWindowRoot.front();

        MYGUI_ASSERT(mMainWidget,
            "root widget name '" << mainWindow << "' in layout '" << mLayoutName
                                 << "' not found (loaded " << mListWindowRoot.size() << " top-level widgets).");
    }

    void Layout::shutdown()
    {
        setVisible(false);
        MyGUI::Gui::getInstance().destroyWidget(mMainWidget);
        mListWindowRoot.clear();
    }

    void Layout::setCoord(int x, int y, int w, int h)
    {
        mMainWidget->setCoord(x, y, w, h);
    }

    void Layout::setVisible(bool b)
    {
        mMainWidget->setVisible(b);
    }

    void Layout::setText(std::string_view name, std::string_view caption)
    {
        MyGUI::Widget* pt;
        getWidget(pt, name);
        static_cast<MyGUI::TextBox*>(pt)->setCaption(MyGUI::UString(caption));
    }

    void Layout::setTitle(std::string_view title)
    {
        MyGUI::Window* window = static_cast<MyGUI::Window*>(mMainWidget);

        if (window->getCaption() != title)
            window->setCaptionWithReplacing(MyGUI::UString(title));
    }

    MyGUI::Widget* Layout::getWidget(std::string_view name) const
    {
        std::string target = mPrefix;
        target += name;
        for (MyGUI::Widget* widget : mListWindowRoot)
        {
            MyGUI::Widget* find = widget->findWidget(target);
            if (nullptr != find)
            {
                return find;
            }
        }
        MYGUI_EXCEPT("widget name '" << name << "' in layout '" << mLayoutName << "' not found.");
    }

}
