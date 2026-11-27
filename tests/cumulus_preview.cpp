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
    cfg.targetProcClouds=160; cfg.cumulusDetail=1;
    float height=400,pitch=.07f;
    Vec sun=unit(Vec{-.32f,.92f,.24f});
    Palette p{{75,148,215},{174,220,250},{50,50,50},{50,50,50}};
    if(scene=="front") sun=unit(Vec{-.55f,-.7f,.45f});
    if(scene=="flight"){height=680;pitch=-.15f;}
    if(scene=="inside"){height=410;pitch=.12f;cfg.spawnRadiusMin=30;}
    if(scene=="ground"){height=30;pitch=.3f;}
    if(scene=="zenith" || scene=="night"){height=30;pitch=1.10f;cfg.spawnRadiusMin=25;cfg.spawnRadiusMax=1250;cfg.targetProcClouds=100;}
    const bool heightSequence=scene=="height_low" || scene=="height_mid" || scene=="height_high";
    if(heightSequence){height=30;pitch=.6f;cfg.spawnRadiusMin=25;cfg.spawnRadiusMax=1250;cfg.targetProcClouds=100;}
    if(scene=="low")cfg.cumulusDetail=0;
    if(scene=="night"){p={{35,48,51},{52,62,63},{0,0,0},{0,0,0}};sun=unit(Vec{.4f,.5f,-.6f});}
    View v{{0,0,height},{1,0,0},{0,-std::sin(pitch),std::cos(pitch)},
        {0,std::cos(pitch),std::sin(pitch)},2500,1};
    const auto r=range(cfg,v.farClip,2000);
    Field f; f.reset(cfg); f.update(cfg,v,r,target(cfg,0),{},1);
    // Identical field and yaw for all three height shots. Only camera Z changes.
    if(heightSequence) {
        v.pos.z=scene=="height_low"?30.f:scene=="height_mid"?210.f:390.f;
    }
    std::vector<Card> items;
    for(const auto& c:f.clouds) if(c.used) {
        Card out[MaxPuffs];
        const float a=cloudAlpha(cfg,c,v,r);
        if(a<1.f/255.f)continue;
        const int n=cumulus::cards(cfg,c,v,a,out);
        for(int i=0;i<n;++i)if(out[i].alpha>=1.f/255.f)items.push_back(out[i]);
    }
    std::stable_sort(items.begin(),items.end(),[&](auto& a,auto& b){return viewDepth(v,a.center)>viewDepth(v,b.center);});
    const cumulus::Lighting lighting(cfg,p,sun,sun.z<0.f?1.f:0.f);
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
