#include "../CloudVolumes.hpp"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc,char** argv) {
    if(argc!=3) return 1;
    using namespace vc;
    const std::string name=argv[1],path=argv[2];
    const bool night=name=="night" || name=="lightning",dawn=name=="sunrise",
        rain=name=="rain",cloudy=name=="cloudy",fog=name=="fog",dust=name=="sandstorm",extra=name=="extra";
    const Palette p=night?Palette{{16,25,31},{34,49,48},{},{}}:
        dawn?Palette{{126,232,245},{255,169,91},{},{}}:
        rain?Palette{{61,78,92},{134,145,146},{},{}}:
        cloudy?Palette{{116,148,167},{174,186,190},{},{}}:
        fog?Palette{{120,135,140},{174,180,180},{},{}}:
        dust?Palette{{153,111,79},{219,176,109},{},{}}:
        Palette{{36,145,220},{151,220,245},{},{}};
    const Vec sun=night?Vec{0,-1,-.8f}:dawn?Vec{.82f,.55f,.13f}:Vec{-.65f,-.35f,.85f};
    Settings s;
    const int type=rain || night?8:cloudy?4:fog?9:dust?19:extra?0:1;
    const auto climate=weather::sample({type,type,.5f,(rain || night || cloudy || fog || dust)?1.f:0.f,
        name=="cleared"?.95f:0.f,(fog || dust)?1.f:0.f,dust?1.f:0.f,extra?1.f:0.f,false,name=="lightning"});
    const float pitch=name=="zenith"?1.42f:name=="flight"?-.18f:.44f;
    const Vec f{0,std::cos(pitch),std::sin(pitch)},right{1,0,0},up=cross(right,f);
    View view{{2266,-1271,name=="flight"?680.f:30.f},right,up,f,2200,1};
    const auto r=range(s,view.farClip,1600);
    const auto light=canopy::liveLighting(s,p,sun,climate);
    canopy::Map map;map.build(canopy::textureSize(s),s.seed);map.shadeWeatherLayers(s);
    volume::Atlas atlas;atlas.start(96,s.seed);while(!atlas.ready()) atlas.advance(100);
    atlas.shade(canopy::relativeLight(light),0,atlas.height());
    const auto save=[&](const char* suffix,const auto& pixels) {
        std::ofstream file(path+suffix,std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(canopy::Pixel));
    };
    save(".canopy",map.pixels);save(".extra",map.extraPixels);save(".volume",atlas.pixels);
    const RGB top=climate.flash?RGB{255,255,255}:p.skyTop,low=climate.flash?RGB{255,255,255}:p.skyBottom;
    std::cout<<top.r<<' '<<top.g<<' '<<top.b<<' '<<low.r<<' '<<low.g<<' '<<low.b<<'\n';
    const auto emit=[&](const auto& m,int layer,float opacity=1.f) {
        std::cout<<layer<<' '<<m.vertices.size()<<' '<<m.indices.size()/3<<' '<<light.bright.r<<' '<<light.bright.g<<' '<<light.bright.b<<'\n';
        for(const auto& v:m.vertices) {
            const auto d=v.pos-view.pos;
            std::cout<<dot(d,view.right)<<' '<<dot(d,view.up)<<' '<<dot(d,view.forward)<<' '<<v.u<<' '<<v.v<<' '<<v.alpha*opacity<<'\n';
        }
        for(std::size_t i=0;i<m.indices.size();i+=3)
            std::cout<<m.indices[i]<<' '<<m.indices[i+1]<<' '<<m.indices[i+2]<<'\n';
    };
    if(name.rfind("orbit",0)==0) {
        const float angle=std::stof(name.substr(5))*Pi/180.f;
        const volume::Group g{{0,0,0},150.f,1.f,0,0};
        const float el=-.30f;
        const Vec d{std::sin(angle)*std::cos(el),std::cos(angle)*std::cos(el),std::sin(el)};
        view.pos=d*410.f;view.forward=d*-1.f;
        view.right=unit(cross(view.forward,{0,0,1}));view.up=cross(view.right,view.forward);
        auto ws=volume::renderWeights(view.pos-g.center,g.scale);
        std::sort(ws.begin(),ws.end(),[](auto i,auto j){return i.view<j.view;});
        for(auto w:ws) if(w.amount>=.004f)emit(volume::mesh(atlas,g,w),2);
    } else {
        if(name=="approach" || name=="entry" || name=="inside") {
            // Approach the same real occupied field cell along one flight path.
            const auto source=volume::groups(s,view,r,{},climate,1.f);
            const volume::Group* best=nullptr;
            for(int i=0;i<source.count;++i) if(source.items[i].form==0 &&
                (!best || source.items[i].occupancy>best->occupancy))best=&source.items[i];
            if(!best) return 2;
            const float offset=name=="approach"?1.75f:name=="entry"?.88f:.18f;
            view.pos=best->center+Vec{0,-1.f,-.16f}*(best->scale*offset);
            view.forward=unit(Vec{0,1.f,.08f});view.right={1,0,0};view.up=cross(view.right,view.forward);
        }
        const auto gs=volume::groups(s,view,r,{},climate,1.f);
        const auto canopyMesh=canopy::mesh(s,view,r,{},volume::radius(s,r),volume::canopyBlend(gs,1.f));
        const auto lw=canopy::layerWeights(s,climate);
        if(lw.base>1.f/255.f)emit(canopyMesh,0,lw.base);
        if(lw.extra>1.f/255.f)emit(canopyMesh,1,lw.extra);
        for(int i=0;i<gs.count;++i) {
            const auto& g=gs.items[i];
            if(g.alpha<=1.f/255.f)continue;
            auto ws=volume::renderWeights(view.pos-g.center,g.scale);
            std::sort(ws.begin(),ws.end(),[](auto a,auto b){return a.view<b.view;});
            for(auto w:ws) if(w.amount>=.004f)emit(volume::mesh(atlas,g,w),2);
        }
        const auto haze=volume::interiorHaze(s,atlas,gs,view,480.f/510.f,270.f/510.f);
        const auto color=mixRGB(light.ambient,light.bright,.57f);
        std::cout<<"H "<<color.r<<' '<<color.g<<' '<<color.b;
        for(float a:haze.alpha)std::cout<<' '<<a;
        std::cout<<'\n';
    }
}
