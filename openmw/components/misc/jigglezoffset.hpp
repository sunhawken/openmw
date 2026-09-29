#ifndef OPENMW_COMPONENTS_MISC_JIGGLEZOFFSET_H
#define OPENMW_COMPONENTS_MISC_JIGGLEZOFFSET_H

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <unordered_map>

#include <components/misc/strings/algorithm.hpp>

#include <components/settings/values.hpp>

namespace Misc::JiggleZOffset
{
    // Per-body-mesh breast/butt jiggle Z offsets, persisted in the "jiggle mesh z offsets" setting
    // as "meshpath=breast,butt" entries and keyed by the player's current body mesh. The in-game
    // sliders edit the entry for whichever body mesh the player is currently using, so each mesh
    // remembers its own tuning while the sliders stay live for the player.

    // The player's current body mesh path (lowercased VFS path), set by NpcAnimation when the
    // player body is (re)built. Inline so it has a single definition across translation units.
    inline std::string& currentPlayerMesh()
    {
        static std::string sMesh;
        return sMesh;
    }

    inline std::unordered_map<std::string, std::string>& currentActorMeshes()
    {
        static std::unordered_map<std::string, std::string> sMeshes;
        return sMeshes;
    }

    inline std::string normalizedActorScope(bool isPlayer, std::string_view actorName)
    {
        if (isPlayer)
            return "player";
        std::string clean = Misc::StringUtils::lowerCase(std::string(actorName));
        for (char& c : clean)
            if (c == '|' || c == '=' || c == ',')
                c = '_';
        return "npc:" + clean;
    }

    inline void setCurrentActorMesh(bool isPlayer, std::string_view actorName, std::string_view meshFile)
    {
        const std::string scope = normalizedActorScope(isPlayer, actorName);
        currentActorMeshes()[scope] = std::string(meshFile);
        if (isPlayer)
            currentPlayerMesh() = std::string(meshFile);
    }

    inline std::string currentActorMesh(bool isPlayer, std::string_view actorName)
    {
        const std::string scope = normalizedActorScope(isPlayer, actorName);
        const auto it = currentActorMeshes().find(scope);
        return it == currentActorMeshes().end() ? std::string() : it->second;
    }

    inline std::optional<std::pair<float, float>> parseEntry(std::string_view entry, std::string_view meshFile)
    {
        const std::size_t eq = entry.rfind('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        if (entry.substr(0, eq) != meshFile)
            return std::nullopt;
        std::string_view rest = entry.substr(eq + 1);
        // breast/butt are separated by ';' - NOT ',', because the whole setting is a comma-separated
        // list, so a comma inside an entry would be split into separate (broken) list elements.
        const std::size_t comma = rest.find(';');
        if (comma == std::string_view::npos)
            return std::nullopt;
        const auto toFloat = [](std::string_view s) -> std::optional<float> {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            // libc++ used by current macOS runners does not provide floating-point
            // std::from_chars on all supported deployment targets. strtof is portable here and
            // the setting values are short, locale-independent numbers written by std::to_string.
            const std::string text(s);
            char* end = nullptr;
            errno = 0;
            const float v = std::strtof(text.c_str(), &end);
            if (end == text.c_str() || !end || *end != '\0' || errno == ERANGE)
                return std::nullopt;
            return v;
        };
        const auto breast = toFloat(rest.substr(0, comma));
        const auto butt = toFloat(rest.substr(comma + 1));
        if (!breast || !butt)
            return std::nullopt;
        return std::make_pair(*breast, *butt);
    }

    // Stored offsets for a given body mesh, if any.
    inline std::optional<std::pair<float, float>> lookup(std::string_view meshFile)
    {
        if (meshFile.empty())
            return std::nullopt;
        for (const std::string& entry : Settings::game().mJiggleMeshZOffsets.get())
        {
            if (auto parsed = parseEntry(entry, meshFile))
                return parsed;
        }
        return std::nullopt;
    }

    inline std::optional<std::pair<float, float>> lookupScoped(
        std::string_view scope, std::string_view meshFile)
    {
        if (scope.empty() || meshFile.empty())
            return std::nullopt;
        const std::string key = std::string(scope) + "|" + std::string(meshFile);
        for (const std::string& entry : Settings::game().mJiggleScopedMeshZOffsets.get())
        {
            if (auto parsed = parseEntry(entry, key))
                return parsed;
        }
        return std::nullopt;
    }

    inline std::optional<std::pair<float, float>> lookupForActor(
        bool isPlayer, std::string_view actorName, std::string_view meshFile)
    {
        const std::string scope = normalizedActorScope(isPlayer, actorName);
        if (auto value = lookupScoped(scope, meshFile))
            return value;
        return lookup(meshFile);
    }

    inline void saveScoped(std::string_view scope, std::string_view meshFile, float breast, float butt)
    {
        if (scope.empty() || meshFile.empty())
            return;
        const std::string key = std::string(scope) + "|" + std::string(meshFile);
        std::vector<std::string> out;
        bool replaced = false;
        for (const std::string& entry : Settings::game().mJiggleScopedMeshZOffsets.get())
        {
            const std::size_t eq = entry.rfind('=');
            if (eq != std::string::npos && std::string_view(entry).substr(0, eq) == key)
            {
                out.push_back(key + '=' + std::to_string(breast) + ';' + std::to_string(butt));
                replaced = true;
            }
            else
                out.push_back(entry);
        }
        if (!replaced)
            out.push_back(key + '=' + std::to_string(breast) + ';' + std::to_string(butt));
        Settings::game().mJiggleScopedMeshZOffsets.set(out);
    }

    inline void resetScoped(std::string_view entryKey)
    {
        if (entryKey.empty())
            return;
        std::vector<std::string> out;
        for (const std::string& entry : Settings::game().mJiggleScopedMeshZOffsets.get())
        {
            const std::size_t eq = entry.rfind('=');
            if (eq == std::string::npos || std::string_view(entry).substr(0, eq) != entryKey)
                out.push_back(entry);
        }
        Settings::game().mJiggleScopedMeshZOffsets.set(out);
    }

    // Save (or replace) the offsets for a body mesh; persists to settings.cfg.
    inline void save(std::string_view meshFile, float breast, float butt)
    {
        if (meshFile.empty())
            return;
        std::vector<std::string> out;
        bool replaced = false;
        for (const std::string& entry : Settings::game().mJiggleMeshZOffsets.get())
        {
            const std::size_t eq = entry.rfind('=');
            if (eq != std::string::npos && std::string_view(entry).substr(0, eq) == meshFile)
            {
                out.push_back(std::string(meshFile) + '=' + std::to_string(breast) + ';' + std::to_string(butt));
                replaced = true;
            }
            else
                out.push_back(entry);
        }
        if (!replaced)
            out.push_back(std::string(meshFile) + '=' + std::to_string(breast) + ';' + std::to_string(butt));
        Settings::game().mJiggleMeshZOffsets.set(out);
    }

    // Remove the saved tuning for one body mesh. The next time that naked body, clothing,
    // or armor is equipped it will use the neutral (zero) breast/butt offset again.
    inline void reset(std::string_view meshFile)
    {
        if (meshFile.empty())
            return;
        std::vector<std::string> out;
        for (const std::string& entry : Settings::game().mJiggleMeshZOffsets.get())
        {
            const std::size_t eq = entry.rfind('=');
            if (eq == std::string::npos || std::string_view(entry).substr(0, eq) != meshFile)
                out.push_back(entry);
        }
        Settings::game().mJiggleMeshZOffsets.set(out);
    }
}

#endif
