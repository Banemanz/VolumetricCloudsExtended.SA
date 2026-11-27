#include "../CloudBillboards.hpp"
#include <fstream>
#include <iostream>
#include <string>
int main(int argc,char** argv) {
    if(argc!=3)return 1;
    using namespace vc;
    const std::string name=argv[1],path=argv[2];
    const bool rain=name=="rain"||name=="rainflight",night=name=="night",dawn=name=="dawn"||name=="dawnmove";
    const Palette p=night?Palette{{18,25,34},{42,50,52},{},{}}:
        rain?Palette{{61,78,92},{134,145,146},{},{}}:dawn?Palette{{160,240,235},{255,174,80},{},{}}:Palette{{36,145,220},{151,220,245},{},{}};
    const Vec sun=night?Vec{0,-1,-.8f}:dawn?Vec{-.8f,-.5f,.15f}:Vec{-.65f,-.35f,.85f};
    Settings s;
    if(name=="canopy")s.hybridClouds=false;
    if(name=="short")s.cloudDrawDistance=1800;
    if(name=="long")s.cloudDrawDistance=12000;
    if(name=="nobillboard")s.farBillboards=false;
    const auto climate=weather::sample({rain?8:1,rain?8:1,0,rain?1.f:0.f,0,0,0,0,false,false});
    const float pitch=name=="ground"||name=="canopy"||name=="night"||name=="rain"?.38f:
        name=="above"?-.25f:name=="zenith"?1.4f:-.04f;
    const Vec f=dawn?unit(Vec{.96f,-.27f,-.17f}):Vec{0,std::cos(pitch),std::sin(pitch)};
    const Vec right=unit(cross(f,{0,0,1})),up=cross(right,f);
    View view{{-1639,1817,name=="above"?690.f:name=="flight"||name=="rainflight"?523.f:name=="entry"?412.f:30.f},right,up,f,2200,1};
    if(name=="ground"||name=="canopy"||name=="rain"||name=="night"||name=="zenith")view.pos={2266,-1271,30};
    if(name=="midair"||name=="long"||name=="short"||name=="nobillboard"){view.pos={2532.6f,-905.3f,name=="midair"?399.3f:499.3f};}
    if(dawn)view.pos={name=="dawn"?2164.f:2255.f,name=="dawn"?-1265.f:-1341.f,673.f};
    canopy::Map map;map.build(canopy::textureSize(s),s.seed);map.shadeWeatherLayers(s);
    banks::Volume volume;volume.build(s,map);
    const auto r=cloudRange(s);
    const auto light=canopy::liveLighting(s,p,sun,climate);
    const auto save=[&](const auto& pixels,const char* suffix){
        std::ofstream file(path+suffix,std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(canopy::Pixel));
    };
    save(map.pixels,".canopy");save(map.extraPixels,".extra");save(volume.pixels[0],".clear3d");save(volume.pixels[1],".wet3d");
    std::cout<<p.skyTop.r<<' '<<p.skyTop.g<<' '<<p.skyTop.b<<' '<<p.skyBottom.r<<' '<<p.skyBottom.g<<' '<<p.skyBottom.b<<'\n';
    const float blend=canopy::handover(s,view.pos.z);
    const auto canopyMesh=canopy::mesh(s,view,r,{},blend);
    const auto emit=[&](int layer,float opacity){
        std::cout<<layer<<' '<<canopyMesh.vertices.size()<<' '<<canopyMesh.indices.size()/3<<'\n';
        for(const auto& v:canopyMesh.vertices) {
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<0.f<<' '<<v.alpha*opacity
              <<' '<<light.bright.r/255.f<<' '<<light.bright.g/255.f<<' '<<light.bright.b/255.f<<' '<<cloudFogFactor(s,length(d))<<'\n';
        }
        for(std::size_t i=0;i<canopyMesh.indices.size();i+=3)std::cout<<canopyMesh.indices[i]<<' '<<canopyMesh.indices[i+1]<<' '<<canopyMesh.indices[i+2]<<'\n';
    };
    auto lw=canopy::layerWeights(s,climate);
    if(lw.base>1.f/255.f)emit(0,lw.base);
    if(lw.extra>1.f/255.f)emit(1,lw.extra);
    if(blend>0.f){
        const auto bankLight=banks::bankLight(light,climate);
        banks::Mesh silhouettes;
        if(billboards::eligible(s,r)) {
            billboards::Atlas atlas;atlas.build(s,volume);
            save(atlas.pixels[0],".atlas0");save(atlas.pixels[1],".atlas1");
            billboards::buildMesh(silhouettes,s,view,r,{},atlas,480.f/510.f,270.f/510.f,climate.coverage);
            std::cout<<3<<' '<<silhouettes.vertices.size()<<' '<<silhouettes.indices.size()/3<<' '<<climate.coverage<<'\n';
            for(const auto& v:silhouettes.vertices) {
                const auto d=v.pos-view.pos;
                std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<v.w<<' '<<v.alpha*weather::visibility(climate)*weather::clearOpacity(climate)*blend
                    <<' '<<bankLight.r/255.f<<' '<<bankLight.g/255.f<<' '<<bankLight.b/255.f<<' '<<v.fog<<'\n';
            }
            for(std::size_t i=0;i<silhouettes.indices.size();i+=3)std::cout<<silhouettes.indices[i]<<' '<<silhouettes.indices[i+1]<<' '<<silhouettes.indices[i+2]<<'\n';
        }
        banks::Mesh mesh;banks::buildMesh(mesh,s,view,r,{},480.f/510.f,270.f/510.f,&volume,climate.coverage,billboards::eligible(s,r));
        std::cout<<2<<' '<<mesh.vertices.size()<<' '<<mesh.indices.size()/3<<' '<<climate.coverage<<'\n';
        for(const auto& v:mesh.vertices){
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<v.w<<' '<<v.alpha*weather::visibility(climate)*weather::clearOpacity(climate)*blend*blend
              <<' '<<bankLight.r/255.f<<' '<<bankLight.g/255.f<<' '<<bankLight.b/255.f<<' '<<v.fog<<'\n';
        }
        for(std::size_t i=0;i<mesh.indices.size();i+=3)std::cout<<mesh.indices[i]<<' '<<mesh.indices[i+1]<<' '<<mesh.indices[i+2]<<'\n';
    }
}
