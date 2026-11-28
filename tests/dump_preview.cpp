// Offline diagnostic: emits the actual core's card geometry for a synthetic
// software preview. It does not embed or load any GTA assets.
#include "../CloudCore.hpp"
#include <iostream>
#include <string>
#include <iomanip>
using namespace vc;

int main(int argc,char** argv) {
    const std::string scene=argc>1?argv[1]:"clear";
    Settings cfg; cfg.renderMode=1;
    float pitch=0.14f;
    float height=20.f;
    if(scene=="crossed") cfg.renderMode=0;
    if(scene=="flight") { height=520.f; pitch=-.13f; }
    Palette palette{{60,128,197},{192,218,236},{75,75,75},{50,50,50}};
    if(scene=="rain") palette={{61,80,99},{145,155,165},{65,65,65},{45,45,45}};
    if(scene=="sunrise" || scene=="sunrise_old") {
        palette={{156,255,255},{255,170,90},{60,30,20},{40,20,10}};
        height=306.f; pitch=.06f;
    }
    if(scene=="sunrise_old") {
        // Reproduce the previous build, not a separate geometry/texture fixture.
        cfg.lightingMode=1; cfg.timecycTintStrength=1.f; cfg.bottomShade=.75f; cfg.hazeStrength=.35f;
    }
    const CloudLighting lighting(cfg,palette);
    View v{{0,0,height},{1,0,0},{0,-std::sin(pitch),std::cos(pitch)},
        {0,std::cos(pitch),std::sin(pitch)},2000,1};
    const auto r=range(cfg,v.farClip,1600);
    Field f; f.reset(cfg);
    f.update(cfg,v,r,target(cfg,scene=="rain"?1.f:0.f),{},1);
    std::vector<Card> items;
    for (auto& c:f.clouds) {
        if(!c.used) continue;
        Card out[MaxCards];
        const auto a=cloudAlpha(cfg,c,v,r);
        if(a<.004f) continue;
        const int count=cards(cfg,c,v,a,out);
        for(int i=0;i<count;++i)if(out[i].alpha>=1.f/255.f)items.push_back(out[i]);
    }
    std::stable_sort(items.begin(),items.end(),[&](auto& a,auto& b){return viewDepth(v,a.center)>viewDepth(v,b.center);});
    // Output projected corners and alpha/shading. Cards crossing the near plane
    // are omitted from this diagnostic, whereas RW clips them in the game.
    auto project=[&](Vec p,float& x,float& y) {
        p=p-v.pos; const auto z=dot(p,v.forward);
        if(z<1.f)return false;
        x=480.f+510.f*dot(p,v.right)/z; y=270.f-510.f*dot(p,v.up)/z;
        return true;
    };
    std::cout << std::setprecision(8);
    auto rgb=[](RGB c) { std::cout<<c.r<<','<<c.g<<','<<c.b; };
    rgb(palette.skyTop); std::cout<<','; rgb(palette.skyBottom); std::cout<<'\n';
    for(auto& c:items) {
        float x[4],y[4];
        bool valid=true;
        for(int i=0;i<4;++i) {
            const auto p=c.center+c.right*((i&1)?1.f:-1.f)+c.up*((i&2)?-1.f:1.f);
            if(!project(p,x[i],y[i]))valid=false;
        }
        if(!valid)continue;
        for(int i=0;i<4;++i) std::cout<<x[i]<<','<<y[i]<<',';
        const auto colours=lighting.colours(c.shade,length(c.center-v.pos)/r.end);
        std::cout<<c.alpha<<',';
        rgb(colours.top); std::cout<<','; rgb(colours.bottom); std::cout<<'\n';
    }
}
