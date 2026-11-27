#pragma once

// Original, shader-free cumulus impostors. All CPU work is portable.
// The one-time atlas bake projects overlapping optically dense sub-lobes,
// then vertex lighting supplies continuously changing sun/view direction.
#include "CloudCore.hpp"

namespace vc::cumulus {
constexpr int Tile=128, AtlasSize=Tile*2, Variants=4;
constexpr int MaxGrid=5, MaxVertices=MaxGrid*MaxGrid, MaxIndices=(MaxGrid-1)*(MaxGrid-1)*6;
struct Pixel { std::uint8_t r,g,b,a; };
static_assert(sizeof(Pixel)==4);
using Atlas=std::vector<Pixel>;
struct Lobe { Vec center; float radius; };

inline std::uint8_t byte(float v) { return static_cast<std::uint8_t>(clamp(v,0.f,255.f)+.5f); }

// Continuous, inexpensive hash noise. No game RNG and no third-party assets.
inline float hash(int x,int y,std::uint32_t seed) {
    std::uint32_t h=std::uint32_t(x)*0x9e3779b9u+std::uint32_t(y)*0x85ebca6bu+seed;
    h^=h>>16; h*=0x7feb352du; h^=h>>15; h*=0x846ca68bu; h^=h>>16;
    return float(h>>8)*(1.f/16777216.f);
}
inline float noise(float x,float y,std::uint32_t seed) {
    const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const float u=smooth(x-ix),v=smooth(y-iy);
    return mix(mix(hash(ix,iy,seed),hash(ix+1,iy,seed),u),
               mix(hash(ix,iy+1,seed),hash(ix+1,iy+1,seed),u),v);
}
inline Atlas bakeAtlas() {
    Atlas pixels(AtlasSize*AtlasSize,{255,255,255,0});
    for(int tile=0;tile<Variants;++tile) {
        RNG rng{std::uint32_t(47017+913*tile)};
        std::array<Lobe,22> lobes{};
        // A large core, irregular surrounding lobes, then small embedded lobes.
        // All projections remain inside the transparent tile border.
        lobes[0]={{0,0,-.12f},.64f};
        for(int i=1;i<10;++i) {
            const float angle=(float(i-1)+rng.sample(-.3f,.3f))*2.f*Pi/9.f;
            const float distance=rng.sample(.33f,.53f);
            lobes[i]={{std::cos(angle)*distance,std::sin(angle)*distance,rng.sample(-.22f,.25f)},rng.sample(.27f,.40f)};
        }
        for(int i=10;i<int(lobes.size());++i) {
            const float angle=rng.sample(0.f,2.f*Pi),distance=rng.sample(.10f,.58f);
            lobes[i]={{std::cos(angle)*distance,std::sin(angle)*distance,rng.sample(.20f,.49f)},rng.sample(.15f,.28f)};
        }
        // Back-to-front composition of sub-lobes; RGB stays straight-alpha.
        std::sort(lobes.begin(),lobes.end(),[](const Lobe& a,const Lobe& b){return a.center.z<b.center.z;});
        const auto seed=std::uint32_t(111+tile*771);
        for(int y=1;y<Tile-1;++y) for(int x=1;x<Tile-1;++x) {
            const float u=2.f*(float(x)+.5f)/Tile-1.f,v=1.f-2.f*(float(y)+.5f)/Tile;
            const float n=(noise(u*7.f,v*7.f,seed)-.5f)*.12f+
                          (noise(u*19.f,v*19.f,seed+71)-.5f)*.055f+
                          (noise(u*43.f,v*43.f,seed+19)-.5f)*.020f;
            float coverage=0.f, premul=0.f;
            for(const auto& l:lobes) {
                const float dx=(u-l.center.x)/l.radius,dy=(v-l.center.y)/l.radius;
                const float w=1.f-dx*dx-dy*dy+n/l.radius;
                if(w<=0.f) continue;
                const float z=std::sqrt(std::min(w,1.f));
                // Optical thickness, plus smooth erosion at the silhouette.
                const float alpha=(1.f-std::exp(-3.4f*z))*smooth(w/.17f);
                // Low-contrast micro-relief, not a baked strong light direction.
                // Large-scale illumination and silhouette rim are dynamic.
                const float light=clamp(.88f+.12f*dy+.05f*z+.18f*n,.72f,1.f);
                premul=light*alpha+premul*(1.f-alpha);
                coverage=alpha+coverage*(1.f-alpha);
            }
            const float border=smooth((1.f-std::max(std::abs(u),std::abs(v)))*13.f);
            const auto a=byte(coverage*border*255.f);
            const auto c=coverage>1e-5f?byte(premul/coverage*255.f):std::uint8_t(255);
            pixels[(tile/2*Tile+y)*AtlasSize+tile%2*Tile+x]={c,c,c,a};
        }
    }
    return pixels;
}
struct UV { float u,v; };
inline UV atlasUV(std::uint32_t key,float u,float v) {
    const int tile=int(key%Variants);
    // Half-texel inset keeps fixed-function linear filtering within each tile.
    return {(tile%2*Tile+.5f+sat(u)*(Tile-1))/AtlasSize,
            (tile/2*Tile+.5f+sat(v)*(Tile-1))/AtlasSize};
}

// Stable eye-facing silhouettes with world-vertical tops and flattened bases.
// No frame-to-frame random numbers; puffs remain attached to the advecting group.
inline int cards(const Settings& s,const Cloud& c,const View& v,float alpha,Card* out) {
    const int count=s.puffsPerCloud;
    const float opacity=1.f-std::pow(1.f-sat(alpha),std::min(1.f,4.f/count));
    // Horizontal camera right is stable during roll. Reconstruct vertical
    // relative to view, with the camera basis as the zenith fallback.
    Vec right=unit(Vec{v.forward.y,-v.forward.x,0.f});
    if(length(right)<.5f) right=v.right;
    Vec up=unit(Vec{right.y*v.forward.z-right.z*v.forward.y,
                    right.z*v.forward.x-right.x*v.forward.z,
                    right.x*v.forward.y-right.y*v.forward.x});
    if(length(up)<.5f) up=v.up;
    const float width=std::max(c.size.x,c.size.y),height=c.size.z;
    // Broad horizontal body + elevated irregular towers. Scale by world Z
    // rather than camera angle so fly-through parallax stays three-dimensional.
    for(int i=0;i<count;++i) {
        const auto& p=c.puffs[i];
        const float phase=float(i)*2.39996323f+float(c.textureKey%1024)*.006135923f;
        const float tier=i==0?0.f:(i%3==0?.23f:.05f);
        const Vec offset=i==0?Vec{0,0,-height*.10f}:
            Vec{c.size.x*.24f*std::cos(phase),c.size.y*.24f*std::sin(phase),
                height*tier+p.offset.z*.22f};
        const float w=i==0?width*.83f:width*mix(.42f,.62f,sat(p.width/width));
        const float h=i==0?height*.88f:height*mix(.86f,1.20f,sat(p.height/height));
        const Vec center=c.position+offset;
        // Fade each puff as the camera flies through it, rather than only
        // the group center. Avoids a solid card abruptly crossing the lens.
        const float proximity=s.nearFadeDistance>0.f
            ? smooth(length(center-v.pos)/std::max(s.nearFadeDistance,.23f*std::max(w,h))) : 1.f;
        out[i]={center,right*(w*.5f),up*(h*.5f),
            opacity*proximity,i==0?.28f:sat(.55f+tier),c.textureKey+std::uint32_t(i)*3u};
    }
    return count;
}

struct LitVertex { Vec pos; UV uv; RGB colour; };
struct Lighting {
    Vec sun; float neutral,contrast,rim,day;
    Lighting(const Settings& s,const Palette& p,Vec direction)
        : sun(finite(direction)?unit(direction):Vec{0,0,1}),
          neutral(nativeCloudColour(s,p).r),
          contrast(s.cumulusContrast),rim(s.cumulusRimStrength),
          day(smooth((sun.z+.09f)/.27f)) {
        if(length(sun)<.5f) {sun={0,0,1};day=1.f;}
    }

    struct Basis { Vec right,up,toward; float back,body; };
    Basis basis(const Card& card,const View& view) const {
        const Vec toward=unit(view.pos-card.center);
        return {unit(card.right),unit(card.up),toward,
            smooth((dot(toward,sun)*-1.f+.10f)/1.10f)*day,sat(card.shade)};
    }
    RGB shade(const Basis& b,float x,float y) const {
        const auto& right=b.right;
        const auto& up=b.up;
        const auto& toward=b.toward;
        const float radius=std::sqrt(x*x+y*y);
        // Orthographic hemisphere in billboard-local coordinates.
        const float z=std::sqrt(std::max(0.f,1.f-radius*radius));
        const Vec normal=unit(right*x+up*y+toward*z);
        const float direct=sat(.5f+.5f*dot(normal,sun));
        const float side=sat(.5f+.5f*dot(right*x+up*y,sun));
        const float back=b.back;
        const float edge=smooth((radius-.32f)/.55f);
        const float height=sat(.5f+.5f*dot(normal,Vec{0,0,1}));
        // Soft indirect light keeps daytime shadows cloudy-white, not soot.
        // Backlight primarily brightens thin edges, leaving a shaded interior.
        const float diffuse=.67f+.19f*direct+.10f*height+.04f*b.body;
        const float lighting=mix(1.f,diffuse,contrast*day);
        const float darken=.29f*contrast*back*(1.f-edge);
        const float silver=rim*back*edge*(.26f+.13f*side);
        const float lum=clamp(neutral*(lighting-darken+silver),0.f,255.f);
        // Only a restrained cool daylight shadow; never the low-cloud tint.
        const float cool=day*contrast*(1.f-direct)*(1.f-edge)*.065f;
        return {lum*(1.f-cool),lum*(1.f-cool*.45f),lum};
    }
};

inline int grid(const Settings& s) { return s.cumulusDetail?5:3; }
inline int mesh(const Card& card,const View& view,const Lighting& lighting,int n,LitVertex* out) {
    const auto basis=lighting.basis(card,view);
    for(int iy=0;iy<n;++iy) for(int ix=0;ix<n;++ix) {
        const float u=float(ix)/(n-1),v=float(iy)/(n-1),x=2.f*u-1.f,y=1.f-2.f*v;
        out[iy*n+ix]={card.center+card.right*x+card.up*y,atlasUV(card.textureKey,u,v),lighting.shade(basis,x,y)};
    }
    return n*n;
}
inline int indices(int n,int base,std::uint16_t* out) {
    int count=0;
    for(int y=0;y<n-1;++y) for(int x=0;x<n-1;++x) {
        const int a=base+y*n+x,b=a+1,c=a+n,d=c+1;
        for(int i:{a,b,c,b,d,c}) out[count++]=std::uint16_t(i);
    }
    return count;
}
} // namespace vc::cumulus
