#include "debugoverlay.hpp"
#include <osg/Depth>
#include <osg/Texture2D>

namespace Terrain
{
    DebugOverlay::DebugOverlay(int width, int height)
    {
        setProjectionMatrixAsOrtho2D(0, width, 0, height);
        setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        setViewMatrix(osg::Matrix::identity());
        setClearMask(0); // Don't clear, just draw on top
        setRenderOrder(osg::Camera::POST_RENDER, 10000); // Draw last
        setAllowEventFocus(false);

        mGeode = new osg::Geode;
        osg::StateSet* ss = mGeode->getOrCreateStateSet();
        ss->setMode(GL_LIGHTING, osg::StateAttribute::OFF);
        ss->setMode(GL_DEPTH_TEST, osg::StateAttribute::OFF);
        
        addChild(mGeode);
    }

    void DebugOverlay::addTexture(osg::Texture2D* texture, float x, float y, float w, float h, const std::string& label)
    {
        if (!texture) return;

        osg::Geometry* geom = new osg::Geometry;
        
        osg::Vec3Array* verts = new osg::Vec3Array;
        verts->push_back(osg::Vec3(x, y, 0));
        verts->push_back(osg::Vec3(x + w, y, 0));
        verts->push_back(osg::Vec3(x + w, y + h, 0));
        verts->push_back(osg::Vec3(x, y + h, 0));
        geom->setVertexArray(verts);

        osg::Vec2Array* texcoords = new osg::Vec2Array;
        texcoords->push_back(osg::Vec2(0, 0));
        texcoords->push_back(osg::Vec2(1, 0));
        texcoords->push_back(osg::Vec2(1, 1));
        texcoords->push_back(osg::Vec2(0, 1));
        geom->setTexCoordArray(0, texcoords);

        geom->addPrimitiveSet(new osg::DrawArrays(osg::PrimitiveSet::QUADS, 0, 4));

        osg::StateSet* ss = geom->getOrCreateStateSet();
        ss->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);

        mGeode->addDrawable(geom);
    }
}
