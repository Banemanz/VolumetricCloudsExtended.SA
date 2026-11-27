// Uses the same atlas, mesh, UVs, lighting, and field as the ASI.
#include "../Cumulus.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
using namespace vc;

int main(int argc,char** argv) {
    if(argc<3) return 2;
    const std::string scene=argv[1];
    const auto atlas=cumulus::bakeAtlas();
    std::ofstream rgba(argv[2],std::ios::binary);
    rgba.write(reinterpret_cast<const char*>(atlas.data()),atlas.size()*sizeof(cumulus::Pixel));
    if(!rgba) return 3;
    Settings cfg; cfg.renderMode=2;
    cfg.targetProcClouds=160;
    float height=400,pitch=.07f;
    Vec sun=unit(Vec{-.32f,.92f,.24f});
    Palette p{{75,148,215},{174,220,250},{50,50,50},{50,50,50}};
    if(scene=="front") sun=unit(Vec{-.55f,-.7f,.45f});
    if(scene=="flight"){height=680;pitch=-.15f;}
    if(scene=="ground"){height=30;pitch=.3f;}
    if(scene=="low")cfg.cumulusDetail=0;
    if(scene=="night"){p={{6,9,25},{16,25,40},{0,0,0},{0,0,0}};sun=unit(Vec{.4f,.5f,-.6f});}
    View v{{0,0,height},{1,0,0},{0,-std::sin(pitch),std::cos(pitch)},
        {0,std::cos(pitch),std::sin(pitch)},2500,1};
    const auto r=range(cfg,v.farClip,2000);
    Field f; f.reset(cfg); f.update(cfg,v,r,target(cfg,0),{},1);
    std::vector<Card> items;
    for(const auto& c:f.clouds) if(c.used) {
        Card out[MaxPuffs];
        const float a=cloudAlpha(cfg,c,v,r);
        if(a<1.f/255.f)continue;
        const int n=cumulus::cards(cfg,c,v,a,out);
        for(int i=0;i<n;++i)items.push_back(out[i]);
    }
    std::stable_sort(items.begin(),items.end(),[&](auto& a,auto& b){return viewDepth(v,a.center)>viewDepth(v,b.center);});
    const cumulus::Lighting lighting(cfg,p,sun);
    const int n=cumulus::grid(cfg);
    std::cout<<std::setprecision(8)<<p.skyTop.r<<','<<p.skyTop.g<<','<<p.skyTop.b<<','
             <<p.skyBottom.r<<','<<p.skyBottom.g<<','<<p.skyBottom.b<<','<<dot(sun,v.right)<<','<<dot(sun,v.forward)<<','<<dot(sun,v.up)<<'\n';
    for(const auto& c:items) {
        cumulus::LitVertex verts[cumulus::MaxVertices];
        const int count=cumulus::mesh(c,v,lighting,n,verts);
        bool valid=true;
        for(int i=0;i<count;++i) if(viewDepth(v,verts[i].pos)<1.f)valid=false;
        if(!valid)continue;
        std::cout<<n<<','<<c.alpha;
        for(int i=0;i<count;++i) {
            const auto& vert=verts[i]; const auto diff=vert.pos-v.pos;
            const float z=dot(diff,v.forward);
            std::cout<<','<<480+510*dot(diff,v.right)/z<<','<<270-510*dot(diff,v.up)/z
                     <<','<<z<<','<<vert.uv.u<<','<<vert.uv.v
                     <<','<<vert.colour.r<<','<<vert.colour.g<<','<<vert.colour.b;
        }
        std::cout<<'\n';
    }
}
