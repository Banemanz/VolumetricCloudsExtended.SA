// Standalone source-mesh stress test, NOT an emulation of GTA or an HD2000.
// Run on one logical CPU, optionally alongside --scene-load on that core.
#include "../CloudCanopyVolume.hpp"
#include "../CloudBillboards.hpp"
#include <chrono>
#include <iostream>
#include <string>
using namespace vc;
using Clock=std::chrono::steady_clock;
void competingSceneWork() {
    // Deterministic cache-resident scene culling + memory traversal. This
    // intentionally runs flat out as a contention stressor, not a GTA timing
    // model. The actual game has many more/different operations.
    std::vector<Vec> objects(65536);
    for(std::size_t i=0;i<objects.size();++i)
        objects[i]={float(i%257)*13.f,float((i/257)%257)*13.f,float(i%83)};
    volatile float result=0;
    for(unsigned frame=0;;++frame) {
        const Vec eye{float(frame%1000),1000.f,30.f},forward=unit(Vec{.7f,.3f,.2f});
        float visible=0;
        for(const auto& pos:objects) {
            const auto d=pos-eye;
            if(dot(d,d)<1500.f*1500.f&&dot(d,forward)>0.f)visible+=d.z;
        }
        result=visible;(void)result;
    }
}
template<class BankMesh>
void run(const Settings& s,const banks::Volume& volume,const billboards::Atlas& atlas,const canopyVolume::Relief& relief,bool tighten,bool legacy) {
    canopyVolume::Mesh canopy;banks::Mesh cards;BankMesh bankMesh;
    for(float z:{30.f,317.f,400.f,523.f,635.f,1000.f}) {
        std::vector<double> times;times.reserve(480);
        std::size_t sheet=0,volumeTris=0,cardTris=0;canopy::Drift drift;
        for(int frame=0;frame<480;++frame) {
            const float yaw=frame*.043f,pitch=(frame%120<30?1.35f:frame%120<60?.38f:frame%120<90?-.25f:0.f);
            const Vec f=unit(Vec{std::cos(yaw)*std::cos(pitch),std::sin(yaw)*std::cos(pitch),std::sin(pitch)});
            const Vec right=unit(cross(f,{0,0,1}));View v{{2239.f+frame*2.f,-1262.f-frame*.8f,z},right,cross(right,f),f,450,1};
            const float blend=canopy::handover(s,z),weather=float(frame%100)/100;
            const auto full=cloudRange(s);
            const bool directional=!legacy&&billboards::eligible(s,full);
            const auto local=directional?full:canopyVolume::volumeRange(s,full,z,blend);
            const auto lod=banks::hybridLod(s,full,z,v.pos.z);
            const auto* sharedLod=directional?&lod:nullptr;
            const bool distant=directional||billboards::eligible(s,local);
            drift.update(s,{.2f,.1f,0},.016f);
            const auto start=Clock::now();
            if(directional)canopy.indices.clear();else canopyVolume::mesh(canopy,relief,s,v,full,drift,.94f,.53f,blend,&local);
            banks::buildMesh(bankMesh,s,v,local,drift,.94f,.53f,&volume,weather,distant,tighten,true,sharedLod);
            if(distant)billboards::buildMesh(cards,s,v,local,drift,atlas,.94f,.53f,weather,sharedLod);
            else cards.indices.clear();
            times.push_back(std::chrono::duration<double,std::milli>(Clock::now()-start).count());
            sheet+=canopy.indices.size()/3;volumeTris+=bankMesh.vertices.size()/2;cardTris+=cards.indices.size()/3;
        }
        std::sort(times.begin(),times.end());
        std::cout<<(std::is_same_v<BankMesh,vc::banks::GpuMesh>?"GPU-source":"CPU-source")
            <<" z="<<z<<" median "<<times[times.size()/2]<<" ms p95 "<<times[times.size()*95/100]
            <<" ms; average canopy/volume/atlas triangles "<<sheet/480<<"/"<<volumeTris/480<<"/"<<cardTris/480<<"\n";
    }
}
int main(int argc,char** argv) {
    if(argc>1&&std::string(argv[1])=="--scene-load"){competingSceneWork();return 0;}
    Settings s;canopy::Map map;map.build(canopy::textureSize(s),s.seed);
    banks::Volume volume;volume.build(s,map);billboards::Atlas atlas;atlas.build(s,volume);
    canopyVolume::Relief relief;relief.build(s,volume);
    std::cout<<"Cloud-only source preparation, no rasterization, no D3D driver or game timing\n";
    const bool tighten=argc<=1||std::string(argv[1])!="--no-trim";
    const bool legacy=argc>1&&std::string(argv[1])=="--legacy";
    run<banks::GpuMesh>(s,volume,atlas,relief,tighten,legacy);
    run<banks::Mesh>(s,volume,atlas,relief,tighten,legacy);
}
