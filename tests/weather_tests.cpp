#include "../CloudVolumes.hpp"
#include <iostream>
#include <sstream>
#include <cstring>
#define CHECK(x) do {if(!(x)){std::cerr<<"Line "<<__LINE__<<": "<<#x<<'\n';std::exit(1);}}while(false)
using namespace vc;
bool close(float a,float b,float eps=.0001f){return std::abs(a-b)<=eps;}
int main() {
    Settings s;
    // SA ForceWeatherNow sets old and new ID immediately. Particle rain
    // can still be at full intensity while native cloud coverage is clear.
    const auto sunny=weather::sample({1,1,.8f,0.f,.95f,0,0,0,false,false});
    const auto rainy=weather::sample({8,8,.2f,1.f,0.f,0,0,0,false,false});
    const auto cloudy=weather::sample({4,4,.4f,1.f,0.f,0,0,0,false,false});
    const auto extra=weather::sample({0,0,0.f,0.f,0.f,0,0,1.f,false,false});
    const auto fog=weather::sample({9,9,.5f,1.f,0.f,1.f,0,0,false,false});
    const auto sand=weather::sample({19,19,.1f,1.f,0.f,1.f,0,0,false,false});
    const auto forced=weather::sample({1,1,0,0,1.f,0,0,0,true,false});
    CHECK(sunny.coverage==0 && sunny.storm==0);
    CHECK(rainy.coverage==1 && rainy.storm==1 && cloudy.storm==0);
    CHECK(extra.extraSunny==1 && weather::clearOpacity(extra)<.35f);
    CHECK(fog.fog==1 && fog.dust==0 && fog.coverage<cloudy.coverage);
    CHECK(sand.dust==1 && weather::visibility(sand)<.25f);
    CHECK(forced.coverage==1 && forced.storm==1);
    // The game provides interpolated coverage, fog, extra sun. Check both
    // directions and cloudy->rainy (same coverage, different lighting).
    for(int i=0;i<=100;++i) {
        const float t=i/100.f;
        const auto to=weather::sample({1,8,t,t,.1f,0,0,0,false,false});
        const auto from=weather::sample({8,1,t,1.f-t,.9f,0,0,0,false,false});
        const auto wet=weather::sample({4,8,t,1.f,0,0,0,0,false,false});
        CHECK(close(to.coverage,t)&&close(to.storm,t)&&close(from.coverage,1.f-t)&&close(from.storm,1.f-t));
        CHECK(wet.coverage==1 && close(wet.storm,t));
    }
    const Palette noon{{35,145,220},{151,220,245},{},{}},overcast{{70,82,99},{130,139,145},{},{}},
        dark{{15,22,32},{35,48,49},{},{}};
    const Vec sun{-.65f,-.35f,.85f};
    const auto light=canopy::liveLighting(s,noon,sun,sunny);
    const auto storm=canopy::liveLighting(s,overcast,sun,rainy);
    auto flash=rainy;flash.flash=true;
    const auto n=canopy::liveLighting(s,dark,{0,-1,-.8f},rainy);
    const auto lightning=canopy::liveLighting(s,dark,{0,-1,-.8f},flash);
    CHECK(light.bright.r>220 && storm.bright.r<light.bright.r*.8f && n.bright.r<60);
    CHECK(lightning.bright.r==255 && lightning.bright.g==255 && lightning.bright.b==255);
    const auto a=canopy::relativeLight(light),b=canopy::relativeLight(storm);
    CHECK(a.bright.r==255 && b.bright.r==255 && a.day==b.day && a.storm==b.storm && length(a.sun-b.sun)<.0001f);
    Settings reduced=s;reduced.badWeatherDensityMultiplier=.45f;
    // At full geometry opacity the optical union reproduces the actual
    // clear and overcast endpoint alpha maps, including reversed density.
    for(const auto& cfg:{s,reduced}) {
        canopy::Map masks,ref;
        masks.build(512,cfg.seed);masks.shadeWeatherLayers(cfg);
        ref.build(512,cfg.seed);
        double clearAlpha=0,rainAlpha=0;
        for(const auto& w:{sunny,rainy}) {
            const auto weights=canopy::layerWeights(cfg,w);
            ref.shade(canopy::lighting(cfg,noon,sun,w.coverage,w.storm,0),0,512);
            double sum=0;
            int maxError=0;
            for(std::size_t i=0;i<masks.pixels.size();++i) {
                const float base=masks.pixels[i].a/255.f*weights.base,ext=masks.extraPixels[i].a/255.f*weights.extra;
                const float combined=base+ext*(1.f-base);
                maxError=std::max(maxError,std::abs(int(canopy::byte(combined*255.f))-int(ref.pixels[i].a)));
                sum+=combined;
            }
            CHECK(maxError<=2);
            if(w.coverage==0)clearAlpha=sum;else rainAlpha=sum;
        }
        CHECK((cfg.badWeatherDensityMultiplier>1.f)==(rainAlpha>clearAlpha));
        // Mutating only weather never mutates pixels and coverage is monotonic.
        for(int i=0;i<101;++i) {
            auto w=weather::sample({1,8,i/100.f,i/100.f,0,0,0,0,false,false});
            auto k=canopy::layerWeights(cfg,w);
            CHECK(k.extra>=0 && k.extra<=1);
            CHECK(close(k.extra,cfg.badWeatherDensityMultiplier>=1?w.coverage:1-w.coverage));
        }
    }
    // Same normalized atlas used across weather changes. Sun-only refresh,
    // no timecycle/weather RGB bake for the storm or lightning flash.
    volume::Atlas atlas;atlas.start(64,s.seed);while(!atlas.ready())atlas.advance(100);
    atlas.shade(a,0,atlas.height());
    volume::Refresh r;r.reset(light);
    for(int i=0;i<200;++i)CHECK(!r.advance(atlas,storm,1));
    CHECK(!r.busy);
    const auto turn=canopy::liveLighting(s,noon,{.8f,.3f,.7f},sunny);
    int uploads=0;
    for(int i=0;i<100;++i)uploads+=r.advance(atlas,turn,1);
    CHECK(uploads==1 && !r.busy);
    // Large sun jump supersedes in-progress stale snapshot.
    r.reset(light);
    for(int i=0;i<35;++i)r.advance(atlas,turn,1);
    CHECK(r.busy);
    const auto opposite=canopy::liveLighting(s,noon,{0,-1,.7f},sunny);
    for(int i=0;i<80;++i)r.advance(atlas,opposite,1);
    CHECK(!r.busy && canopy::directionalDifference(r.shown,canopy::relativeLight(opposite))<.001f);
    for(int form=0;form<volume::Forms;++form) {
        CHECK(atlas.interiorDensity(form,{0,0,-.15f})>.25f);
        CHECK(atlas.interiorDensity(form,{2,0,0})==0);
    }
    volume::Groups local;local.count=1;
    local.items[0]={{0,0,0},180.f,0.f,0,0,1.f};
    View v{{0,0,-15},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto in=volume::interiorHaze(s,atlas,local,v,.9f,.65f);
    CHECK(in.maxAlpha>.4f && in.maxAlpha<=1.f);
    CHECK(std::all_of(in.alpha.begin(),in.alpha.end(),[](float a){return std::isfinite(a)&&a>=0&&a<=1;}));
    v.pos={600,0,0};
    CHECK(volume::interiorHaze(s,atlas,local,v,.9f,.65f).maxAlpha==0.f);
    v.pos={0,0,-15};auto noHaze=s;noHaze.interiorHaze=0;
    CHECK(volume::interiorHaze(noHaze,atlas,local,v,.9f,.65f).maxAlpha==0.f);
    local.items[0].occupancy=.25f;
    CHECK(volume::interiorHaze(s,atlas,local,v,.9f,.65f).maxAlpha<in.maxAlpha*.3f);
    const View eye{{2266,-1271,30},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto rangeLimit=range(s,2200,1600);
    const auto gs=volume::groups(s,eye,rangeLimit,{},sunny,1.f);
    const auto gr=volume::groups(s,eye,rangeLimit,{},rainy,1.f);
    int matched=0;
    for(int i=0;i<gs.count;++i)for(int j=0;j<gr.count;++j) {
        const auto& a=gs.items[i];const auto& b=gr.items[j];
        if(length(a.center-b.center)==0.f){CHECK(a.scale==b.scale && a.form==b.form);++matched;}
    }
    CHECK(matched>4);
    CHECK(weather::formWeight(rainy,2)>weather::formWeight(sunny,2));
    CHECK(weather::formWeight(rainy,0)<weather::formWeight(sunny,0));
    // Local group is retained when exterior proxy alpha vanishes.
    auto inside=eye;
    inside.pos=gs.items[0].center;
    const auto nearby=volume::groups(s,inside,rangeLimit,{},sunny,1.f);
    bool kept=false;
    for(int i=0;i<nearby.count;++i)if(length(nearby.items[i].center-inside.pos)==0.f) {
        kept=true;CHECK(nearby.items[i].alpha==0.f && nearby.items[i].occupancy>0.f);
    }
    CHECK(kept);
    std::cout<<"PASS: native weather transitions and forced switches, residual rain, fog/dust, extrasunny, lightning, "
        "static canopy endpoint opacity and reversed density, live RGB/no weather upload, directional supersession, "
        "density-sampled interior haze and weather-stable cloud forms. Interior alpha "<<in.maxAlpha<<"\n";
}
