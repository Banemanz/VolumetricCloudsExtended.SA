#include "../Canopy.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#define CHECK(x) do { if (!(x)) { std::cerr<<"Line "<<__LINE__<<": "<<#x<<'\n'; std::exit(1); } } while(false)
using namespace vc;
using namespace vc::canopy;
bool close(float a,float b,float eps=.0001f) { return std::abs(a-b)<=eps; }

int main() {
    Settings s;
    CHECK(s.renderMode==2 && textureSize(s)==512);
    std::ostringstream ini;write(ini,s);
    CHECK(ini.str().find("RenderMode=2\n")!=std::string::npos);
    CHECK(ini.str().find("CumulusImpostors=")==std::string::npos && ini.str().find("CanopyFlightClouds=")==std::string::npos);
    CHECK(ini.str().find("UseGameFog=")==std::string::npos && ini.str().find("CloudFogStart=")!=std::string::npos);
    CHECK(ini.str().find("[Atmosphere]")!=std::string::npos);
    for(int mode=0;mode<=2;++mode) {
        Settings old;std::istringstream in("RenderMode="+std::to_string(mode)+"\nCumulusDetail=1\nCumulusImpostors=0\nCanopyFlightClouds=99\n");
        load(in,old);CHECK(old.renderMode==mode && textureSize(old)==1024);
    }
    for(int i=0;i<257;++i) {
        const float v=float(i)/257.f;
        CHECK(close(shape(0,v,s.seed),shape(1,v,s.seed),.0001f));
        CHECK(close(shape(v,0,s.seed),shape(v,1,s.seed),.0001f));
        CHECK(close(shape(-.01f,v,s.seed),shape(.99f,v,s.seed),.0001f));
    }
    const Palette clear{{36,145,220},{151,220,245},{},{}};
    const Palette dark{{16,25,31},{34,49,48},{},{}};
    const auto day=lighting(s,clear,{.65f,-.35f,.85f},0,0,0);
    const auto night=lighting(s,dark,{0,-1,-.8f},0,0,0);
    auto rain=lighting(s,clear,{.65f,-.35f,.85f},1,.8f,0);
    CHECK(day.bright.r>220 && night.bright.r<60 && rain.bright.r<day.bright.r);
    CHECK(day.threshold>rain.threshold);
    Settings less=s;less.badWeatherDensityMultiplier=.5f;
    CHECK(threshold(less,1)>threshold(less,0));
    Map map,repeat;
    const auto start=std::chrono::steady_clock::now();
    map.build(EconomySize,s.seed);map.shade(day,0,map.size());
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    repeat.build(EconomySize,s.seed);repeat.shade(day,0,repeat.size());
    CHECK(std::memcmp(map.pixels.data(),repeat.pixels.data(),map.pixels.size()*sizeof(Pixel))==0);
    const auto coverage=[](const Map& m) {
        std::size_t covered=0;double brightness=0;int low=255,high=0;
        for(const auto& p:m.pixels) if(p.a>96) { ++covered;brightness+=p.r;low=std::min(low,int(p.r));high=std::max(high,int(p.r)); }
        return std::array<double,4>{double(covered)/m.pixels.size(),brightness/std::max(std::size_t(1),covered),double(low),double(high)};
    };
    const auto c=coverage(map);
    CHECK(c[0]>.18 && c[0]<.8 && c[1]>175 && c[3]-c[2]>15);
    map.shade(rain,0,map.size());const auto r=coverage(map);
    CHECK(r[0]>c[0]+.15 && r[1]<c[1]);
    map.shade(night,0,map.size());const auto n=coverage(map);
    CHECK(close(float(n[0]),float(c[0])) && n[1]<60);
    const auto opposite=lighting(s,clear,{-.65f,.35f,.85f},0,0,0);
    map.shade(opposite,0,map.size());int changed=0;
    for(std::size_t i=0;i<map.pixels.size();++i) {
        CHECK(map.pixels[i].a==repeat.pixels[i].a);
        if(map.pixels[i].a>96 && std::abs(int(map.pixels[i].r)-int(repeat.pixels[i].r))>4) ++changed;
    }
    CHECK(changed>1000);
    const View v{{2266,-1271,30},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto rangeLimit=range(s,v.farClip,1600);
    const auto mesh0=mesh(s,v,rangeLimit,{});
    CHECK(VertexCount==257 && IndexCount==1440);
    for(auto i:mesh0.indices) CHECK(i<VertexCount);
    for(int z=0;z<1100;z+=50) {
        auto v2=v;v2.pos.z=float(z);v2.forward={0,0,1};v2.right={-1,0,0};v2.up={0,1,0};
        const auto m=mesh(s,v2,rangeLimit,{});
        const auto rotated=mesh(s,View{v2.pos,v.right,v.up,v.forward,v.farClip,v.nearClip},rangeLimit,{});
        for(int i=0;i<VertexCount;++i) {
            const auto& a=mesh0.vertices[i];const auto& b=m.vertices[i];
            CHECK(finite(b.pos) && length(a.pos-b.pos)==0 && a.u==b.u && a.v==b.v);
            CHECK(b.alpha>=0 && b.alpha<=1 && b.alpha==rotated.vertices[i].alpha);
            if(b.alpha>0) CHECK(length(b.pos-v2.pos)<rangeLimit.end+.001f);
        }
    }
    for(int i=VertexCount-Segments;i<VertexCount;++i) CHECK(mesh0.vertices[i].alpha==0);
    auto inside=v;inside.pos.z=altitude(s);
    CHECK(mesh(s,inside,rangeLimit,{}).vertices[0].alpha==0 && handover(s,inside.pos.z)==1);
    CHECK(handover(s,v.pos.z)==0);
    auto moved=v;moved.pos.x+=500;moved.pos.y-=800;
    const float h=altitude(s),blendStart=h-std::max(1.f,std::abs(h)*(1.f-HandoverRatio));
    CHECK(handover(s,blendStart-1)==0 && handover(s,blendStart)==0 && handover(s,h)==1 && handover(s,h+300)==1);
    CHECK(std::abs(handover(s,(blendStart+h)*.5f)-.5f)<.0001f);
    for(float offset:{100.f,500.f,2000.f}) {
        auto elevated=s;elevated.minSpawnZ+=offset;elevated.maxSpawnZ+=offset;
        const float canopyZ=altitude(elevated);
        CHECK(handover(elevated,canopyZ*HandoverRatio-.01f)==0.f);
        CHECK(std::abs(handover(elevated,canopyZ*(1.f+HandoverRatio)*.5f)-.5f)<.0001f);
        CHECK(handover(elevated,canopyZ)==1.f);
    }
    auto hybridSettings=s;hybridSettings.hybridClouds=true;
    const auto low=mesh(hybridSettings,v,rangeLimit,{},0.f);
    const auto upperMesh=mesh(hybridSettings,v,rangeLimit,{},1.f);
    for(std::size_t i=0;i<low.vertices.size();++i) {
        CHECK(low.vertices[i].pos.z==upperMesh.vertices[i].pos.z);
        CHECK(low.vertices[i].alpha>=upperMesh.vertices[i].alpha);
    }
    const auto m=mesh(s,moved,rangeLimit,{});
    for(int i=0;i<VertexCount;++i) {
        const float expectedU=(m.vertices[i].pos.x-mesh0.vertices[i].pos.x)/tileWorldSize(s);
        CHECK(close(fract(m.vertices[i].u-mesh0.vertices[i].u-expectedU+.5f),.5f));
    }
    Drift a,b;
    for(int i=0;i<50;++i)a.update(s,{1,-2,0},1);
    for(int i=0;i<100;++i)b.update(s,{1,-2,0},.5f);
    CHECK(std::abs(a.x-b.x)<1e-9 && std::abs(a.y-b.y)<1e-9);
    const auto prior=a;a.update(s,{1,2,0},0);CHECK(a.x==prior.x && a.y==prior.y);
    s.targetProcClouds=0;
    for(auto& vtx:mesh(s,v,rangeLimit,{}).vertices)CHECK(vtx.alpha==0);
    CHECK(textureSize(s)==EconomySize);
    Map high;high.build(DetailSize,s.seed);high.shade(day,0,high.size());
    CHECK(high.pixels.size()==std::size_t(DetailSize)*DetailSize);
    CHECK(std::abs(coverage(high)[0]-c[0])<.03);
    std::cout<<"PASS: default mode, INI compatibility, periodic density, weather, day/night lighting, sun direction, "
        "world-fixed geometry, clip/fade and wind; clear coverage "<<c[0]<<", rainy "<<r[0]<<", initial 512 bake "<<ms<<" ms\n";
}
