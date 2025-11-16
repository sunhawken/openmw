#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATIONUPDATER_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATIONUPDATER_H

#include <osg/StateSet>
#include <components/sceneutil/statesetupdater.hpp>

namespace Terrain
{
    class World;

    /// StateSetUpdater that adds snow deformation uniforms to terrain
    /// With the vertex shader array approach, this simply adds the uniform
    /// references to the terrain stateset. The uniforms are updated directly
    /// by SnowDeformationManager each frame.
    class SnowDeformationUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        SnowDeformationUpdater(World* terrainWorld);

        void setDefaults(osg::StateSet* stateset) override;
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

    private:
        World* mTerrainWorld;
    };
}

#endif
