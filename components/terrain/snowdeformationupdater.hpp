#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATIONUPDATER_H
#define OPENMW_COMPONENTS_TERRAIN_SNOWDEFORMATIONUPDATER_H

#include <osg/StateSet>
#include <osg/Uniform>
#include <osg/Texture2D>
#include <components/sceneutil/statesetupdater.hpp>

namespace Terrain
{
    class SnowDeformationManager;
    class World;

    /// StateSetUpdater that binds snow deformation textures and uniforms to terrain
    class SnowDeformationUpdater : public SceneUtil::StateSetUpdater
    {
    public:
        SnowDeformationUpdater(World* terrainWorld);

        void setDefaults(osg::StateSet* stateset) override;
        void apply(osg::StateSet* stateset, osg::NodeVisitor* nv) override;

    private:
        World* mTerrainWorld;
        osg::ref_ptr<osg::Uniform> mDeformationMapUniform;
        osg::ref_ptr<osg::Uniform> mDeformationCenterUniform;
        osg::ref_ptr<osg::Uniform> mDeformationRadiusUniform;
        osg::ref_ptr<osg::Uniform> mDeformationEnabledUniform;
        int mTextureUnit;
    };
}

#endif
