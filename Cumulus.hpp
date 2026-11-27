#pragma once

// Original shader-free clouds: three fixed world-space density projections.
// Both the one-time atlas bake and the per-vertex light are portable CPU code.
#include "CloudCore.hpp"

namespace vc::cumulus {
constexpr int Tile=128, AtlasColumns=4, AtlasSize=Tile*AtlasColumns;
constexpr int Variants=4, Projections=3, Tiles=Variants*Projections;
constexpr int MaxGrid=3, MaxVertices=MaxGrid*MaxGrid, MaxIndices=(MaxGrid-1)*(MaxGrid-1)*6;
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
// All three textures are projections of the same deterministic lobe field.
// XZ, YZ, XY respectively: the vertical profiles share the world's Z axis.
inline Vec project(Vec p,int plane) {
    if(plane==0)return {p.x,p.z,p.y};
    if(plane==1)return {p.y,p.z,p.x};
    return p;
}
inline Atlas bakeAtlas() {
    Atlas pixels(AtlasSize*AtlasSize,{255,255,255,0});
    for(int variant=0;variant<Variants;++variant) {
        RNG rng{std::uint32_t(42119+variant*10739)};
        std::array<Lobe,24> lobes{};
        lobes[0]={{0,0,-.12f},.61f};
        // Broad, irregular body and a few higher billows. No regular sphere
        // outlines or baked sun-side highlight. Density tapers at the edge.
        for(int i=1;i<=8;++i) {
            const float angle=(i-1+rng.sample(-.4f,.4f))*2.f*Pi/8;
            lobes[i]={{std::cos(angle)*rng.sample(.32f,.50f),
                       std::sin(angle)*rng.sample(.29f,.49f),
                       rng.sample(-.30f,.10f)},rng.sample(.27f,.43f)};
        }
        for(int i=9;i<int(lobes.size());++i) {
            const float angle=rng.sample(0.f,2.f*Pi),d=rng.sample(.10f,.51f);
            lobes[i]={{std::cos(angle)*d,std::sin(angle)*d,rng.sample(.07f,.49f)},
                      rng.sample(.15f,.30f)};
        }
        for(int plane=0;plane<Projections;++plane) {
            const int tile=variant*Projections+plane;
            const auto seed=std::uint32_t(7607+variant*907+plane*3391);
            std::array<Lobe,24> projected=lobes;
            for(auto& l:projected)l.center=project(l.center,plane);
            for(int y=1;y<Tile-1;++y) for(int x=1;x<Tile-1;++x) {
                const float u=2.f*(x+.5f)/Tile-1.f,v=1.f-2.f*(y+.5f)/Tile;
                const float warpX=(noise(u*5.f,v*5.f,seed+1)-.5f)*.22f;
                const float warpY=(noise(u*5.f,v*5.f,seed+2)-.5f)*.22f;
                const float erosion=(noise(u*8.f,v*8.f,seed)-.5f)*.30f+
                    (noise(u*21.f,v*21.f,seed+11)-.5f)*.13f+
                    (noise(u*40.f,v*40.f,seed+19)-.5f)*.06f;
                float tau=0.f;
                for(const auto& l:projected) {
                    const float dx=(u+warpX-l.center.x)/l.radius,dy=(v+warpY-l.center.y)/l.radius;
                    const float w=std::max(0.f,1.f-dx*dx-dy*dy+erosion);
                    // Integrated soft radial density; not opaque sphere sprites.
                    tau+=1.35f*w*std::sqrt(w);
                }
                const float broad=noise(u*5.f,v*5.f,seed+147);
                const float fine=noise(u*17.f,v*17.f,seed+31);
                tau*=mix(.55f,1.20f,.65f*broad+.35f*fine);
                const float border=smooth((1.f-std::max(std::abs(u),std::abs(v)))*7.f);
                const float alpha=(1.f-std::exp(-tau))*border;
                // A low-contrast neutral density texture leaves illumination
                // to the game's timecycle, with no luminous white edge band.
                const float grey=clamp(.98f-.065f*smooth(tau*.4f)+.07f*(broad-.5f)+.035f*(fine-.5f),.88f,1.f);
                const auto c=byte(grey*255.f);
                pixels[(tile/AtlasColumns*Tile+y)*AtlasSize+tile%AtlasColumns*Tile+x]={c,c,c,byte(alpha*255.f)};
            }
        }
    }
    return pixels;
}

struct UV { float u,v; };
inline UV atlasUV(std::uint32_t key,float u,float v) {
    const int tile=int(key%Tiles);
    // Half-texel inset: bilinear filtering never reads another projection.
    return {(tile%AtlasColumns*Tile+.5f+sat(u)*(Tile-1))/AtlasSize,
            (tile/AtlasColumns*Tile+.5f+sat(v)*(Tile-1))/AtlasSize};
}

inline float opticalPortion(float alpha,float weight) {
    if(weight<=0.f)return 0.f;
    if(weight>=1.f)return sat(alpha);
    return 1.f-std::pow(1.f-std::min(sat(alpha),.9995f),weight);
}

// Like SA's native intersecting-card renderer, NONE of the geometry follows
// the camera. Only the three optical weights respond to observer position.
// Each cloud has a deterministic yaw with a true horizontal underside.
inline int cards(const Settings&,const Cloud& c,const View& v,float alpha,Card* out) {
    const float angle=float((c.textureKey>>8)&1023u)*(2.f*Pi/1024.f);
    const float co=std::cos(angle),si=std::sin(angle);
    const Vec x{co,si,0},y{-si,co,0},z{0,0,1};
    const Vec rx=x*(c.size.x*.5f),ry=y*(c.size.y*.5f),rz=z*(c.size.z*.5f);
    const auto direction=unit(v.pos-c.position);
    const float dx=dot(x,direction),dy=dot(y,direction),dz=direction.z;
    // Squared direction cosines sum to one. Preserve total optical depth
    // at the common center (different silhouettes still approximate a volume).
    const float sum=dx*dx+dy*dy+dz*dz;
    const float wx=sum>1e-5f?dx*dx/sum:1.f/3.f;
    const float wy=sum>1e-5f?dy*dy/sum:1.f/3.f;
    const float wz=sum>1e-5f?dz*dz/sum:1.f/3.f;
    const auto key=(c.textureKey%Variants)*Projections;
    out[0]={c.position,rx,rz,opticalPortion(alpha,wy),.5f,key};
    out[1]={c.position,ry,rz,opticalPortion(alpha,wx),.5f,key+1};
    out[2]={c.position,rx,ry,opticalPortion(alpha,wz),.5f,key+2};
    return Projections;
}

inline float moonVisibility(int hour,int minute,float rain,float fog) {
    // GTA's lunar sprite is near midnight. Approximate its visibility window
    // smoothly (20:20 to 03:40), attenuate by weather; no phase simulation.
    const float time=float(std::clamp(hour,0,23)*60+std::clamp(minute,0,59));
    const float distance=std::min(time,1440.f-time);
    return smooth((220.f-distance)/120.f)*(1.f-.84f*std::max(sat(rain),sat(fog)));
}

struct LitVertex { Vec pos; UV uv; RGB colour; };
struct Lighting {
    Vec sun; float neutral,contrast,rim,day,nightLight,moon; RGB nightSky;
    Lighting(const Settings& s,const Palette& p,Vec direction,float moonIntensity=0.f)
        : sun(finite(direction)?unit(direction):Vec{0,0,1}),
          neutral(nativeCloudColour(s,p).r),
          contrast(s.cumulusContrast),rim(s.cumulusRimStrength),
          day(smooth((sun.z+.09f)/.27f)),nightLight(s.cumulusNightLight),moon(sat(moonIntensity)),
          nightSky(mixRGB(p.skyTop,p.skyBottom,.25f)) {
        if(length(sun)<.5f) {sun={0,0,1};day=1.f;}
    }
    struct Basis { Vec right,up; };
    // Fixed world light gradients, no camera-facing normals or rotating rim.
    Basis basis(const Card& card,const View&) const {
        return {unit(card.right),unit(card.up)};
    }
    RGB shade(const Basis& b,float x,float y) const {
        const Vec offset=b.right*x+b.up*y;
        const float height=sat(.5f+.5f*offset.z);
        const float direct=sat(.5f+.5f*dot(offset,sun));
        // Modest deep-body shadow and a sunward softening instead of an
        // additive silver outline. Even maximum rim cannot exceed neutral.
        const float diffuse=.83f+.10f*direct+.07f*height;
        const float light=mix(1.f,diffuse,contrast);
        const float edge=smooth((length(offset)-.40f)/.60f);
        const float softened=mix(light,1.f,rim*.25f*edge*direct);
        const float lum=clamp(neutral*softened,0.f,255.f);
        const float cool=contrast*(1.f-direct)*.025f;
        const RGB daylight{lum*(1.f-cool),lum*(1.f-cool*.45f),lum};

        const float mean=(nightSky.r+nightSky.g+nightSky.b)/3.f;
        const float ambient=clamp((mean*.95f+23.f)*nightLight,0.f,255.f);
        const float lunar=moon*sat(.5f+.5f*dot(offset,unit(Vec{0,-100,15})));
        const float relief=mix(.92f,1.06f,height)+.09f*lunar;
        const RGB n{clamp(ambient*relief+(nightSky.r-mean)*.14f*nightLight,0.f,255.f),
                    clamp(ambient*relief+(nightSky.g-mean)*.14f*nightLight,0.f,255.f),
                    clamp(ambient*relief+(nightSky.b-mean)*.14f*nightLight,0.f,255.f)};
        return mixRGB(n,daylight,day);
    }
};

inline int grid(const Settings& s) { return s.cumulusDetail?3:2; }
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
