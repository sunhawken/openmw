#include "brutalfootsteps.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <istream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/Image>
#include <osg/StateSet>
#include <osg/Texture>
#include <osgUtil/IntersectionVisitor>
#include <osgUtil/LineSegmentIntersector>

#include <components/esm/util.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadltex.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwphysics/collisiontype.hpp"
#include "../mwphysics/raycasting.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include "actorutil.hpp"
#include "npcstats.hpp"

namespace MWMechanics::BrutalFootsteps
{
    namespace
    {
        struct Event
        {
            float mVolume[2];
            float mPitch[2];
            std::vector<VFS::Path::Normalized> mFiles;
        };

        using Table = std::unordered_map<std::string, Event>;

        const Table* table()
        {
            static bool loaded = false;
            static Table events;
            if (!loaded)
            {
                loaded = true;
                const VFS::Manager* vfs = MWBase::Environment::get().getResourceSystem()->getVFS();
                const VFS::Path::Normalized path("brutal/footsteps.txt");
                if (vfs->exists(path))
                {
                    Files::IStreamPtr stream = vfs->get(path);
                    std::string line;
                    while (std::getline(*stream, line))
                    {
                        std::istringstream fields(line);
                        std::string name, files;
                        Event event;
                        if (!(fields >> name >> event.mVolume[0] >> event.mVolume[1] >> event.mPitch[0]
                                >> event.mPitch[1] >> files))
                            continue;
                        std::size_t start = 0;
                        while (start < files.size())
                        {
                            const std::size_t end = std::min(files.find(';', start), files.size());
                            if (end > start)
                            {
                                VFS::Path::Normalized file(std::string_view(files).substr(start, end - start));
                                if (vfs->exists(file))
                                    event.mFiles.push_back(std::move(file));
                            }
                            start = end + 1;
                        }
                        if (!event.mFiles.empty())
                            events.emplace(std::move(name), std::move(event));
                    }
                }
            }
            return events.empty() ? nullptr : &events;
        }

        float random()
        {
            // Cosmetic variation only; the game's own generator stays untouched.
            static uint32_t state = 0x2545f491u;
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return float(state & 0x00ffffff) / float(0x01000000);
        }

        bool emit(const MWWorld::Ptr& actor, const std::string& name)
        {
            const Table* events = table();
            if (!events)
                return false;
            const auto found = events->find(name);
            if (found == events->end())
                return false;
            const Event& event = found->second;
            const VFS::Path::Normalized& file
                = event.mFiles[std::min(event.mFiles.size() - 1, std::size_t(random() * event.mFiles.size()))];
            const float volume = event.mVolume[0] + (event.mVolume[1] - event.mVolume[0]) * random();
            const float pitch = event.mPitch[0] + (event.mPitch[1] - event.mPitch[0]) * random();
            // Dark Messiah plays steps at the same level as its combat sounds. Footstep sounds
            // here also follow the footsteps volume, whose default is 0.2: at that default the
            // balance between steps and blows is Dark Messiah's.
            constexpr float defaultFootstepsVolume = 0.2f;
            MWBase::Environment::get().getSoundManager()->playSound3D(actor, file, volume / defaultFootstepsVolume,
                pitch, MWSound::Type::Foot, MWSound::PlayMode::NoPlayerLocal);
            return true;
        }

        struct Rule
        {
            std::string_view mWord;
            std::string_view mSurface;
        };

        std::string_view classify(std::string_view name, const std::initializer_list<Rule>& rules,
            std::string_view fallback)
        {
            for (const Rule& rule : rules)
                if (name.find(rule.mWord) != std::string_view::npos)
                    return rule.mSurface;
            return fallback;
        }

        // Land texture file names, first match wins.
        std::string_view terrainSurface(const std::string& texture)
        {
            return classify(texture,
                { { "mud", "mud" }, { "muck", "mud" }, { "swamp", "mud" }, { "sand", "sand" }, { "snow", "sand" },
                    { "dirt", "dirt" }, { "ash", "dirt" }, { "earth", "dirt" }, { "tilled", "dirt" },
                    { "gravel", "dirt" }, { "grass", "grass" }, { "clover", "grass" }, { "moss", "grass" },
                    { "undergrowth", "grass" }, { "scrub", "grass" }, { "road", "concrete" }, { "cobble", "concrete" },
                    { "daed", "concrete" }, { "rock", "concrete" }, { "stone", "concrete" }, { "lava", "concrete" },
                    { "sulphur", "concrete" }, { "crack", "concrete" } },
                "dirt");
        }

        // Texture file name of the hit triangle, then the model path.
        std::string_view objectSurface(const std::string& name)
        {
            return classify(name,
                { { "carpet", "carpet" }, { "rug", "carpet" }, { "cloth", "carpet" }, { "fabric", "carpet" },
                    { "canvas", "carpet" }, { "redoran", "chitin" }, { "telvanni", "chitin" }, { "wood", "wood" }, { "plank", "wood" }, { "board", "wood" }, { "timber", "wood" },
                    { "barrel", "wood" }, { "crate", "wood" }, { "dock", "wood" }, { "ship", "wood" },
                    { "dwrv", "metal" }, { "dwemer", "metal" }, { "metal", "metal" }, { "iron", "metal" },
                    { "steel", "metal" }, { "bronze", "metal" }, { "grate", "metal" }, { "chitin", "chitin" },
                    { "shell", "chitin" }, { "mushroom", "chitin" }, { "glass", "glass" }, { "crystal", "glass" },
                    { "slate", "slate" }, { "mud", "mud" }, { "sand", "sand" }, { "grass", "grass" },
                    { "moss", "grass" }, { "dirt", "dirt" }, { "earth", "dirt" } },
                "concrete");
        }

        std::string landTexture(const osg::Vec3f& position, const MWWorld::CellStore& cell)
        {
            if (!cell.getCell()->isExterior() || cell.getCell()->getWorldSpace() != ESM::Cell::sDefaultWorldspaceId)
                return {};
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            const float cellSize = static_cast<float>(ESM::getCellSize(ESM::Cell::sDefaultWorldspaceId));
            // The texture grid is drawn a quarter texel off the height grid.
            const float offset = (cellSize / ESM::LandRecordData::sLandTextureSize) * 0.25f;
            const osg::Vec3f corrected = position + osg::Vec3f(-offset, offset, 0.f);
            const int cellX = static_cast<int>(std::floor(corrected.x() / cellSize));
            const int cellY = static_cast<int>(std::floor(corrected.y() / cellSize));
            const ESM::Land* land = store.get<ESM::Land>().search(cellX, cellY);
            const ESM::Land::LandData* data = land ? land->getLandData(ESM::Land::DATA_VTEX) : nullptr;
            if (!data)
                return {};
            const int x = std::clamp(
                static_cast<int>((corrected.x() / cellSize - cellX) * ESM::Land::LAND_TEXTURE_SIZE), 0,
                ESM::Land::LAND_TEXTURE_SIZE - 1);
            const int y = std::clamp(
                static_cast<int>((corrected.y() / cellSize - cellY) * ESM::Land::LAND_TEXTURE_SIZE), 0,
                ESM::Land::LAND_TEXTURE_SIZE - 1);
            const std::uint16_t texture = data->mTextures[y * ESM::Land::LAND_TEXTURE_SIZE + x];
            if (texture == 0)
                return {};
            const std::string* name = store.get<ESM::LandTexture>().search(texture - 1, land->getPlugin());
            return name ? Misc::StringUtils::lowerCase(*name) : std::string();
        }

        std::string hitTexture(const MWWorld::Ptr& object, const osg::Vec3f& position)
        {
            osg::Node* node = object.getRefData().getBaseNode();
            if (!node)
                return {};
            osg::ref_ptr<osgUtil::LineSegmentIntersector> intersector = new osgUtil::LineSegmentIntersector(
                osgUtil::Intersector::MODEL, position + osg::Vec3f(0, 0, 4), position - osg::Vec3f(0, 0, 4));
            osgUtil::IntersectionVisitor visitor(intersector);
            node->accept(visitor);
            if (!intersector->containsIntersections())
                return {};
            const osgUtil::LineSegmentIntersector::Intersection& hit = intersector->getFirstIntersection();
            std::vector<osg::Node*> path(hit.nodePath.begin(), hit.nodePath.end());
            if (hit.drawable)
                path.push_back(hit.drawable.get());
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                const osg::StateSet* state = (*it)->getStateSet();
                const osg::Texture* texture = state
                    ? dynamic_cast<const osg::Texture*>(state->getTextureAttribute(0, osg::StateAttribute::TEXTURE))
                    : nullptr;
                if (texture && texture->getImage(0))
                    return Misc::StringUtils::lowerCase(texture->getImage(0)->getFileName());
            }
            return {};
        }

        std::string_view playerSurface(const MWWorld::Ptr& actor)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const osg::Vec3f position = actor.getRefData().getPosition().asVec3();
            if (world->isWalkingOnWater(actor))
                return "water";
            const MWWorld::CellStore* cell = actor.getCell();
            if (cell->getCell()->hasWater() && position.z() < cell->getWaterLevel())
                return cell->getWaterLevel() - position.z() < 25.f ? "water" : "wade";
            const MWPhysics::RayCastingResult ray = world->getRayCasting()->castRay(position + osg::Vec3f(0, 0, 10),
                position - osg::Vec3f(0, 0, 60), { actor },
                {}, MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap | MWPhysics::CollisionType_Door);
            if (!ray.mHit)
                return "concrete";
            if (ray.mHitObject.isEmpty())
                return terrainSurface(landTexture(ray.mHitPos, *cell));
            std::string texture = hitTexture(ray.mHitObject, ray.mHitPos);
            const std::string_view surface = objectSurface(texture);
            if (!texture.empty() && surface != "concrete")
                return surface;
            // Stone textures rarely say so; the model name often does when the texture does not.
            const std::string model = Misc::StringUtils::lowerCase(ray.mHitObject.getClass().getCorrectedModel(ray.mHitObject));
            return objectSurface(texture + ' ' + model);
        }
    }

    bool available()
    {
        return Settings::game().mBrutalEnabled && Settings::game().mBrutalCombatSounds && table();
    }

    bool play(const MWWorld::Ptr& actor, bool left, bool landing, Gait gait)
    {
        if (!available())
            return false;
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world->isFlying(actor) || world->isSwimming(actor))
            return false;
        const char* side = left ? "left" : "right";
        if (actor != getPlayer())
        {
            if (!actor.getClass().isNpc() || actor.getClass().getNpcStats(actor).isWerewolf())
                return false;
            // Dark Messiah gives NPCs one step per character type: an armoured guard, or
            // a villager's softer tread.
            const MWWorld::InventoryStore& inventory = actor.getClass().getInventoryStore(actor);
            const MWWorld::ConstContainerStoreIterator boots = inventory.getSlot(MWWorld::InventoryStore::Slot_Boots);
            bool armoured = false;
            if (boots != inventory.end() && boots->getType() == ESM::Armor::sRecordId)
            {
                const ESM::RefId skill = boots->getClass().getEquipmentSkill(*boots);
                armoured = skill == ESM::Skill::MediumArmor || skill == ESM::Skill::HeavyArmor;
            }
            return emit(actor, std::string(armoured ? "guard" : "villagermale") + ".footstep" + side);
        }
        static constexpr std::array<std::string_view, 4> gaits{ "", "stealth", "run", "sprint" };
        std::string name(playerSurface(actor));
        name += ".step";
        name += side;
        name += gaits[static_cast<int>(gait)];
        if (landing)
            name += "jump";
        return emit(actor, name);
    }

    bool playJump(const MWWorld::Ptr& player)
    {
        return available() && emit(player, "player.jump");
    }
}
