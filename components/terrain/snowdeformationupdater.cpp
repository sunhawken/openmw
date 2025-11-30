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

        // DEBUG: Add object mask uniform
        stateset->addUniform(new osg::Uniform("debugObjectMask", 8));
    }

    void SnowDeformationUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (mTerrainWorld && mTerrainWorld->getSnowDeformationManager())
        {
            auto* manager = mTerrainWorld->getSnowDeformationManager();
            
            // DEBUG: Verify execution and texture
            static int logCounter = 0;
            if (logCounter++ % 300 == 0) // Log every 300 frames (approx 5s)
            {
                osg::Texture2D* tex = manager->getCurrentDeformationMap();
                Log(Debug::Info) << "SnowDeformationUpdater::apply - Binding Texture Unit 7. Texture Ptr: " << tex;
                if (tex)
                {
                     Log(Debug::Info) << "  Texture ID: " << tex->getTextureObject(0) 
                                      << " (may be 0 if not compiled yet)";
                }
            }

            // Update the texture binding on Unit 7 to point to the current Write Buffer (which contains the latest RTT result)
            // DEBUG: Bind Object Mask to Unit 7 to test binding mechanism (skipping ping-pong/float issues)
            stateset->setTextureAttributeAndModes(7, manager->getObjectMaskMap(), osg::StateAttribute::ON);

            // DEBUG: Bind object mask for visualization
            stateset->setTextureAttributeAndModes(8, manager->getObjectMaskMap(), osg::StateAttribute::ON);
        }
    }
}
