#ifndef OPENMW_COMPONENTS_TERRAIN_SNOWPARTICLEEMITTER_HPP
#define OPENMW_COMPONENTS_TERRAIN_SNOWPARTICLEEMITTER_HPP

#include <osg/Group>
#include <osg/ref_ptr>
#include <osgParticle/ParticleSystem>
#include <osgParticle/ModularEmitter>
#include <osgParticle/Program>

#include <map>
#include <string>

namespace Resource {
    class SceneManager;
}

namespace Terrain
{
    class SnowParticleEmitter
    {
    public:
        SnowParticleEmitter(osg::Group* parentNode, Resource::SceneManager* sceneManager);
        ~SnowParticleEmitter();

        void emit(const osg::Vec3f& position, const std::string& terrainType);
        void update(float dt);

    private:
        void createParticleSystem(const std::string& textureName);
        osg::ref_ptr<osgParticle::ModularEmitter> createEmitter(const osg::Vec3f& position, const std::string& type);

        osg::Group* mParentNode;
        Resource::SceneManager* mSceneManager;
        
        osg::ref_ptr<osgParticle::ParticleSystem> mParticleSystem;
        osg::ref_ptr<osgParticle::Program> mParticleProgram;
        osg::ref_ptr<osg::Group> mParticleGroup;

        // Configuration for different terrain types
        struct ParticleConfig {
            std::string texture;
            osg::Vec4 color;
            float size;
            float lifeTime;
            float speed;
            int count;
        };

        std::map<std::string, ParticleConfig> mConfigs;
    };
}

#endif
