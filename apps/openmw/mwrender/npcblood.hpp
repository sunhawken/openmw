#ifndef OPENMW_MWRENDER_NPCBLOOD_H
#define OPENMW_MWRENDER_NPCBLOOD_H
// NPC surface-blood implementation is separate from animation/physics policy.
namespace osg
{
    class Geometry;
}
namespace MWRender
{
    // This exact rigid wrapper preserves the original arrays and transforms.
    // Garment coverage may accept it wherever it accepts ordinary Geometry.
    bool isRigidBloodSurface(const osg::Geometry* geometry);
}
#endif
