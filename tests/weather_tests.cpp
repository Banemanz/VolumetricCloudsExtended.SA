#include "../CloudBanks.hpp"
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
    // The active volume has fixed weather endpoints; weather affects their
    // alpha/color interpolation, not generation or geometry. A forced clear
    // must select exactly the clear endpoint regardless of residual rain.
    canopy::Map map;map.build(512,s.seed);
    banks::Volume volume;volume.build(s,map);
    const auto voxelCount=volume.pixels[0].size();
    const auto endpoint=[&](std::size_t i,const weather::State& w) {
        return mix(float(volume.pixels[0][i].a),float(volume.pixels[1][i].a),w.coverage);
    };
    for(std::size_t i=0;i<voxelCount;i+=31) {
        CHECK(endpoint(i,sunny)==volume.pixels[0][i].a);
        CHECK(endpoint(i,rainy)==volume.pixels[1][i].a);
        const auto midway=weather::sample({1,8,.5f,.5f,0,0,0,0,false,false});
        CHECK(close(endpoint(i,midway),.5f*(volume.pixels[0][i].a+volume.pixels[1][i].a)));
    }
    // Density multiplier below 1 is valid, including zero in bad weather.
    Settings less=s;less.badWeatherDensityMultiplier=0;
    banks::Volume reducedVolume;reducedVolume.build(less,map);
    for(std::size_t i=0;i<voxelCount;i+=31)CHECK(reducedVolume.pixels[1][i].a==0);
    std::cout<<"PASS: native weather transitions and forced switches, residual rain, fog/dust, extrasunny, lightning, "
        "static canopy endpoint opacity and reversed density, volume endpoint interpolation and live lighting\n";
}
