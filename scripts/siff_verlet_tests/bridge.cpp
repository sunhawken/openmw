#include "shim.hpp"
#define private public
#ifdef USE_BASELINE
#include "original/verletclothcontroller.hpp"
#else
#include "../../apps/openmw/mwrender/verletclothcontroller.hpp"
#endif
#undef private
#ifdef USE_BASELINE
#include "original/verletclothcontroller.cpp"
#else
#include "../../apps/openmw/mwrender/verletclothcontroller.cpp"
#endif
#include <memory>
struct Rig {std::vector<std::unique_ptr<osg::MatrixTransform>> nodes;std::vector<std::unique_ptr<MWRender::VerletClothController>> solvers;osg::NodeVisitor visitor;};
extern "C" {
void rig_set_motion_strength(float value){Settings::game().mVerletMovementInfluence.v=value;}
void* rig_create(int n,const int* parents,const char* const* names,const double* local,int nc,const int* starts,const int* chains,const float* settings){
 auto* r=new Rig;for(int i=0;i<n;i++){auto p=std::make_unique<NifOsg::MatrixTransform>();p->name=names[i];for(int j=0;j<16;j++)p->matrix.a[j/4][j%4]=local[i*16+j];p->mScale=std::sqrt(p->matrix(0,0)*p->matrix(0,0)+p->matrix(0,1)*p->matrix(0,1)+p->matrix(0,2)*p->matrix(0,2));r->nodes.push_back(std::move(p));}
 for(int i=0;i<n;i++)if(parents[i]>=0){r->nodes[i]->parent=r->nodes[parents[i]].get();r->nodes[parents[i]]->children.push_back(r->nodes[i].get());}
 for(int c=0;c<nc;c++){std::vector<osg::ref_ptr<osg::MatrixTransform>> chain;for(int i=starts[c];i<starts[c+1];i++)chain.push_back(r->nodes[chains[i]].get());MWRender::VerletClothSettings s;s.mEnabled=true;s.mPinCount=3;s.mSoftRootCount=(int)settings[0];s.mSoftRootStrength=settings[1];s.mFriction=settings[2];s.mGravity=711;s.mWindStrength=0;s.mIterations=32;s.mSubsteps=(int)settings[3];s.mMaxStep=6;s.mRotationCarry=settings[4];s.mLateralMemory=settings[5];s.mVelocityDeadzone=1.2;s.mContactSlop=.08;s.mCollideLegs=true;s.mGround=true;
#ifndef USE_BASELINE
 s.mStableTiming=true;s.mProjectVelocity=true;s.mInertia=settings[6];s.mInertiaMaxAcceleration=settings[7];
 s.mSleepSpeed=2.f;s.mSleepDelay=.8f;
 s.mSleepAmplitude=.18f;
 s.mRestCollisionFit=true;
 s.mAlignBones=true;
 s.mMaxLateralDeviation=settings[8];
 s.mAirDrag=settings[9];s.mAirDragMaxAcceleration=settings[10];
 s.mAirShapeResponse=settings[11];
#endif
 r->solvers.push_back(std::make_unique<MWRender::VerletClothController>(chain,s));}
 return r;
}
void rig_set_matrix(void* ptr,int node,const double* m){auto* r=(Rig*)ptr;for(int j=0;j<16;j++)r->nodes[node]->matrix.a[j/4][j%4]=m[j];}
void rig_tick(void* ptr,double time){osg::worldCache.clear();auto* r=(Rig*)ptr;r->visitor.fs.time=time;for(auto& s:r->solvers)(*s)(s->mChain.front().get(),&r->visitor);}
void rig_positions(void* ptr,float* dst){auto* r=(Rig*)ptr;for(auto& s:r->solvers)for(auto& p:s->mPositions)for(int j=0;j<3;j++)*dst++=p.v[j];}
void rig_previous(void* ptr,float* dst){auto* r=(Rig*)ptr;for(auto& s:r->solvers)for(auto& p:s->mPreviousPositions)for(int j=0;j<3;j++)*dst++=p.v[j];}
void rig_world(void* ptr,double* dst){osg::worldCache.clear();auto* r=(Rig*)ptr;for(auto& n:r->nodes){auto m=osg::computeLocalToWorld(n->getParentalNodePaths()[0]);for(int j=0;j<16;j++)*dst++=m.a[j/4][j%4];}}
void rig_impulse(void* ptr,float x,float y,float z){auto* r=(Rig*)ptr;for(auto& s:r->solvers){
#ifndef USE_BASELINE
s->mSleeping=false;s->mSleepTime=0.f;
#endif
for(size_t i=3;i<s->mPositions.size();i++)s->mPreviousPositions[i]-=osg::Vec3f(x,y,z);}}
void rig_sleep(void* ptr,int* dst){auto* r=(Rig*)ptr;for(auto& s:r->solvers){
#ifdef USE_BASELINE
*dst++=0;
#else
*dst++=s->mSleeping?1:0;
#endif
}}
void rig_destroy(void* ptr){delete (Rig*)ptr;}
}
