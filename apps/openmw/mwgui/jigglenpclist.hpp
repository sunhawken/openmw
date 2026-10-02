#ifndef OPENMW_MWGUI_JIGGLENPCLIST_H
#define OPENMW_MWGUI_JIGGLENPCLIST_H

#include <string_view>

namespace MWWorld
{
    class Ptr;
}

namespace MWGui::JiggleNpcList
{
    /// Body jiggle is on for the player and for the NPCs on "jiggle npc enabled names" only.
    /// These edit that list (display names, case-insensitive) and rebuild every loaded NPC with
    /// that name, so the change shows at once instead of after a reload.
    bool contains(std::string_view name);
    /// Returns false when the name is empty or already listed.
    bool add(std::string_view name);
    /// Returns false when the name was not listed.
    bool remove(std::string_view name);

    /// Rebuilds the player and every loaded NPC on the list, e.g. after an auto-rig, blacklist or
    /// mesh-offset change, so the change shows without re-equipping or reloading.
    void rebuildJigglingActors();

    /// The NPC display name for a console selection, or empty when the selection is not an NPC
    /// (or is the player).
    std::string_view npcName(const MWWorld::Ptr& ptr);
}

#endif
