#include "snowdeformationupdater.hpp"
#include "snowdeformation.hpp"
#include "world.hpp"

#include <components/debug/debuglog.hpp>

namespace Terrain
{
    SnowDeformationUpdater::SnowDeformationUpdater(World* terrainWorld)
        : mTerrainWorld(terrainWorld)
        , mTextureUnit(7)  // Use texture unit 7 for deformation map
    {
        // Create uniforms
        mDeformationMapUniform = new osg::Uniform("snowDeformationMap", mTextureUnit);
        mDeformationCenterUniform = new osg::Uniform("snowDeformationCenter", osg::Vec2f(0.0f, 0.0f));
        mDeformationRadiusUniform = new osg::Uniform("snowDeformationRadius", 150.0f);
        mDeformationEnabledUniform = new osg::Uniform("snowDeformationEnabled", false);
    }

    void SnowDeformationUpdater::setDefaults(osg::StateSet* stateset)
    {
        // Add uniforms to stateset
        stateset->addUniform(mDeformationMapUniform);
        stateset->addUniform(mDeformationCenterUniform);
        stateset->addUniform(mDeformationRadiusUniform);
        stateset->addUniform(mDeformationEnabledUniform);
    }

    void SnowDeformationUpdater::apply(osg::StateSet* stateset, osg::NodeVisitor* nv)
    {
        if (!mTerrainWorld)
        {
            Log(Debug::Warning) << "[SNOW UPDATER] No terrain world!";
            return;
        }

        SnowDeformationManager* manager = mTerrainWorld->getSnowDeformationManager();
        if (!manager)
        {
            Log(Debug::Warning) << "[SNOW UPDATER] No deformation manager!";
            mDeformationEnabledUniform->set(false);
            return;
        }

        // Get current deformation texture
        osg::Texture2D* deformationTexture = manager->getDeformationTexture();

        if (deformationTexture && manager->isEnabled())
        {
            // Bind deformation texture
            stateset->setTextureAttributeAndModes(mTextureUnit,
                deformationTexture, osg::StateAttribute::ON);

            // Update uniforms
            osg::Vec2f center;
            float radius;
            manager->getDeformationTextureParams(center, radius);

            mDeformationCenterUniform->set(center);
            mDeformationRadiusUniform->set(radius);
            mDeformationEnabledUniform->set(true);

            static int logCount = 0;
            if (logCount++ < 5)
            {
                Log(Debug::Info) << "[SNOW UPDATER] Binding deformation texture at ("
                                << (int)center.x() << ", " << (int)center.y()
                                << ") radius=" << radius
                                << " textureUnit=" << mTextureUnit;
            }
        }
        else
        {
            // Disable deformation
            static bool warned = false;
            if (!warned)
            {
                Log(Debug::Warning) << "[SNOW UPDATER] No deformation texture or disabled! "
                                   << "texture=" << (deformationTexture ? "valid" : "null")
                                   << " enabled=" << manager->isEnabled();
                warned = true;
            }
            mDeformationEnabledUniform->set(false);
        }
    }
}
