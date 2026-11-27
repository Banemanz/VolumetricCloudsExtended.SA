#include "../CloudBanks.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <map>
#define CHECK(x) do {if(!(x)){std::cerr<<"Line "<<__LINE__<<": "<<#x<<'\n';std::exit(1);}}while(false)
using namespace vc;
int main() {
    Settings s;
    CHECK(s.renderMode==2 && s.hybridClouds && s.cumulusDetail==0);
    canopy::Map m;m.build(512,s.seed);m.shadeWeatherLayers(s);
    banks::Volume b,other;
    const auto begin=std::chrono::steady_clock::now();
    b.build(s,m);
    const auto bake=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    other.build(s,m);
    for(int i=0;i<2;++i) {
        CHECK(b.pixels[i].size()==std::size_t(banks::Width)*banks::Width*banks::Depth);
        CHECK(std::memcmp(b.pixels[i].data(),other.pixels[i].data(),b.pixels[i].size()*sizeof(canopy::Pixel))==0);
    }
    std::uint64_t total[2]{};
    int varied=0,clear=0,wet=0;
    for(int z=0;z<b.depth;++z)for(int y=0;y<b.width;++y)for(int x=0;x<b.width;++x) {
        const auto i=b.index(x,y,z);
        const auto a=b.pixels[0][i],c=b.pixels[1][i];
        if(z==0||z==b.depth-1)CHECK(a.a==0 && c.a==0);
        total[0]+=a.a;total[1]+=c.a;
        CHECK(a.a<=c.a);
        clear+=a.a>20;wet+=c.a>20;
        if(z>0&&z<b.depth-1)varied+=std::abs(int(b.pixels[0][i].a)-int(b.pixels[0][b.index(x,y,z-1)].a))>10;
        if(a.a>0||c.a>0) {
            const float u=(x+.5f)/b.width,v=(y+.5f)/b.width,w=(z+.5f)/b.depth;
            CHECK(b.potentiallyOccupied(u,v,w,u,v,w,.5f));
        }
    }
    CHECK(total[1]>total[0] && varied>1000 && wet>clear && clear>1000);
    for(int i=0;i<300;++i) {
        const float u=i*.013f,v=i*.027f,w=i/300.f;
        const auto a=b.sample(0,u,v,w),c=b.sample(0,u+1,v-1,w);
        CHECK(std::abs(int(a.a)-int(c.a))<=1);
        // Every trilinear sample with nonzero alpha must survive the conservative occupancy grid.
        if(a.a>0)CHECK(b.potentiallyOccupied(u,v,w,u,v,w,0.f));
    }
    CHECK(b.sample(0,.5f,.5f,-3).a==0 && b.sample(1,.5f,.5f,6).a==0);
    const View ground{{2266,-1271,30},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto r=range(s,ground.farClip,1600);
    banks::Mesh raw,culled;
    for(int detail:{0,1})for(float alt:{30.f,300.f,523.f,673.f,690.f,1600.f})for(float pitch:{-.7f,0.f,.5f,1.5f})
       for(float az:{0.f,1.57f,3.14f}) {
        s.cumulusDetail=detail;auto v=ground;v.pos.z=alt;
        v.forward={std::sin(az)*std::cos(pitch),std::cos(az)*std::cos(pitch),std::sin(pitch)};
        v.right=unit(cross(v.forward,{0,0,1}));v.up=cross(v.right,v.forward);
        banks::buildMesh(raw,s,v,r,{},.94f,.53f);
        banks::buildMesh(culled,s,v,r,{},.94f,.53f,&b,0);
        CHECK(raw.indices.size()>=culled.indices.size());
        CHECK(culled.slices<=(detail?banks::DetailSlices:banks::MaxSlices) && culled.vertices.size()<(1u<<16));
        const float step=detail?banks::DetailStep:banks::EconomyStep;
        CHECK(culled.vertices.size()==std::size_t(culled.slices)*(banks::GridX+1)*(banks::GridY+1));
        for(const auto& p:culled.vertices) {
            const float phase=dot(p.pos-Vec{0,0,s.minSpawnZ},v.forward)/step-.5f;
            CHECK(std::abs(phase-std::round(phase))<.0001f);
        }
        float previous=1e9f;
        for(std::size_t i=0;i<culled.indices.size();i+=3) {
            const auto a=culled.indices[i],bb=culled.indices[i+1],c=culled.indices[i+2];
            CHECK(a<culled.vertices.size()&&bb<culled.vertices.size()&&c<culled.vertices.size());
            const auto& p=culled.vertices[a];const float d=viewDepth(v,p.pos);
            CHECK(d<=previous+.002f && d>=v.nearClip && d<=v.farClip);
            previous=d;
            const float u=p.pos.x/canopy::tileWorldSize(s),vv=p.pos.y/canopy::tileWorldSize(s);
            CHECK(std::abs(u-p.u-std::round(u-p.u))<.001f);
            CHECK(std::abs(vv-p.v-std::round(vv-p.v))<.001f);
            CHECK(std::abs(p.w-(p.pos.z-s.minSpawnZ)/(s.maxSpawnZ-s.minSpawnZ))<.001f);
            CHECK(p.alpha>=0 && p.alpha<=1 && finite(p.pos));
        }
    }
    s.cumulusDetail=0;
    auto flight=ground;flight.pos={-1639,1817,523};
    banks::buildMesh(raw,s,flight,r,{},.94f,.53f);
    banks::buildMesh(culled,s,flight,r,{},.94f,.53f,&b,0);
    CHECK(culled.indices.size()>0 && culled.indices.size()<raw.indices.size());
    const auto count=culled.indices.size()/3,unculled=raw.indices.size()/3;
    // Reproduce the reported video: z~673, travel at nearly constant
    // altitude. At fixed heading arbitrary translation and clip changes
    // must leave shared samples on exactly the same world planes.
    auto pose=ground;
    pose.pos={2164,-1265,673};
    pose.forward=unit(Vec{.96f,-.27f,-.04f});
    pose.right=unit(cross(pose.forward,{0,0,1}));pose.up=cross(pose.right,pose.forward);
    canopy::Drift wind;wind.update(s,{1.2f,-.8f,0},2.7f);
    const auto planeNumber=[&](const banks::Vertex& p) {
        const double phase=dot(p.pos-Vec{float(wind.worldX),float(wind.worldY),s.minSpawnZ},pose.forward)/banks::EconomyStep-.5;
        CHECK(std::abs(phase-std::round(phase))<.0002);
        return int(std::round(phase));
    };
    const auto centers=[&](const banks::Mesh& mesh) {
        std::map<int,banks::Vertex> result;
        for(std::size_t i=0;i<mesh.vertices.size();i+=(banks::GridX+1)*(banks::GridY+1)) {
            const auto& p=mesh.vertices[i+(banks::GridY/2)*(banks::GridX+1)+banks::GridX/2];
            result.emplace(planeNumber(p),p);
        }
        return result;
    };
    banks::buildMesh(raw,s,pose,r,wind,.94f,.53f,&b,0);
    const auto original=centers(raw);
    CHECK(original.size()>5);
    for(float travel:{.5f,3.f,39.f,41.f,94.f}) {
        auto moved=pose;moved.pos=moved.pos+pose.forward*travel;
        const auto limited=range(s,moved.farClip,1400);
        banks::buildMesh(culled,s,moved,limited,wind,.94f,.53f,&b,0);
        int shared=0;
        for(const auto& pair:centers(culled)) {
            const auto i=original.find(pair.first);
            if(i==original.end())continue;
            ++shared;
            const auto& a=i->second;const auto& q=pair.second;
            CHECK(length(a.pos-q.pos)<.002f);
            CHECK(std::abs(a.u-q.u-std::round(a.u-q.u))<.00001f);
            CHECK(std::abs(a.v-q.v-std::round(a.v-q.v))<.00001f);
            CHECK(std::abs(a.w-q.w)<.00001f);
        }
        CHECK(shared>=4);
    }
    // Across the exact horizontal movement in the recording, the plane
    // positions (world offset along heading) remain phase-aligned.
    auto moved=pose;moved.pos={2255,-1341,672.7f};
    banks::buildMesh(culled,s,moved,r,wind,.94f,.53f,&b,0);
    int shared=0;for(const auto& pair:centers(culled))shared+=original.count(pair.first)>0;
    CHECK(shared>=4);
    std::cout<<"PASS: fixed world-phase samples survive translation, altitude and clip changes; "<<shared<<" shared planes in video poses\n";

    s.hybridClouds=false;banks::buildMesh(culled,s,flight,r,{},.94f,.53f,&b,0);
    CHECK(culled.indices.empty());
    std::cout<<"PASS: periodic deterministic 3D density, clear/rain coverage, vertical borders, trilinear occupancy, world XYZ under view changes, fixed phase, depth order and slice cap; "<<bake<<" ms CPU bake; flight "<<count<<" triangles vs "<<unculled<<" unculled\n";
}
