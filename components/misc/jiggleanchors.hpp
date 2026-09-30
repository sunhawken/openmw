#ifndef OPENMW_COMPONENTS_MISC_JIGGLEANCHORS_H
#define OPENMW_COMPONENTS_MISC_JIGGLEANCHORS_H

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <components/settings/values.hpp>

namespace Misc::JiggleAnchors
{
    // Hand-placed jiggle anchors, saved per body mesh. A pair holds the (y, z) mesh-space position of
    // the left and right anchor (y is lateral, +y = the character's left, z is height); the depth is
    // found on the mesh surface when the bones are built. Kind 'b' = breasts, keyed by the chest mesh;
    // kind 'u' = butt, keyed by the mesh that is weighted to the pelvis. Persisted in the
    // "jiggle mesh anchors" setting as "kind|meshpath=leftY;leftZ;rightY;rightZ" entries.
    struct Pair
    {
        float mLeftY = 0.f, mLeftZ = 0.f, mRightY = 0.f, mRightZ = 0.f;
    };

    // The meshes of the player's current outfit, set by the auto-rigger when the player is (re)built.
    inline std::string& currentChestMesh()
    {
        static std::string sMesh;
        return sMesh;
    }
    inline std::string& currentPelvisMesh()
    {
        static std::string sMesh;
        return sMesh;
    }

    inline std::string key(char kind, std::string_view mesh)
    {
        return std::string(1, kind) + "|" + std::string(mesh);
    }

    inline std::optional<Pair> parse(std::string_view entry, const std::string& wantedKey)
    {
        const std::size_t eq = entry.rfind('=');
        if (eq == std::string_view::npos || entry.substr(0, eq) != wantedKey)
            return std::nullopt;
        std::string text(entry.substr(eq + 1));
        float values[4];
        std::size_t start = 0;
        for (int i = 0; i < 4; ++i)
        {
            const std::size_t end = text.find(';', start);
            const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            char* endPtr = nullptr;
            errno = 0;
            values[i] = std::strtof(part.c_str(), &endPtr);
            if (endPtr == part.c_str() || errno == ERANGE)
                return std::nullopt;
            if (end == std::string::npos && i < 3)
                return std::nullopt;
            start = end == std::string::npos ? text.size() : end + 1;
        }
        return Pair{ values[0], values[1], values[2], values[3] };
    }

    inline std::optional<Pair> lookup(char kind, std::string_view mesh)
    {
        if (mesh.empty())
            return std::nullopt;
        const std::string wanted = key(kind, mesh);
        for (const std::string& entry : Settings::game().mJiggleMeshAnchors.get())
            if (auto parsed = parse(entry, wanted))
                return parsed;
        return std::nullopt;
    }

    inline void reset(char kind, std::string_view mesh)
    {
        if (mesh.empty())
            return;
        const std::string wanted = key(kind, mesh);
        std::vector<std::string> out;
        for (const std::string& entry : Settings::game().mJiggleMeshAnchors.get())
        {
            const std::size_t eq = entry.rfind('=');
            if (eq == std::string::npos || entry.substr(0, eq) != wanted)
                out.push_back(entry);
        }
        Settings::game().mJiggleMeshAnchors.set(out);
    }

    inline void save(char kind, std::string_view mesh, const Pair& pair)
    {
        if (mesh.empty())
            return;
        reset(kind, mesh);
        std::vector<std::string> out = Settings::game().mJiggleMeshAnchors.get();
        out.push_back(key(kind, mesh) + '=' + std::to_string(pair.mLeftY) + ';' + std::to_string(pair.mLeftZ) + ';'
            + std::to_string(pair.mRightY) + ';' + std::to_string(pair.mRightZ));
        Settings::game().mJiggleMeshAnchors.set(out);
    }
}

#endif
