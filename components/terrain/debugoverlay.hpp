#ifndef OPENMW_COMPONENTS_TERRAIN_DEBUGOVERLAY_H
#define OPENMW_COMPONENTS_TERRAIN_DEBUGOVERLAY_H

#include <osg/Camera>
#include <osg/Texture2D>
#include <osg/Geode>
#include <osg/Geometry>

namespace Terrain
{
    class DebugOverlay : public osg::Camera
    {
    public:
        DebugOverlay(int width, int height);

        void addTexture(osg::Texture2D* texture, float x, float y, float w, float h, const std::string& label);

    private:
        osg::ref_ptr<osg::Geode> mGeode;
    };
}

#endif
