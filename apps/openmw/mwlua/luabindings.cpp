#include "luabindings.hpp"

#include <components/debug/debuglog.hpp>
#include <components/lua/asyncpackage.hpp>
#include <components/lua/utilpackage.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/datetimemanager.hpp"

#include "animationbindings.hpp"
#include "camerabindings.hpp"
#include "cellbindings.hpp"
#include "corebindings.hpp"
#include "debugbindings.hpp"
#include "inputbindings.hpp"
#include "localscripts.hpp"
#include "markupbindings.hpp"
#include "menuscripts.hpp"
#include "nearbybindings.hpp"
#include "objectbindings.hpp"
#include "postprocessingbindings.hpp"
#include "soundbindings.hpp"
#include "types/types.hpp"
#include "uibindings.hpp"
#include "vfsbindings.hpp"
#include "worldbindings.hpp"

namespace MWLua
{
    std::map<std::string, sol::object> initCommonPackages(const Context& context)
    {
        Log(Debug::Info) << "[LUA DEBUG] initCommonPackages starting...";
        sol::state_view lua = context.mLua->unsafeState();
        Log(Debug::Info) << "[LUA DEBUG] Got lua state view";
        MWWorld::DateTimeManager* tm = MWBase::Environment::get().getWorld()->getTimeManager();
        Log(Debug::Info) << "[LUA DEBUG] Got TimeManager";

        std::map<std::string, sol::object> result;

        try {
            Log(Debug::Info) << "[LUA DEBUG] Creating openmw.async...";
            result["openmw.async"] = LuaUtil::getAsyncPackageInitializer(
                lua, [tm] { return tm->getSimulationTime(); }, [tm] { return tm->getGameTime(); });
            Log(Debug::Info) << "[LUA DEBUG] openmw.async created";
        } catch (const std::exception& e) {
            Log(Debug::Error) << "[LUA DEBUG] openmw.async failed: " << e.what();
            throw;
        }

        try {
            Log(Debug::Info) << "[LUA DEBUG] Creating openmw.markup...";
            result["openmw.markup"] = initMarkupPackage(context);
            Log(Debug::Info) << "[LUA DEBUG] openmw.markup created";
        } catch (const std::exception& e) {
            Log(Debug::Error) << "[LUA DEBUG] openmw.markup failed: " << e.what();
            throw;
        }

        try {
            Log(Debug::Info) << "[LUA DEBUG] Creating openmw.util...";
            result["openmw.util"] = LuaUtil::initUtilPackage(lua);
            Log(Debug::Info) << "[LUA DEBUG] openmw.util created";
        } catch (const std::exception& e) {
            Log(Debug::Error) << "[LUA DEBUG] openmw.util failed: " << e.what();
            throw;
        }

        try {
            Log(Debug::Info) << "[LUA DEBUG] Creating openmw.vfs...";
            result["openmw.vfs"] = initVFSPackage(context);
            Log(Debug::Info) << "[LUA DEBUG] openmw.vfs created";
        } catch (const std::exception& e) {
            Log(Debug::Error) << "[LUA DEBUG] openmw.vfs failed: " << e.what();
            throw;
        }

        Log(Debug::Info) << "[LUA DEBUG] initCommonPackages complete";
        return result;
    }

    std::map<std::string, sol::object> initGlobalPackages(const Context& context)
    {
        initObjectBindingsForGlobalScripts(context);
        initCellBindingsForGlobalScripts(context);
        return {
            { "openmw.core", initCorePackage(context) },
            { "openmw.types", initTypesPackage(context) },
            { "openmw.world", initWorldPackage(context) },
        };
    }

    std::map<std::string, sol::object> initLocalPackages(const Context& context)
    {
        initObjectBindingsForLocalScripts(context);
        initCellBindingsForLocalScripts(context);
        LocalScripts::initializeSelfPackage(context);
        return {
            { "openmw.animation", initAnimationPackage(context) },
            { "openmw.core", initCorePackage(context) },
            { "openmw.types", initTypesPackage(context) },
            { "openmw.nearby", initNearbyPackage(context) },
        };
    }

    std::map<std::string, sol::object> initPlayerPackages(const Context& context)
    {
        return {
            { "openmw.ambient", initAmbientPackage(context) },
            { "openmw.camera", initCameraPackage(context.sol()) },
            { "openmw.debug", initDebugPackage(context) },
            { "openmw.input", initInputPackage(context) },
            { "openmw.postprocessing", initPostprocessingPackage(context) },
            { "openmw.ui", initUserInterfacePackage(context) },
        };
    }

    std::map<std::string, sol::object> initMenuPackages(const Context& context)
    {
        return {
            { "openmw.core", initCorePackage(context) },
            { "openmw.ambient", initAmbientPackage(context) },
            { "openmw.ui", initUserInterfacePackage(context) },
            { "openmw.menu", initMenuPackage(context) },
            { "openmw.input", initInputPackage(context) },
        };
    }
}
