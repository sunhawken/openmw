#include "jigglenpclist.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <components/misc/jigglepolicy.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/settings/values.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwrender/renderingmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

namespace MWGui::JiggleNpcList
{
    namespace
    {
        bool inGame()
        {
            return MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running;
        }

        std::vector<MWWorld::Ptr> loadedActors()
        {
            std::vector<MWWorld::Ptr> actors;
            if (!inGame())
                return actors;
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty())
                return actors;
            // Every actor the mechanics manager tracks (the active cells); the radius just covers them all.
            MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                player.getRefData().getPosition().asVec3(), 1e9f, actors);
            return actors;
        }

        void rebuildNpcsNamed(std::string_view name)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            for (const MWWorld::Ptr& actor : loadedActors())
                if (!npcName(actor).empty() && Misc::StringUtils::ciEqual(npcName(actor), name))
                    world->getRenderingManager()->rebuildPtr(actor);
        }
    }

    std::string_view npcName(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || !ptr.getClass().isNpc())
            return {};
        if (ptr == MWBase::Environment::get().getWorld()->getPlayerPtr())
            return {};
        return ptr.getClass().getName(ptr);
    }

    bool contains(std::string_view name)
    {
        return Misc::JigglePolicy::containsName(Settings::game().mJiggleNpcEnabledNames.get(), name);
    }

    bool add(std::string_view name)
    {
        if (name.empty() || contains(name))
            return false;
        std::vector<std::string> names = Settings::game().mJiggleNpcEnabledNames.get();
        names.emplace_back(name);
        Settings::game().mJiggleNpcEnabledNames.set(names);
        rebuildNpcsNamed(name);
        return true;
    }

    bool remove(std::string_view name)
    {
        const std::string removed(name);
        std::vector<std::string> names = Settings::game().mJiggleNpcEnabledNames.get();
        const auto it = std::remove_if(names.begin(), names.end(),
            [&](const std::string& item) { return Misc::StringUtils::ciEqual(item, removed); });
        if (it == names.end())
            return false;
        names.erase(it, names.end());
        Settings::game().mJiggleNpcEnabledNames.set(names);
        rebuildNpcsNamed(removed);
        return true;
    }

    void rebuildJigglingActors()
    {
        if (!inGame())
            return;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        world->getRenderingManager()->rebuildPtr(world->getPlayerPtr());
        for (const MWWorld::Ptr& actor : loadedActors())
        {
            const std::string_view name = npcName(actor);
            if (!name.empty() && contains(name))
                world->getRenderingManager()->rebuildPtr(actor);
        }
    }
}
