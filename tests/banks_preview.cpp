#include "../CloudBanks.hpp"
#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    if(argc!=3)return 1;
    using namespace vc;
    const std::string name=argv[1],path=argv[2];
    const bool rain=name=="rain"||name=="rainflight",night=name=="night";
    const Palette p=night?Palette{{18,25,34},{42,50,52},{},{}}:
        rain?Palette{{61,78,92},{134,145,146},{},{}}:Palette{{36,145,220},{151,220,245},{},{}};
    const Vec sun=night?Vec{0,-1,-.8f}:Vec{-.65f,-.35f,.85f};
    Settings s;
    if(name=="canopy")s.hybridClouds=false;
    const auto climate=weather::sample({rain?8:1,rain?8:1,0,rain?1.f:0.f,0,0,0,0,false,false});
    const float pitch=name=="ground"||name=="canopy"||name=="night"||name=="rain"?.38f:
        name=="above"?-.25f:name=="zenith"?1.4f:-.04f;
    const Vec f{0,std::cos(pitch),std::sin(pitch)},right{1,0,0},up=cross(right,f);
    View view{{-1639,1817,name=="above"?690.f:name=="flight"||name=="rainflight"?523.f:name=="entry"?455.f:30.f},right,up,f,2200,1};
    if(name=="ground"||name=="canopy"||name=="rain"||name=="night"||name=="zenith")view.pos={2266,-1271,30};
    canopy::Map map;map.build(canopy::textureSize(s),s.seed);map.shadeWeatherLayers(s);
    banks::Volume volume;volume.build(s,map);
    const auto r=range(s,view.farClip,1600);
    const auto light=canopy::liveLighting(s,p,sun,climate);
    const auto save=[&](const auto& pixels,const char* suffix){
        std::ofstream file(path+suffix,std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(canopy::Pixel));
    };
    save(map.pixels,".canopy");save(map.extraPixels,".extra");save(volume.pixels[0],".clear3d");save(volume.pixels[1],".wet3d");
    std::cout<<p.skyTop.r<<' '<<p.skyTop.g<<' '<<p.skyTop.b<<' '<<p.skyBottom.r<<' '<<p.skyBottom.g<<' '<<p.skyBottom.b<<'\n';
    const auto canopyMesh=canopy::mesh(s,view,r,{},banks::radius(s,r),s.hybridClouds?1.f:0.f,true);
    const auto emit=[&](int layer,float opacity){
        std::cout<<layer<<' '<<canopyMesh.vertices.size()<<' '<<canopyMesh.indices.size()/3<<'\n';
        for(const auto& v:canopyMesh.vertices) {
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<0.f<<' '<<v.alpha*opacity
              <<' '<<light.bright.r/255.f<<' '<<light.bright.g/255.f<<' '<<light.bright.b/255.f<<'\n';
        }
        for(std::size_t i=0;i<canopyMesh.indices.size();i+=3)std::cout<<canopyMesh.indices[i]<<' '<<canopyMesh.indices[i+1]<<' '<<canopyMesh.indices[i+2]<<'\n';
    };
    auto lw=canopy::layerWeights(s,climate);
    if(lw.base>1.f/255.f)emit(0,lw.base);
    if(lw.extra>1.f/255.f)emit(1,lw.extra);
    if(s.hybridClouds){
        banks::Mesh mesh;banks::buildMesh(mesh,s,view,r,{},480.f/510.f,270.f/510.f,&volume,climate.coverage);
        std::cout<<2<<' '<<mesh.vertices.size()<<' '<<mesh.indices.size()/3<<' '<<climate.coverage<<'\n';
        for(const auto& v:mesh.vertices){
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<v.w<<' '<<v.alpha*lw.base
              <<' '<<light.bright.r/255.f<<' '<<light.bright.g/255.f<<' '<<light.bright.b/255.f<<'\n';
        }
        for(std::size_t i=0;i<mesh.indices.size();i+=3)std::cout<<mesh.indices[i]<<' '<<mesh.indices[i+1]<<' '<<mesh.indices[i+2]<<'\n';
    }
}
