#include "snowdeformationupdater.hpp"
#include "snowdeformation.hpp"
#include "world.hpp"

#include <components/debug/debuglog.hpp>

namespace Terrain
{
    SnowDeformationUpdater::SnowDeformationUpdater(World* terrainWorld)
        : mTerrainWorld(terrainWorld)
    {
    }

    void SnowDeformationUpdater::setDefaults(osg::StateSet* stateset)
    {
        if (!mTerrainWorld)
            return;

        SnowDeformationManager* manager = mTerrainWorld->getSnowDeformationManager();
        if (!manager)
            return;

        // Add all terrain deformation uniforms to the terrain stateset
        // These will be shared across all terrain chunks
        // Add RTT uniforms
        stateset->addUniform(manager->getDeformationMapUniform());
        stateset->setTextureAttributeAndModes(7, manager->getDeformationMap(), osg::StateAttribute::ON);
        stateset->addUniform(manager->getRTTWorldOriginUniform());
        stateset->addUniform(manager->getRTTScaleUniform());

        Log(Debug::Info) << "SnowDeformationUpdater::setDefaults - Added RTT uniforms to terrain stateset";

        // Terrain-specific parameters
        stateset->addUniform(manager->getDeformationDepthUniform());
        stateset->addUniform(manager->getAshDeformationDepthUniform());
        stateset->addUniform(manager->getMudDeformationDepthUniform());
        stateset->addUniform(manager->getCurrentTimeUniform());

        // Create and add the enabled uniform (defaults to true for testing)
        osg::ref_ptr<osg::Uniform> enabledUniform = new osg::Uniform("snowDeformationEnabled", true);
        stateset->addUniform(enabledUniform);
    }

    void SnowDeformationUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        // With the vertex shader array approach, uniforms are updated directly by
        // SnowDeformationManager in its update() function
        // This callback is now essentially a no-op, but kept for API compatibility

        // Could add per-frame diagnostics here if needed
    }
}
