#include "../CloudBanks.hpp"
#include "../CloudBillboards.hpp"
#include <chrono>
#include <iostream>
using namespace vc;
int main(){
    Settings s;canopy::Map map;map.build(canopy::textureSize(s),s.seed);
    banks::Volume v;v.build(s,map);
#ifdef GPU
    banks::GpuMesh m;
#else
    banks::Mesh m;
#endif
    std::size_t triangles=0,verts=0;
#ifdef GPU
    std::size_t submitted=0;
#endif
    double sum=0;canopy::Drift drift;
    const auto r=cloudRange(s);
    for(int i=0;i<480;++i){
        float yaw=i*.043f; Vec f=unit(Vec{std::cos(yaw),std::sin(yaw),-.25f+.12f*std::sin(yaw*3)});
        Vec rt=unit(cross(f,{0,0,1}));View view{{2164.f+i*4,-1265.f-i*1.2f,350.f+float(i%90)*5},rt,cross(rt,f),f,450,1};
        drift.update(s,{.2f,.1f,0},.016f);
        const auto t=std::chrono::steady_clock::now();
        banks::buildMesh(m,s,view,r,drift,.94f,.53f,&v,float(i%100)/100);
#ifdef GPU
        // Same conservative homogeneous far-plane test as GpuPasses.
        // SA-like 450m scene clip; use the full projection precision guard.
        const float a=view.farClip/(view.farClip-view.nearClip),b=-view.nearClip*a;
        const float eyeDepth=-dot(view.pos,f);
        const std::array<double,4> clipW{f.x,f.y,f.z,eyeDepth},
            clipZ{a*f.x,a*f.y,a*f.z,a*eyeDepth+b};
        const float extended=std::max(view.farClip+100.f,2.f*r.end+view.farClip),
            da=extended/(extended-view.farClip),db=-da*view.farClip;
        const std::array<double,4> distantZ{da*f.x,da*f.y,da*f.z,da*eyeDepth+db};
        vc::banks::ClipPlane scenePlane{},distantPlane{};
        for(int k=0;k<4;++k) {
            scenePlane.coefficient[k]=clipW[k]-clipZ[k];
            scenePlane.absolute[k]=std::abs(clipW[k])+std::abs(clipZ[k]);
            distantPlane.coefficient[k]=distantZ[k];
            distantPlane.absolute[k]=std::abs(distantZ[k]);
        }
        submitted+=m.rangeFor(scenePlane).count()+m.rangeFor(distantPlane).count();
#endif
        sum+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
#ifdef GPU
        triangles+=m.triangleCount();
#else
        triangles+=m.indices.size()/3;
#endif
        verts+=m.vertices.size();
    }
    std::cout<<sum/480<<" ms, "<<triangles/480<<" triangles, "<<verts/480<<" vertices\n";
#ifdef GPU
    std::cout<<"Both projection passes: "<<submitted/480<<" / "<<2*verts/480
        <<" input vertices per frame ("<<100.0*(1.-double(submitted)/(2*verts))<<"% fewer)\n";
#endif
}
