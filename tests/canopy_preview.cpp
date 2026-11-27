#include "../Canopy.hpp"
#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    if(argc!=3) return 1;
    using namespace vc;
    const std::string scene=argv[1];
    Settings s;s.hybridClouds=false;
    const bool night=scene=="night",dawn=scene=="sunrise",storm=scene=="rain";
    const Palette p=night?Palette{{16,25,31},{34,49,48},{},{}}:
        dawn?Palette{{126,232,245},{255,169,91},{},{}}:
        storm?Palette{{61,78,92},{134,145,146},{},{}}:
        Palette{{36,145,220},{151,220,245},{},{}};
    const Vec sun=night?Vec{0,-1,-.8f}:dawn?Vec{.82f,.55f,.13f}:Vec{.65f,-.35f,.85f};
    const float z=scene=="flight"?700.f:scene=="inside"?canopy::altitude(s):30.f;
    const float pitch=scene=="zenith"?1.42f:scene=="flight"?-.24f:scene=="inside"?.05f:.36f;
    const Vec f{0,std::cos(pitch),std::sin(pitch)},right{1,0,0};
    const Vec up=cross(right,f);
    const View view{{2266,-1271,z},right,up,f,2200,1};
    const auto r=range(s,view.farClip,1600);
    canopy::Map map;
    map.build(canopy::textureSize(s),s.seed);
    map.shade(canopy::lighting(s,p,sun,storm?1.f:0.f,storm?.8f:0.f,0.f),0,map.size());
    std::ofstream out(argv[2],std::ios::binary);
    out.write(reinterpret_cast<const char*>(map.pixels.data()),map.pixels.size()*sizeof(canopy::Pixel));
    const auto m=canopy::mesh(s,view,r,{});
    std::cout<<p.skyTop.r<<','<<p.skyTop.g<<','<<p.skyTop.b<<','<<p.skyBottom.r<<','<<p.skyBottom.g<<','<<p.skyBottom.b<<'\n';
    for(const auto& v:m.vertices) {
        const auto d=v.pos-view.pos;
        std::cout<<dot(d,right)<<','<<dot(d,up)<<','<<dot(d,f)<<','<<v.u<<','<<v.v<<','<<v.alpha<<'\n';
    }
    for(int i=0;i<canopy::IndexCount;i+=3)
        std::cout<<m.indices[i]<<','<<m.indices[i+1]<<','<<m.indices[i+2]<<'\n';
}
