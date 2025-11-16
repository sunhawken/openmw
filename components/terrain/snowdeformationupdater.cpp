#include "snowdeformationupdater.hpp"
#include "snowdeformation.hpp"
#include "world.hpp"

#include <components/debug/debuglog.hpp>

namespace Terrain
{
    SnowDeformationUpdater::SnowDeformationUpdater(World* terrainWorld)
        : mTerrainWorld(terrainWorld)
    {
        Log(Debug::Info) << "[SNOW UPDATER] Created (vertex shader array approach)";
    }

    void SnowDeformationUpdater::setDefaults(osg::StateSet* stateset)
    {
        if (!mTerrainWorld)
            return;

        SnowDeformationManager* manager = mTerrainWorld->getSnowDeformationManager();
        if (!manager)
            return;

        // Add all snow deformation uniforms to the terrain stateset
        // These will be shared across all terrain chunks
        stateset->addUniform(manager->getFootprintPositionsUniform());
        stateset->addUniform(manager->getFootprintCountUniform());
        stateset->addUniform(manager->getFootprintRadiusUniform());
        stateset->addUniform(manager->getDeformationDepthUniform());
        stateset->addUniform(manager->getCurrentTimeUniform());
        stateset->addUniform(manager->getDecayTimeUniform());

        // Create and add the enabled uniform (defaults to true for testing)
        osg::ref_ptr<osg::Uniform> enabledUniform = new osg::Uniform("snowDeformationEnabled", true);
        stateset->addUniform(enabledUniform);

        Log(Debug::Info) << "[SNOW UPDATER] Uniforms added to terrain stateset";
    }

    void SnowDeformationUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        // With the vertex shader array approach, uniforms are updated directly by
        // SnowDeformationManager in its update() function
        // This callback is now essentially a no-op, but kept for API compatibility

        // Could add per-frame diagnostics here if needed
    }
}
