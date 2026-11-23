#include "colladanames.hpp"

#include <iterator>
#include <set>

#include <components/misc/strings/algorithm.hpp>

namespace Resource
{
    namespace
    {
        std::string decodeEntities(std::string_view value)
        {
            std::string out;
            out.reserve(value.size());
            for (std::size_t i = 0; i < value.size(); ++i)
            {
                if (value[i] != '&')
                {
                    out += value[i];
                    continue;
                }
                const std::size_t end = value.find(';', i);
                if (end == std::string_view::npos)
                {
                    out += value[i];
                    continue;
                }
                const std::string_view entity = value.substr(i + 1, end - i - 1);
                if (entity == "amp")
                    out += '&';
                else if (entity == "lt")
                    out += '<';
                else if (entity == "gt")
                    out += '>';
                else if (entity == "quot")
                    out += '"';
                else if (entity == "apos")
                    out += '\'';
                else if (!entity.empty() && entity[0] == '#')
                {
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    const std::string digits(entity.substr(hex ? 2 : 1));
                    const unsigned long code = std::stoul(digits.empty() ? "0" : digits, nullptr, hex ? 16 : 10);
                    if (code < 0x80)
                        out += static_cast<char>(code);
                    else
                        out += '?';
                }
                else
                {
                    out += value.substr(i, end - i + 1);
                }
                i = end;
            }
            return out;
        }

        // Finds attribute `attr` inside a start tag (the text between '<' and '>').
        bool findAttribute(std::string_view tag, std::string_view attr, std::string& out)
        {
            std::size_t pos = 0;
            while ((pos = tag.find(attr, pos)) != std::string_view::npos)
            {
                const bool boundary = pos == 0 || tag[pos - 1] == ' ' || tag[pos - 1] == '\t' || tag[pos - 1] == '\n'
                    || tag[pos - 1] == '\r';
                std::size_t p = pos + attr.size();
                while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\n' || tag[p] == '\r'))
                    ++p;
                if (boundary && p < tag.size() && tag[p] == '=')
                {
                    ++p;
                    while (p < tag.size() && (tag[p] == ' ' || tag[p] == '\t' || tag[p] == '\n' || tag[p] == '\r'))
                        ++p;
                    if (p < tag.size() && (tag[p] == '"' || tag[p] == '\''))
                    {
                        const char quote = tag[p];
                        const std::size_t end = tag.find(quote, p + 1);
                        if (end == std::string_view::npos)
                            return false;
                        out = decodeEntities(tag.substr(p + 1, end - p - 1));
                        return true;
                    }
                }
                pos += attr.size();
            }
            return false;
        }

        void collect(std::string_view document, std::string_view element,
            std::map<std::string, std::string, std::less<>>& target, std::set<std::string, std::less<>>* ids)
        {
            const std::string open = "<" + std::string(element);
            std::size_t pos = 0;
            while ((pos = document.find(open, pos)) != std::string_view::npos)
            {
                const std::size_t after = pos + open.size();
                pos = after;
                if (after >= document.size())
                    break;
                const char next = document[after];
                if (next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '>' && next != '/')
                    continue; // e.g. <node_extra>
                const std::size_t end = document.find('>', after);
                if (end == std::string_view::npos)
                    break;
                const std::string_view tag = document.substr(after, end - after);
                std::string id, name;
                const bool hasId = findAttribute(tag, "id", id) && !id.empty();
                if (hasId && ids)
                    ids->insert(id);
                if (hasId && findAttribute(tag, "name", name) && !name.empty())
                    target.emplace(std::move(id), std::move(name)); // first wins on a duplicate id
                pos = end;
            }
        }

    }

    ColladaNames parseColladaNames(std::string_view document)
    {
        ColladaNames names;
        collect(document, "node", names.mNodes, &names.mNodeIds);
        collect(document, "animation_clip", names.mClips, nullptr);
        return names;
    }

    std::string colladaNodeName(const ColladaNames& names, const std::string& loadedName)
    {
        if (const auto found = names.mNodes.find(loadedName); found != names.mNodes.end())
            return found->second; // Explicit name: exact.

        const std::string spaced = Misc::StringUtils::underscoresToSpaces(loadedName);
        if (spaced == loadedName)
            return loadedName;
        if (names.mNodeIds.contains(spaced))
            return loadedName;
        for (const auto& [id, name] : names.mNodes)
            if (name == spaced)
                return loadedName;
        return spaced;
    }

    std::string colladaClipName(const ColladaNames& names, const std::string& loadedName)
    {
        const auto found = names.mClips.find(loadedName);
        return found == names.mClips.end() ? loadedName : found->second;
    }

    std::string readColladaForReader(std::istream& stream)
    {
        std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        text.push_back('\0');
        return text;
    }
}
