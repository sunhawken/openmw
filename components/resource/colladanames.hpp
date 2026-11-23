#ifndef OPENMW_COMPONENTS_RESOURCE_COLLADANAMES_H
#define OPENMW_COMPONENTS_RESOURCE_COLLADANAMES_H

#include <istream>
#include <map>
#include <set>
#include <string>
#include <string_view>

namespace Resource
{
    /// Names recorded in a Collada document. OSG's Collada reader names bones and
    /// animations after their XML `id`, which exporters derive from the real name
    /// (spaces and other characters replaced). The `name` attribute keeps the
    /// exact name; these maps let the loader restore it.
    struct ColladaNames
    {
        std::map<std::string, std::string, std::less<>> mNodes; // node id -> name
        std::set<std::string, std::less<>> mNodeIds; // every node id, named or not
        std::map<std::string, std::string, std::less<>> mClips; // animation_clip id -> name

        bool empty() const { return mNodes.empty() && mClips.empty() && mNodeIds.empty(); }
    };

    /// Reads `<node id=.. name=..>` and `<animation_clip id=.. name=..>` from a
    /// Collada document. Elements without both attributes are skipped.
    ColladaNames parseColladaNames(std::string_view document);

    /// Name a scene node (bone) gets after loading.
    ///
    /// A node with an explicit `name` attribute keeps that name exactly,
    /// underscores included. A node without one (older exports) gets the old
    /// rule, underscores become spaces, but only when the spaced form is not
    /// already the id or name of another node in the document; otherwise its
    /// loaded name is kept so two bones never collapse into one.
    std::string colladaNodeName(const ColladaNames& names, const std::string& loadedName);

    /// Name an animation (clip) gets after loading: the clip's `name`, or the
    /// loaded name when the document has none.
    std::string colladaClipName(const ColladaNames& names, const std::string& loadedName);

    /// Reads \a stream from its current position to the end and returns the text
    /// followed by one NUL byte. OSG's Collada reader copies its input stream
    /// into a buffer of exactly the stream's length and parses it as a
    /// NUL-terminated string; the trailing NUL makes that buffer terminated.
    std::string readColladaForReader(std::istream& stream);
}

#endif
