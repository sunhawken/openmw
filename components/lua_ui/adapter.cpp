#include "adapter.hpp"

#include <algorithm>
#include <MyGUI_Gui.h>

#include "container.hpp"
#include "element.hpp"

namespace LuaUi
{
    namespace
    {
        sol::state luaState;
    }

    LuaAdapter::LuaAdapter()
        : mElement(nullptr)
        , mContainer(nullptr)
    {
        mContainer = MyGUI::Gui::getInstancePtr()->createWidget<LuaContainer>(
            "", MyGUI::IntCoord(), MyGUI::Align::Default, "", "");
        mContainer->initialize(luaState, mContainer, false);
        mContainer->widget()->eventChangeCoord += MyGUI::newDelegate(this, &LuaAdapter::containerChangedCoord);
        mContainer->widget()->attachToWidget(this);
    }

    void LuaAdapter::containerChangedCoord(MyGUI::Widget*)
    {
        setSize(mContainer->getSize());
    }

    void LuaAdapter::attach(const std::shared_ptr<Element>& element)
    {
        detachElement();
        mElement = element;
        attachElement();
        setSize(mContainer->widget()->getSize());

        // workaround for MyGUI bug
        // parent visibility doesn't affect added children
        setVisible(!getVisible());
        setVisible(!getVisible());
    }

    void LuaAdapter::detach()
    {
        detachElement();
        setSize({ 0, 0 });
    }

    void LuaAdapter::setViewportWidth(int width)
    {
        if (!mElement || !mElement->mRoot || !mElement->mRoot->externalValue("fitToWidth", false))
            return;
        auto* root = mElement->mRoot;
        const int padding = std::max(0, root->widget()->getLeft());
        width = std::max(1, width - 2 * padding);
        if (root->widget()->getWidth() == width)
            return;
        // Keep layout properties for subsequent edits. Zero minimum height lets
        // wrapped text reflow when the window becomes wider as well as narrower.
        auto layout = root->getLayout();
        sol::state_view lua(layout.lua_state());
        sol::table props = layout.get_or("props", lua.create_table());
        props["size"] = osg::Vec2f(width, 0);
        layout["props"] = props;
        root->setProperties(props);
        root->updateCoord();
        mContainer->updateCoord();
    }

    void LuaAdapter::attachElement()
    {
        if (!mElement.get())
            return;
        if (!mElement->mRoot)
            throw std::logic_error("Attempting to use a destroyed UI Element");
        mContainer->setChildren({ mElement->mRoot });
        mElement->mRoot->updateCoord();
        mContainer->updateCoord();
    }

    void LuaAdapter::detachElement()
    {
        mContainer->setChildren({});
        if (mElement && mElement->mRoot)
            mElement->mRoot->widget()->detachFromWidget();
        mElement = nullptr;
    }
}
