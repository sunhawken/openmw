#pragma once
// Minimal scene/matrix adapter for executing the real controller headlessly.
// This is a numerical harness, not an OpenMW renderer or gameplay capture.
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include <iostream>
#include <cctype>
namespace osg {
struct Vec3f {
 float v[3]{}; Vec3f()=default; Vec3f(float a,float b,float c):v{a,b,c}{}
 float& x(){return v[0];} float& y(){return v[1];} float& z(){return v[2];}
 float x()const{return v[0];} float y()const{return v[1];} float z()const{return v[2];}
 float& operator[](int i){return v[i];} float operator[](int i)const{return v[i];}
 void set(float a,float b,float c){v[0]=a;v[1]=b;v[2]=c;}
 Vec3f operator+(const Vec3f& b)const{return {x()+b.x(),y()+b.y(),z()+b.z()};}
 Vec3f operator-(const Vec3f& b)const{return {x()-b.x(),y()-b.y(),z()-b.z()};}
 Vec3f operator-()const{return {-x(),-y(),-z()};}
 Vec3f operator*(float s)const{return {x()*s,y()*s,z()*s};}
 Vec3f operator/(float s)const{return *this*(1.f/s);}
 float operator*(const Vec3f& b)const{return x()*b.x()+y()*b.y()+z()*b.z();}
 Vec3f operator^(const Vec3f& b)const{return {y()*b.z()-z()*b.y(),z()*b.x()-x()*b.z(),x()*b.y()-y()*b.x()};}
 Vec3f& operator+=(const Vec3f& b){*this=*this+b;return *this;}
 Vec3f& operator-=(const Vec3f& b){*this=*this-b;return *this;}
 Vec3f& operator*=(float s){*this=*this*s;return *this;}
 Vec3f& operator/=(float s){*this=*this/s;return *this;}
 float length2()const{return *this**this;}
 float length()const{return std::sqrt(length2());}
 float normalize(){float d=length();if(d>0)*this/=d;return d;}
};
using Vec3d=Vec3f;
struct Quat;
struct Matrix {
 double a[4][4]{}; Matrix(){for(int i=0;i<4;i++)a[i][i]=1;}
 double& operator()(int r,int c){return a[r][c];}
 double operator()(int r,int c)const{return a[r][c];}
 Vec3f getTrans()const{return {(float)a[3][0],(float)a[3][1],(float)a[3][2]};}
 void setTrans(const Vec3f& t){for(int j=0;j<3;j++)a[3][j]=t.v[j];}
 Matrix operator*(const Matrix& b)const{Matrix c;for(int i=0;i<4;i++)for(int j=0;j<4;j++){c.a[i][j]=0;for(int k=0;k<4;k++)c.a[i][j]+=a[i][k]*b.a[k][j];}return c;}
 static Matrix inverse(const Matrix& m){double e[4][8]{};for(int i=0;i<4;i++)for(int j=0;j<4;j++){e[i][j]=m.a[i][j];e[i][j+4]=(i==j);}
 for(int i=0;i<4;i++){int k=i;for(int j=i+1;j<4;j++)if(std::abs(e[j][i])>std::abs(e[k][i]))k=j;for(int j=0;j<8;j++)std::swap(e[i][j],e[k][j]);double q=e[i][i];if(std::abs(q)<1e-15)return Matrix();for(int j=0;j<8;j++)e[i][j]/=q;for(int r=0;r<4;r++)if(r!=i){q=e[r][i];for(int j=0;j<8;j++)e[r][j]-=q*e[i][j];}}
 Matrix out;for(int i=0;i<4;i++)for(int j=0;j<4;j++)out.a[i][j]=e[i][j+4];return out;}
 Quat getRotate()const; static Matrix rotate(const Quat& q);
};
inline Vec3f operator*(const Vec3f& p,const Matrix& m){Vec3f q;for(int j=0;j<3;j++)q.v[j]=(float)(p.x()*m.a[0][j]+p.y()*m.a[1][j]+p.z()*m.a[2][j]+m.a[3][j]);return q;}
struct Quat {
 double x=0,y=0,z=0,w=1;
 void makeRotate(double angle,const Vec3f& axis){double s=std::sin(angle/2);x=axis.x()*s;y=axis.y()*s;z=axis.z()*s;w=std::cos(angle/2);}
 void slerp(double t,const Quat& aa,const Quat& bb){Quat b=bb;double d=aa.x*b.x+aa.y*b.y+aa.z*b.z+aa.w*b.w;if(d<0){b.x=-b.x;b.y=-b.y;b.z=-b.z;b.w=-b.w;d=-d;}double u=1-t,v=t;if(d<0.9995){double ang=std::acos(std::clamp(d,-1.,1.));u=std::sin((1-t)*ang)/std::sin(ang);v=std::sin(t*ang)/std::sin(ang);}x=aa.x*u+b.x*v;y=aa.y*u+b.y*v;z=aa.z*u+b.z*v;w=aa.w*u+b.w*v;double n=std::sqrt(x*x+y*y+z*z+w*w);x/=n;y/=n;z/=n;w/=n;}
};
inline Quat Matrix::getRotate()const{Quat q;double tr=a[0][0]+a[1][1]+a[2][2];if(tr>0){double s=std::sqrt(tr+1)*2;q.w=s/4;q.x=(a[1][2]-a[2][1])/s;q.y=(a[2][0]-a[0][2])/s;q.z=(a[0][1]-a[1][0])/s;}else{int i=0;if(a[1][1]>a[i][i])i=1;if(a[2][2]>a[i][i])i=2;int j=(i+1)%3,k=(i+2)%3;double s=std::sqrt(1+a[i][i]-a[j][j]-a[k][k])*2;double v[3]{};v[i]=s/4;v[j]=(a[i][j]+a[j][i])/s;v[k]=(a[i][k]+a[k][i])/s;q.w=(a[j][k]-a[k][j])/s;q.x=v[0];q.y=v[1];q.z=v[2];}return q;}
inline Matrix Matrix::rotate(const Quat& q){Matrix m;double x=q.x,y=q.y,z=q.z,w=q.w;m.a[0][0]=1-2*(y*y+z*z);m.a[0][1]=2*(x*y+z*w);m.a[0][2]=2*(x*z-y*w);m.a[1][0]=2*(x*y-z*w);m.a[1][1]=1-2*(x*x+z*z);m.a[1][2]=2*(y*z+x*w);m.a[2][0]=2*(x*z+y*w);m.a[2][1]=2*(y*z-x*w);m.a[2][2]=1-2*(x*x+y*y);return m;}
template<class T> struct ref_ptr {T* p=nullptr;ref_ptr()=default;ref_ptr(T* a):p(a){};T* get()const{return p;}T* operator->()const{return p;}operator bool()const{return p!=nullptr;}ref_ptr& operator=(T* a){p=a;return *this;}};
struct Node; using NodePath=std::vector<Node*>; using NodePathList=std::vector<NodePath>;
struct Node {std::string name;Node* parent=nullptr;std::vector<Node*> children;virtual ~Node()=default;const std::string& getName()const{return name;}void setName(const std::string& s){name=s;}NodePathList getParentalNodePaths()const{NodePath p;for(Node* n=(Node*)this;n;n=n->parent)p.push_back(n);std::reverse(p.begin(),p.end());return {p};}template<class Visitor>void accept(Visitor& v){v.apply(*this);for(Node* n:children)n->accept(v);}};
struct MatrixTransform:Node {Matrix matrix;const Matrix& getMatrix()const{return matrix;}void setMatrix(const Matrix& m){matrix=m;}};
inline std::map<std::pair<Node*,size_t>,Matrix> worldCache;
inline Matrix computeLocalToWorld(const NodePath& path){auto key=std::make_pair(path.back(),path.size());auto it=worldCache.find(key);if(it!=worldCache.end())return it->second;Matrix m;for(Node* n:path)if(auto* t=dynamic_cast<MatrixTransform*>(n))m=t->matrix*m;worldCache[key]=m;return m;}
struct FrameStamp {double time=0;double getSimulationTime()const{return time;}};
struct NodeVisitor {FrameStamp fs;FrameStamp* getFrameStamp(){return &fs;}};
}
namespace SceneUtil {
template<class T,class N>struct NodeCallback{void traverse(N,osg::NodeVisitor*){}};
using NodeMap=std::map<std::string,osg::ref_ptr<osg::MatrixTransform>>;
struct NodeMapVisitor {NodeMap& map;NodeMapVisitor(NodeMap& m):map(m){}void apply(osg::Node& n){if(auto* t=dynamic_cast<osg::MatrixTransform*>(&n))map[n.name]=t;}};
}
namespace NifOsg {struct MatrixTransform:osg::MatrixTransform {
 float mScale=1;
 void setTranslation(const osg::Vec3f& t){matrix.setTrans(t);}
 void setRotation(const osg::Quat& q){auto r=osg::Matrix::rotate(q);for(int i=0;i<3;i++)for(int j=0;j<3;j++)matrix(i,j)=r(i,j)*mScale;}
};}
namespace Misc::StringUtils {inline bool ciEqual(const std::string& a,const std::string& b){if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();i++)if(std::tolower(a[i])!=std::tolower(b[i]))return false;return true;}}
namespace Debug {constexpr int Info=0;}
struct Log {Log(int){}template<class T>Log& operator<<(const T&){return *this;}};
namespace Settings {
template<class T>struct Value{T v;operator T()const{return v;}T get()const{return v;}};
struct Game {
 Value<bool> mVerletEnabled{true},mVerletUseGlobalSettings{false},mVerletBodyCollision{true},mVerletIdleWind{false};
 Value<int> mVerletPinCount{3},mVerletSubsteps{6},mVerletIterations{20};
 Value<float> mVerletGravity{711},mVerletWindStrength{0},mVerletWindFrequency{0.8},mVerletFriction{0.94},mVerletMaxStep{6},mVerletIdleDamping{0.90},mVerletBodyCollisionRadius{12},mVerletBodyCollisionMargin{1.5};
};inline Game& game(){static Game g;return g;}
}
