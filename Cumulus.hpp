#pragma once

// Original shader-free clouds: preintegrated density slabs in fixed world space.
// Both the one-time atlas bake and the per-vertex light are portable CPU code.
#include "CloudCore.hpp"

namespace vc::cumulus {
constexpr int Tile=64, HighTile=128, AtlasColumns=8, AtlasSize=Tile*AtlasColumns;
constexpr int Variants=4, Projections=3, Slabs=3, TilesPerAxis=Slabs+1;
constexpr int Tiles=Variants*Projections*TilesPerAxis;
constexpr int CardsPerCloud=Projections*TilesPerAxis;
static_assert(CardsPerCloud<=MaxCards);
constexpr int MaxGrid=3, MaxVertices=MaxGrid*MaxGrid, MaxIndices=(MaxGrid-1)*(MaxGrid-1)*6;
struct Pixel { std::uint8_t r,g,b,a; };
static_assert(sizeof(Pixel)==4);
using Atlas=std::vector<Pixel>;
struct Lobe { Vec center; Vec radius; };

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
// A small coherent 3D density field. The expensive procedural and
// self-shadowing work runs only at atlas creation, never in the frame loop.
inline float noise3(float x,float y,float z,std::uint32_t seed) {
    const int iz=int(std::floor(z));
    return mix(noise(x,y,seed+std::uint32_t(iz)*0x27d4eb2du),
               noise(x,y,seed+std::uint32_t(iz+1)*0x27d4eb2du),smooth(z-iz));
}
inline int tileKey(int variant,int axis,int slab) {
    return (variant*Projections+axis)*TilesPerAxis+slab; // 0 = full projection, 1..3 = slabs
}
inline int atlasTile(const Settings& s) {return s.cumulusDetail?HighTile:Tile;}
inline Atlas bakeAtlas(int tileSize=Tile) {
    const int res=tileSize>=HighTile?HighTile:Tile;
    const int atlasSize=res*AtlasColumns;
    const auto coordinate=[res](int i){return 2.f*(i+.5f)/res-1.f;};
    const auto voxel=[res](int x,int y,int z)->std::size_t{return (z*res+y)*res+x;};
    Atlas pixels(atlasSize*atlasSize,{255,255,255,0});
    std::vector<float> density(res*res*res),light(density.size());
    const float extinction=9.8f/res;
    for(int variant=0;variant<Variants;++variant) {
        RNG rng{std::uint32_t(42119+variant*10739)};
        std::array<Lobe,19> lobes{};
        lobes[0]={{0,0,-.22f},{.63f,.55f,.39f}};
        // Broad base and asymmetric, vertically developed shoulders.
        // Every archetype is one connected density field, not a pile of
        // baked sphere silhouettes.
        for(int i=1;i<=8;++i) {
            const float angle=(i-1+rng.sample(-.5f,.5f))*2.f*Pi/8;
            const float r=rng.sample(.24f,.41f);
            lobes[i]={{std::cos(angle)*rng.sample(.30f,.52f),
                       std::sin(angle)*rng.sample(.27f,.50f),
                       rng.sample(-.32f,.06f)},{r*1.12f,r,r*.94f}};
        }
        for(int i=9;i<int(lobes.size());++i) {
            const float angle=rng.sample(0.f,2.f*Pi),d=rng.sample(.08f,.44f);
            const float r=rng.sample(.20f,.39f);
            lobes[i]={{std::cos(angle)*d,std::sin(angle)*d,rng.sample(.08f,.54f)},
                      {r,r,r*rng.sample(.85f,1.28f)}};
        }
        // Pre-invert radii to avoid three divisions per lobe per voxel.
        for(auto& l:lobes)l.radius={1.f/l.radius.x,1.f/l.radius.y,1.f/l.radius.z};
        const auto seed=std::uint32_t(7607+variant*907);
        for(int z=0;z<res;++z)for(int y=0;y<res;++y)for(int x=0;x<res;++x) {
            const Vec p{coordinate(x),coordinate(y),coordinate(z)};
            float sum=0.f;
            for(const auto& l:lobes) {
                const Vec d=p-l.center;
                const Vec q{d.x*l.radius.x,d.y*l.radius.y,d.z*l.radius.z};
                const float w=std::max(0.f,1.f-dot(q,q));
                sum+=w*w;
            }
            if(sum<=.002f){density[voxel(x,y,z)]=0;continue;}
            const float coarse=noise3(p.x*4.8f,p.y*4.8f,p.z*4.8f,seed);
            const float fine=noise3(p.x*12.f,p.y*12.f,p.z*12.f,seed+91);
            const float grain=noise3(p.x*25.f,p.y*25.f,p.z*25.f,seed+173);
            const float micro=noise3(p.x*44.f,p.y*44.f,p.z*44.f,seed+373);
            const float n=(coarse-.5f)*.58f+(fine-.5f)*.30f+(grain-.5f)*.14f+(micro-.5f)*.045f;
            const float edge=std::min({1.f-std::abs(p.x),1.f-std::abs(p.y),1.f-std::abs(p.z)});
            const float base=smooth((p.z+.68f+n*.18f)/.23f);
            // Turbulence erodes the boundary and varies the interior. Thick
            // lobes merge smoothly; the lowest portion has a modest flat base.
            density[voxel(x,y,z)]=smooth((std::sqrt(sum)+n-.12f)/.46f)
                *mix(.40f,1.f,.45f*coarse+.42f*fine+.13f*grain)*smooth(edge*8.f)*base;
        }
        // A vertical light integral approximates diffuse self-shadowing
        // from the sky. Dynamic world-space solar tint is still vertex-lit.
        for(int y=0;y<res;++y)for(int x=0;x<res;++x){
            float above=0.f;
            for(int z=res-1;z>=0;--z) {
                const auto i=voxel(x,y,z);
                const float tau=density[i]*extinction;
                const auto at=[&](int a,int b,int c) {
                    return density[voxel(std::clamp(a,0,res-1),std::clamp(b,0,res-1),std::clamp(c,0,res-1))];
                };
                // Small-scale density relief supplies readable billows in
                // addition to the large-scale diffuse sky occlusion.
                const Vec normal=unit(Vec{at(x-1,y,z)-at(x+1,y,z),at(x,y-1,z)-at(x,y+1,z),
                                          at(x,y,z-1)-at(x,y,z+1)});
                const float relief=.5f+.5f*dot(normal,Vec{-.30f,.22f,.928f});
                light[i]=clamp(.76f+.22f*std::exp(-.84f*(above+tau*.5f))+.13f*(relief-.5f),.72f,1.f);
                above+=tau;
            }
        }
        for(int axis=0;axis<Projections;++axis) {
            std::vector<std::array<float,TilesPerAxis>> thickness(res*res), radiance(res*res);
            for(int y=1;y<res-1;++y)for(int x=1;x<res-1;++x) {
                std::array<float,TilesPerAxis> tau{},lit{};
                // XZ depth +Y, YZ depth +X, XY depth +Z.
                for(int k=0;k<res;++k) {
                    const int v=res-1-y;
                    const auto i=axis==0?voxel(x,k,v):axis==1?voxel(k,x,v):voxel(x,v,k);
                    const float t=density[i]*extinction;
                    const int slab=1+std::min(Slabs-1,k*Slabs/res);
                    tau[0]+=t;lit[0]+=t*light[i];
                    tau[slab]+=t;lit[slab]+=t*light[i];
                }
                thickness[y*res+x]=tau;radiance[y*res+x]=lit;
            }
            for(int y=1;y<res-1;++y)for(int x=1;x<res-1;++x) {
                const auto& tau=thickness[y*res+x];
                const auto& lit=radiance[y*res+x];
                for(int slab=0;slab<TilesPerAxis;++slab) {
                    const int tile=tileKey(variant,axis,slab);
                    const float u=coordinate(x),v=coordinate(res-1-y);
                    // Fine texture variation, restricted to the body, not a
                    // hard white rim. Per-projection grain does not affect alpha.
                    const float grain=(noise(u*15.f,v*15.f,seed+axis*331)-.5f)*.07f+
                                      (noise(u*30.f,v*30.f,seed+axis*331+71)-.5f)*.045f;
                    const auto sample=[&](int px,int py) {
                        return thickness[std::clamp(py,0,res-1)*res+std::clamp(px,0,res-1)][slab];
                    };
                    const int step=std::max(1,res/48);
                    const float gx=sample(x-step,y)-sample(x+step,y);
                    const float gy=sample(x,y+step)-sample(x,y-step);
                    const float relief=.085f*clamp(-.45f*gx+.8f*gy,-1.f,1.f)*smooth(tau[slab]*1.5f);
                    const auto c=tau[slab]>1e-5f?byte(clamp(lit[slab]/tau[slab]+grain+relief,.72f,1.f)*255.f):std::uint8_t(255);
                    const auto a=byte((1.f-std::exp(-tau[slab]))*255.f);
                    pixels[(tile/AtlasColumns*res+y)*atlasSize+tile%AtlasColumns*res+x]={c,c,c,a};
                }
            }
        }
    }
    return pixels;
}

struct UV { float u,v; };
inline UV atlasUV(std::uint32_t key,float u,float v) {
    const int tile=int(key%Tiles);
    // Conservative half-texel inset for the 64px atlas; also safe for 128px tiles.
    // UVs do not change if the higher resolution allocation falls back.
    return {(tile%AtlasColumns*Tile+.5f+sat(u)*(Tile-1))/AtlasSize,
            (tile/AtlasColumns*Tile+.5f+sat(v)*(Tile-1))/AtlasSize};
}

inline float opticalPortion(float alpha,float weight) {
    if(weight<=0.f)return 0.f;
    if(weight>=1.f)return sat(alpha);
    return 1.f-std::pow(1.f-std::min(sat(alpha),.9995f),weight);
}

// Normalize alpha shares against a representative dense ray. The 2D
// fixed-function alpha multiplier cannot exponentiate each texture sample;
// this calibration keeps slice and whole-projection LOD close for the core.
inline float stackAlpha(float alpha,float share,bool sliced) {
    if(share<=0.f||alpha<=0.f)return 0.f;
    constexpr float reference=.95f;
    const float target=opticalPortion(sat(alpha)*reference,share);
    if(!sliced)return target/reference;
    const float referenceSlab=1.f-std::pow(1.f-reference,1.f/Slabs);
    return sat((1.f-std::pow(1.f-target,1.f/Slabs))/referenceSlab);
}
inline int cards(const Settings& s,const Cloud& c,const View& v,float alpha,Card* out) {
    const float angle=float((c.textureKey>>8)&1023u)*(2.f*Pi/1024.f);
    const float co=std::cos(angle),si=std::sin(angle);
    const Vec x{co,si,0},y{-si,co,0},z{0,0,1};
    // Keep some stretched clouds but avoid needle-like silhouettes from
    // the independent legacy X/Y random scales. Only affects mode 2.
    const float broad=std::max(c.size.x,c.size.y);
    const float sx=mix(c.size.x,broad,.20f),sy=mix(c.size.y,broad,.20f);
    const Vec rx=x*(sx*.55f),ry=y*(sy*.55f),rz=z*(c.size.z*.66f);
    const auto eye=v.pos-c.position;
    const auto direction=unit(eye);
    const std::array<float,3> facing{
        std::abs(dot(y,direction))/std::max(sy,1.f),
        std::abs(dot(x,direction))/std::max(sx,1.f),
        std::abs(direction.z)/std::max(c.size.z*1.20f,1.f)};
    const float strongest=std::max({facing[0],facing[1],facing[2]});
    std::array<float,3> weights{};
    float total=0.f;
    for(int i=0;i<3;++i) {
        // Suppress nearly edge-on stacks; smoothly crossfade comparable
        // axes instead of switching abruptly at 45 degrees.
        weights[i]=strongest>1e-5f?smooth((facing[i]/strongest-.70f)/.30f):1.f;
        total+=weights[i];
    }
    const float angular=.55f*std::max({c.size.x,c.size.y,c.size.z})/std::max(1.f,length(eye));
    const float near=smooth((angular-(s.cumulusDetail?.10f:.16f))/(s.cumulusDetail?.13f:.16f));
    const Vec right[3]={rx,ry,rx}, up[3]={rz,rz,ry}, depth[3]={ry,rx,rz}, axisDir[3]={y,x,z};
    const int variant=int(c.textureKey%Variants);
    for(int i=0;i<3;++i) {
        const float w=weights[i]/total;
        const int base=i*TilesPerAxis;
        out[base]={c.position,right[i],up[i],stackAlpha(alpha,w*(1.f-near),false),.5f,
            std::uint32_t(tileKey(variant,i,0))};
        const float a=stackAlpha(alpha,w*near,true);
        for(int j=0;j<Slabs;++j) {
            // Sample near the voxel slab midpoint; slightly compress spacing
            // to hide gaps between the three thick slices at oblique angles.
            // Positions and UVs never follow the eye.
            const float p=(2.f*j+1.f)/Slabs-1.f;
            out[base+j+1]={c.position+depth[i]*(p*.85f),right[i],up[i],a,.5f,
                std::uint32_t(tileKey(variant,i,j+1)),axisDir[i]*p};
        }
    }
    return CardsPerCloud;
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
    struct Basis { Vec right,up,offset; };
    // Fixed world light gradients, no camera-facing normals or rotating rim.
    Basis basis(const Card& card,const View&) const {
        return {unit(card.right),unit(card.up),card.lightOffset};
    }
    RGB shade(const Basis& b,float x,float y) const {
        const Vec offset=b.offset+b.right*x+b.up*y;
        const float height=sat(.5f+.5f*offset.z);
        const float direct=sat(.5f+.5f*dot(offset,sun));
        // Modest deep-body shadow and a sunward softening instead of an
        // additive silver outline. Even maximum rim cannot exceed neutral.
        const float diffuse=.86f+.09f*direct+.05f*height;
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
