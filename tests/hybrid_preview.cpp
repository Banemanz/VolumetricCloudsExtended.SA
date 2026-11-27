#include "../CloudVolumes.hpp"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc,char** argv) {
    if(argc!=3) return 1;
    using namespace vc;
    const std::string name=argv[1],path=argv[2];
    const bool night=name=="night",dawn=name=="sunrise",rain=name=="rain",flight=name=="flight";
    const Palette p=night?Palette{{16,25,31},{34,49,48},{},{}}:
        dawn?Palette{{126,232,245},{255,169,91},{},{}}:
        rain?Palette{{61,78,92},{134,145,146},{},{}}:
        Palette{{36,145,220},{151,220,245},{},{}};
    const Vec sun=night?Vec{0,-1,-.8f}:dawn?Vec{.82f,.55f,.13f}:Vec{-.65f,-.35f,.85f};
    Settings s;
    const float pitch=name=="zenith"?1.42f:flight?-.18f:.44f;
    const Vec f{0,std::cos(pitch),std::sin(pitch)},right{1,0,0},up=cross(right,f);
    View view{{2266,-1271,flight?680.f:30.f},right,up,f,2200,1};
    const auto r=range(s,view.farClip,1600);
    const auto light=canopy::lighting(s,p,sun,rain?1.f:0.f,rain?.75f:0.f,0.f);
    canopy::Map map;map.build(canopy::textureSize(s),s.seed);map.shade(light,0,map.size());
    volume::Atlas atlas;atlas.start(96,s.seed);while(!atlas.ready()) atlas.advance(100);
    atlas.shade(light,0,atlas.height());
    std::ofstream a(path+".canopy",std::ios::binary),b(path+".volume",std::ios::binary);
    a.write(reinterpret_cast<const char*>(map.pixels.data()),map.pixels.size()*sizeof(canopy::Pixel));
    b.write(reinterpret_cast<const char*>(atlas.pixels.data()),atlas.pixels.size()*sizeof(canopy::Pixel));
    std::cout<<p.skyTop.r<<' '<<p.skyTop.g<<' '<<p.skyTop.b<<' '<<p.skyBottom.r<<' '<<p.skyBottom.g<<' '<<p.skyBottom.b<<'\n';
    auto emit=[&](const auto& m,int layer) {
        std::cout<<layer<<' '<<m.vertices.size()<<' '<<m.indices.size()/3<<'\n';
        for(const auto& v:m.vertices) {
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<v.alpha<<'\n';
        }
        for(std::size_t i=0;i<m.indices.size();i+=3)
            std::cout<<m.indices[i]<<' '<<m.indices[i+1]<<' '<<m.indices[i+2]<<'\n';
    };
    if(name.rfind("orbit",0)==0) {
        // Same one coherent form, world positions and lighting, different eye locations.
        const float angle=std::stof(name.substr(5))*Pi/180.f;
        const volume::Group g{{0,0,0},150.f,1.f,0,0};
        const float el=-.30f;
        const Vec d{std::sin(angle)*std::cos(el),std::cos(angle)*std::cos(el),std::sin(el)};
        view.pos=d*410.f;view.forward=d*-1.f;
        view.right=unit(cross(view.forward,{0,0,1}));view.up=cross(view.right,view.forward);
        auto ws=volume::weights(view.pos-g.center);
        std::sort(ws.begin(),ws.end(),[](auto i,auto j){return i.view<j.view;});
        for(auto w:ws) if(w.amount>=.004f)emit(volume::mesh(atlas,g,w),1);
    } else {
        const auto gs=volume::groups(s,view,r,{},rain?1.f:0.f,1.f);
        emit(canopy::mesh(s,view,r,{},volume::radius(s,r),volume::canopyBlend(gs,1.f)),0);
        for(int i=0;i<gs.count;++i) {
            const auto& g=gs.items[i];
            auto ws=volume::weights(view.pos-g.center);
            std::sort(ws.begin(),ws.end(),[](auto a,auto b){return a.view<b.view;});
            for(auto w:ws) if(w.amount>=.004f)emit(volume::mesh(atlas,g,w),1);
        }
    }
}
