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
        // Create uniforms with default values
        mDeformationMapUniform = new osg::Uniform("snowDeformationMap", mTextureUnit);
        mDeformationCenterUniform = new osg::Uniform("snowDeformationCenter", osg::Vec2f(0.0f, 0.0f));
        mDeformationRadiusUniform = new osg::Uniform("snowDeformationRadius", 150.0f);
        mDeformationEnabledUniform = new osg::Uniform("snowDeformationEnabled", false);
        mRaiseAmountUniform = new osg::Uniform("snowRaiseAmount", 100.0f);  // Default for snow terrain
    }

    void SnowDeformationUpdater::setDefaults(osg::StateSet* stateset)
    {
        // Add uniforms to stateset
        stateset->addUniform(mDeformationMapUniform);
        stateset->addUniform(mDeformationCenterUniform);
        stateset->addUniform(mDeformationRadiusUniform);
        stateset->addUniform(mDeformationEnabledUniform);
        stateset->addUniform(mRaiseAmountUniform);

        // DON'T set chunkWorldOffset here - it's per-chunk and set in chunkmanager.cpp
        // We just need to make sure we don't override it
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

            // Get deformation texture parameters from manager
            osg::Vec2f center;
            float radius;
            manager->getDeformationTextureParams(center, radius);

            // Get current deformation parameters (terrain-specific)
            float footprintRadius, deformationDepth, footprintInterval;
            manager->getDeformationParams(footprintRadius, deformationDepth, footprintInterval);

            // Update uniforms - Use our cached uniforms and add them to stateset if missing
            // This ensures the uniforms actually reach the shader
            mDeformationEnabledUniform->set(true);
            mDeformationCenterUniform->set(center);
            mDeformationRadiusUniform->set(radius);
            mRaiseAmountUniform->set(deformationDepth);  // CRITICAL: Pass depth as raise amount to shader

            // Make sure uniforms are in the stateset
            if (!stateset->getUniform("snowDeformationEnabled"))
                stateset->addUniform(mDeformationEnabledUniform);
            if (!stateset->getUniform("snowDeformationCenter"))
                stateset->addUniform(mDeformationCenterUniform);
            if (!stateset->getUniform("snowDeformationRadius"))
                stateset->addUniform(mDeformationRadiusUniform);
            if (!stateset->getUniform("snowDeformationMap"))
                stateset->addUniform(mDeformationMapUniform);
            if (!stateset->getUniform("snowRaiseAmount"))
                stateset->addUniform(mRaiseAmountUniform);

            static int logCount = 0;
            if (logCount++ < 10)
            {
                bool enabledValue = false;
                mDeformationEnabledUniform->get(enabledValue);

                Log(Debug::Info) << "[SNOW UPDATER] Binding deformation texture at ("
                                << (int)center.x() << ", " << (int)center.y()
                                << ") radius=" << radius
                                << " raiseAmount=" << deformationDepth
                                << " textureUnit=" << mTextureUnit
                                << " ENABLED=" << (enabledValue ? "TRUE" : "FALSE")
                                << " texture=" << deformationTexture;
            }
        }
        else
        {
            // Disable deformation
            osg::Uniform* enabledUniform = stateset->getUniform("snowDeformationEnabled");
            if (enabledUniform)
            {
                enabledUniform->set(false);
            }

            static bool warned = false;
            if (!warned)
            {
                Log(Debug::Warning) << "[SNOW UPDATER] No deformation texture or disabled! "
                                   << "texture=" << (deformationTexture ? "valid" : "null")
                                   << " enabled=" << manager->isEnabled();
                warned = true;
            }
        }
    }
}
