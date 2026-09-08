#ifndef OPENMW_COMPONENTS_MISC_JIGGLEZOFFSET_H
#define OPENMW_COMPONENTS_MISC_JIGGLEZOFFSET_H

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

    inline std::optional<std::pair<float, float>> parseEntry(std::string_view entry, std::string_view meshFile)
    {
        const std::size_t eq = entry.rfind('=');
        if (eq == std::string_view::npos)
            return std::nullopt;
        if (entry.substr(0, eq) != meshFile)
            return std::nullopt;
        std::string_view rest = entry.substr(eq + 1);
        const std::size_t comma = rest.find(',');
        if (comma == std::string_view::npos)
            return std::nullopt;
        const auto toFloat = [](std::string_view s) -> std::optional<float> {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
                s.remove_suffix(1);
            float v = 0.f;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
            if (res.ec != std::errc())
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
                out.push_back(std::string(meshFile) + '=' + std::to_string(breast) + ',' + std::to_string(butt));
                replaced = true;
            }
            else
                out.push_back(entry);
        }
        if (!replaced)
            out.push_back(std::string(meshFile) + '=' + std::to_string(breast) + ',' + std::to_string(butt));
        Settings::game().mJiggleMeshZOffsets.set(out);
    }
}

#endif
