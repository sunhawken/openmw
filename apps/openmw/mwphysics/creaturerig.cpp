#include "creaturerig.hpp"
#include <algorithm>
#include <cmath>
#include <istream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcrea.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include "../mwbase/environment.hpp"
#include "../mwworld/livecellref.hpp"
#include "../mwworld/ptr.hpp"
namespace MWPhysics
{
    namespace
    {
        constexpr VFS::Path::NormalizedView sRigFile("brutal/creature_rigs.txt");
        constexpr float sHumanMass = 64.f; // total mass of the NPC ragdoll

        std::string rigKey(std::string_view model)
        {
            std::string key = Misc::StringUtils::lowerCase(model);
            std::replace(key.begin(), key.end(), '/', '\\');
            return key;
        }

        std::vector<std::string> split(const std::string& line)
        {
            std::vector<std::string> fields;
            std::stringstream stream(line);
            std::string field;
            while (std::getline(stream, field, '\t'))
                fields.push_back(field);
            return fields;
        }

        CreatureRigPart parsePart(const std::vector<std::string>& fields, std::size_t index)
        {
            if (fields.size() != 14)
                throw std::runtime_error("expected 14 fields in a part line");
            CreatureRigPart part;
            part.mBone = fields[0];
            part.mParent = std::stoi(fields[1]);
            part.mCenter = osg::Vec3f(std::stof(fields[2]), std::stof(fields[3]), std::stof(fields[4]));
            part.mAxis = osg::Vec3f(std::stof(fields[5]), std::stof(fields[6]), std::stof(fields[7]));
            part.mHalfLength = std::stof(fields[8]);
            part.mRadius = std::stof(fields[9]);
            part.mMass = std::stof(fields[10]);
            part.mSwing1 = std::stof(fields[11]);
            part.mSwing2 = std::stof(fields[12]);
            part.mTwist = std::stof(fields[13]);
            if ((index == 0) != (part.mParent < 0) || part.mParent >= static_cast<int>(index) || part.mMass <= 0.f
                || part.mRadius <= 0.f || part.mAxis.normalize() < 1e-3f)
                throw std::runtime_error("invalid part " + part.mBone);
            return part;
        }

        std::map<std::string, CreatureRig> loadRigs()
        {
            std::map<std::string, CreatureRig> rigs;
            const auto* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
            if (!vfs->exists(sRigFile))
                return rigs;
            const auto stream = vfs->get(sRigFile);
            std::string line, model;
            std::size_t remaining = 0;
            CreatureRig current;
            while (std::getline(*stream, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                const auto fields = split(line);
                try
                {
                    if (remaining == 0)
                    {
                        if (fields.size() != 3 || fields[0] != "rig")
                            throw std::runtime_error("expected a rig line");
                        model = fields[1];
                        remaining = std::stoul(fields[2]);
                        current = {};
                        if (remaining == 0)
                            throw std::runtime_error("rig without parts");
                        continue;
                    }
                    CreatureRigPart part = parsePart(fields, current.mParts.size());
                    current.mTotalMass += part.mMass;
                    current.mParts.push_back(std::move(part));
                    if (--remaining == 0)
                        rigs.emplace(rigKey(model), std::move(current));
                }
                catch (const std::exception& error)
                {
                    Log(Debug::Error) << "Creature rig file rejected, creatures keep their death animations: "
                                      << error.what() << " in: " << line;
                    return {};
                }
            }
            Log(Debug::Info) << "Loaded " << rigs.size() << " creature ragdoll rigs";
            return rigs;
        }
    }

    float CreatureRig::kickScale(float actorScale) const
    {
        const float mass = mTotalMass * std::pow(std::max(actorScale, .01f), 2.25f);
        // Never above the NPC's push: its recoil is already a long stagger.
        return std::clamp(std::sqrt(sHumanMass / std::max(mass, 1.f)), .25f, 1.f);
    }

    const CreatureRig* findCreatureRig(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || ptr.getType() != ESM::Creature::sRecordId)
            return nullptr;
        static const std::map<std::string, CreatureRig> rigs = loadRigs();
        const auto found = rigs.find(rigKey(ptr.get<ESM::Creature>()->mBase->mModel));
        return found == rigs.end() ? nullptr : &found->second;
    }
}
