#ifndef MWLUA_KURENAIBINDINGS_H
#define MWLUA_KURENAIBINDINGS_H

#include <sol/sol.hpp>

namespace MWLua
{
    struct Context;

    // openmw.kurenai (player scripts, only in a build with the KURENAI bridge): the Kurenai options menu's data and pad input.
    sol::table initKurenaiPackage(const Context& context);
}

#endif
