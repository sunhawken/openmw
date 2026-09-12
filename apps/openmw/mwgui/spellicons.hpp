#ifndef MWGUI_SPELLICONS_H
#define MWGUI_SPELLICONS_H

#include <map>
#include <vector>

#include <components/esm/refid.hpp>

namespace MyGUI
{
    class Widget;
    class ImageBox;
}

namespace MWGui
{

    class SpellIcons
    {
    public:
        void updateWidgets(MyGUI::Widget* parent, bool adjustSize, bool anchorRight = true);
        void getVisibleWidgets(std::vector<MyGUI::ImageBox*>& out) const;

    private:
        std::map<ESM::RefId, MyGUI::ImageBox*> mWidgetMap;
    };

}

#endif
