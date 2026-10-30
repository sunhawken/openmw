#include "kurenaibindings.hpp"

#include "context.hpp"

#include <components/lua/luastate.hpp>

#include "../mwkurenai/driver.hpp"

namespace MWLua
{
    sol::table initKurenaiPackage(const Context& context)
    {
        sol::state_view view = context.sol();
        sol::table api(view, sol::create);

        // true when Red Ninja is connected and the options below do something
        api["available"] = []() { return MWKurenai::Driver::instance() != nullptr; };

        // { { id, label, value, choices = { ... }, names = { ... } }, ... }
        api["options"] = [view]() {
            sol::table out(view, sol::create);
            if (MWKurenai::Driver* d = MWKurenai::Driver::instance())
            {
                int n = 0;
                for (const MWKurenai::Driver::Option& o : d->options())
                {
                    sol::table t(view, sol::create);
                    t["id"] = o.id;
                    t["label"] = o.label;
                    t["value"] = o.value;
                    t["kind"] = o.kind;
                    sol::table choices(view, sol::create), names(view, sol::create);
                    for (std::size_t i = 0; i < o.choices.size(); ++i)
                    {
                        choices[i + 1] = o.choices[i];
                        names[i + 1] = o.names[i];
                    }
                    t["choices"] = choices;
                    t["names"] = names;
                    out[++n] = t;
                }
            }
            return out;
        };
        api["setOption"] = [](const std::string& id, const std::string& value) {
            MWKurenai::Driver* d = MWKurenai::Driver::instance();
            return d && d->setOption(id, value);
        };
        // the SDL scancode of the menu key (F10 by default)
        api["menuKey"] = []() {
            MWKurenai::Driver* d = MWKurenai::Driver::instance();
            return d ? d->menuKey() : 0;
        };
        // lines of text about the pads and their buttons
        api["padInfo"] = [view]() {
            sol::table out(view, sol::create);
            if (MWKurenai::Driver* d = MWKurenai::Driver::instance())
            {
                int n = 0;
                for (const std::string& l : d->padInfo())
                    out[++n] = l;
            }
            return out;
        };
        // While the menu is up the game gets neutral input and the pad's presses queue for pollNav.
        api["setMenuOpen"] = [](bool open) {
            if (MWKurenai::Driver* d = MWKurenai::Driver::instance())
                d->setMenuOpen(open);
        };
        // "menu" (the pad's open chord), then while open "up" "down" "left" "right" "accept" "back" "close"
        api["pollNav"] = [view]() {
            sol::table out(view, sol::create);
            if (MWKurenai::Driver* d = MWKurenai::Driver::instance())
            {
                int n = 0;
                for (const std::string& s : d->takeNav())
                    out[++n] = s;
            }
            return out;
        };
        return LuaUtil::makeReadOnly(api);
    }
}
