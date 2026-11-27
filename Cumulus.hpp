#pragma once

// Original, shader-free cumulus impostors. All CPU work is portable.
// The one-time atlas bake projects overlapping optically dense sub-lobes,
// then vertex lighting supplies continuously changing sun/view direction.
#include "CloudCore.hpp"

namespace vc::cumulus {
constexpr int Tile=256, AtlasSize=Tile*2, Variants=4;
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
inline Atlas bakeLegacyAtlas() {
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
// Bake one integrated optical-depth image per cloud archetype. The previous
// atlas composited translucent sphere sprites; this one accumulates density
// first, then computes transmittance and a continuous visible-surface relief.
// A 2-D bake performs this once, never raymarches the screen during gameplay.
inline Atlas bakeAtlas(bool coherent=true) {
    if(!coherent) return bakeLegacyAtlas();
    Atlas pixels(AtlasSize*AtlasSize,{255,255,255,0});
    struct Sample { float tau{},surface{},relief{1.f}; };
    std::vector<Sample> field(Tile*Tile);
    for(int tile=0;tile<Variants;++tile) {
        RNG rng{std::uint32_t(42119+tile*10739)};
        std::array<Lobe,36> lobes{};
        lobes[0]={{0,-.10f,0},.62f};
        // Irregular broad base, tall shoulders and smaller embedded billows.
        for(int i=1;i<=8;++i) {
            const float angle=(i-1+rng.sample(-.32f,.32f))*2.f*Pi/8;
            lobes[i]={{std::cos(angle)*rng.sample(.36f,.53f),
                       std::sin(angle)*rng.sample(.26f,.46f)-.04f,
                       rng.sample(-.20f,.27f)},rng.sample(.26f,.43f)};
        }
        for(int i=9;i<int(lobes.size());++i) {
            const float angle=rng.sample(0.f,2.f*Pi),dist=rng.sample(.12f,.54f);
            lobes[i]={{std::cos(angle)*dist,std::sin(angle)*dist,
                        rng.sample(.08f,.39f)},rng.sample(.14f,.32f)};
        }
        const auto seed=std::uint32_t(7607+tile*907);
        for(int y=0;y<Tile;++y) for(int x=0;x<Tile;++x) {
            const float u=2.f*(x+.5f)/Tile-1.f,v=1.f-2.f*(y+.5f)/Tile;
            const float warpX=(noise(u*5.f,v*5.f,seed+1)-.5f)*.12f;
            const float warpY=(noise(u*5.f,v*5.f,seed+2)-.5f)*.12f;
            const float erosion=(noise(u*10.f,v*10.f,seed)-.5f)*.10f+
                                (noise(u*23.f,v*23.f,seed+1)-.5f)*.042f+
                                (noise(u*53.f,v*53.f,seed+7)-.5f)*.018f;
            float tau=0.f,weight=0.f,depth=0.f,lightSum=0.f;
            for(const auto& l:lobes) {
                const float dx=(u+warpX-l.center.x)/l.radius,dy=(v+warpY-l.center.y)/l.radius;
                const float w=1.f-dx*dx-dy*dy+erosion;
                if(w<=0.f)continue;
                const float chord=std::sqrt(std::min(w,1.f));
                const float amount=5.2f*w*chord;
                tau+=amount;
                // Smooth weighted visible surface: no hard sphere intersections.
                const float front=l.center.z+chord*l.radius;
                const float a=amount*std::exp(12.f*front);
                // Blend the visible lobes' shading, not their alpha. The
                // cloud's extinction remains a single integrated field.
                const float lighting=.70f+.30f*sat(-.18f*dx+.67f*dy+.68f*chord);
                weight+=a; depth+=a*front; lightSum+=lighting*a;
            }
            const float border=smooth((1.f-std::max(std::abs(u),std::abs(v)))*15.f);
            field[y*Tile+x]={tau*border,weight>1e-5f?depth/weight:0.f,weight>1e-5f?lightSum/weight:1.f};
        }
        for(int y=1;y<Tile-1;++y) for(int x=1;x<Tile-1;++x) {
            const auto at=[&](int x,int y){return field[y*Tile+x];};
            const auto p=at(x,y);
            const float a=1.f-std::exp(-p.tau);
            // Smooth gradients over several texels, plus small neutral noise.
            const int ix=std::min(Tile-1,x+2),jx=std::max(0,x-2);
            const int iy=std::min(Tile-1,y+2),jy=std::max(0,y-2);
            const float gx=(at(ix,y).surface-at(jx,y).surface)*Tile*.125f;
            const float gy=(at(x,iy).surface-at(x,jy).surface)*Tile*.125f;
            const auto normal=unit(Vec{-gx,gy,1.f});
            const float u=2.f*x/Tile-1.f,v=1.f-2.f*y/Tile;
            const float detail=(noise(u*35.f,v*35.f,seed+15)-.5f)*.035f;
            const float broad=.81f+.17f*dot(normal,unit(Vec{-.18f,.55f,.82f}));
            const float relief=mix(broad,p.relief,.64f)+
                .045f*std::exp(-p.tau*.28f)+detail;
            // Bright texture RGB even at transparent edges avoids dark halos.
            const auto c=byte(mix(1.f,clamp(relief,.72f,1.f),smooth(p.tau*.6f))*255.f);
            pixels[(tile/2*Tile+y)*AtlasSize+tile%2*Tile+x]={c,c,c,byte(a*255.f)};
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

// Billboard facing is controlled by the observer's position, never camera
// yaw/pitch/roll. Use a rotation-minimizing world basis; no frame-to-frame
// state or game RNG is consumed.
inline Vec cross(Vec a,Vec b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
inline Vec eyeDirection(Vec center,Vec eye) {
    const auto d=unit(center-eye);
    return length(d)>.5f?d:Vec{0,1,0};
}
inline void facingAxes(Vec direction,Vec& right,Vec& up) {
    // Shortest-arc transport of a world frame from +Y to the eye ray.
    // Continuous through zenith/nadir and all other directions except the
    // unavoidable antipodal horizon singularity; no camera-axis input.
    const Vec reference{0,1,0},baseRight{1,0,0};
    const float denominator=1.f+direction.y;
    if(denominator<1e-4f)right=unit(cross(direction,Vec{0,0,1}));
    else {
        const Vec axis=cross(reference,direction);
        right=unit(baseRight+cross(axis,baseRight)+cross(axis,cross(axis,baseRight))/denominator);
    }
    // Reproject to maintain exact orthogonality near the antipode.
    right=unit(right-direction*dot(right,direction));
    up=unit(cross(right,direction));
}
inline float projectedRadius(Vec size,Vec axis) {
    return .58f*length(Vec{size.x*axis.x,size.y*axis.y,size.z*axis.z});
}
inline float opticalPortion(float alpha,float weight) {
    if(weight<=0.f)return 0.f;
    if(weight>=1.f)return sat(alpha);
    // Clamp full opacity only during transitions so both representations
    // compose to the same total optical depth instead of transparent holes.
    return 1.f-std::pow(1.f-std::min(sat(alpha),.9995f),weight);
}
// Stable world-space puffs; near-cloud geometry still gives depth/parallax.
inline int layeredCards(const Settings& s,const Cloud& c,const View& v,float alpha,Card* out,int count=0,bool normalized=false) {
    if(!count)count=s.puffsPerCloud;
    const float opacity=opticalPortion(alpha,normalized?1.f/count:std::min(1.f,4.f/count));
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
        const auto direction=eyeDirection(center,v.pos);
        Vec right,up;facingAxes(direction,right,up);
        // Give each puff true horizontal depth rather than using only Z
        // height when viewed from underneath or above.
        const Vec puffSize{w,w*.88f,h};
        const float rw=projectedRadius(puffSize,right)/1.16f;
        const float rh=projectedRadius(puffSize,up)/1.16f;
        // Fade each puff as the camera flies through it, rather than only
        // the group center. Avoids a solid card abruptly crossing the lens.
        const float proximity=s.nearFadeDistance>0.f
            ? smooth(length(center-v.pos)/std::max(s.nearFadeDistance,.23f*std::max(w,h))) : 1.f;
        out[i]={center,right*rw,up*rh,
            opacity*proximity,i==0?.28f:sat(.55f+tier),c.textureKey+std::uint32_t(i)*3u};
    }
    return count;
}

// Outside a cloud, use one coherent density impostor. Project a world-space
// ellipsoid into its plane: the underside uses X/Y extent instead of Z height.
// Near its boundary, retain depth-separated puffs for flight through the body.
inline Card wholeCard(const Cloud& c,const View& v,float alpha) {
    const Vec direction=eyeDirection(c.position,v.pos);
    Vec right,up;facingAxes(direction,right,up);
    // Project all three world dimensions; retain true horizontal depth.
    const Vec size{c.size.x,c.size.y,c.size.z};
    return {c.position,right*projectedRadius(size,right),up*projectedRadius(size,up),
        alpha,.5f,c.textureKey};
}
inline int cards(const Settings& s,const Cloud& c,const View& v,float alpha,Card* out) {
    if(!s.cumulusImpostors)return layeredCards(s,c,v,alpha,out);
    const float dist=length(c.position-v.pos);
    // Blend outside-to-inside in a narrow normalized shell. One body card
    // plus at most seven puffs never exceeds the existing fixed draw buffers.
    // Use a flat high optical body opacity; only spatial fades lower it.
    // Use the actual ellipsoid intersection radius along the camera ray so broad shallow
    // clouds are not treated as "inside" when looking up from street level.
    const auto direction=eyeDirection(c.position,v.pos);
    const float depthRadius=.58f/std::max(length(Vec{direction.x/c.size.x,direction.y/c.size.y,direction.z/c.size.z}),1e-6f);
    const float bodyWeight=smooth((dist/std::max(depthRadius,1.f)-.82f)/.65f);
    const auto body=wholeCard(c,v,opticalPortion(alpha,bodyWeight));
    if(s.puffsPerCloud==1){out[0]=wholeCard(c,v,alpha);return 1;}
    if(bodyWeight>=.999f){out[0]=body;return 1;}
    const int layers=std::max(1,s.puffsPerCloud-1);
    // Body and puffs share the same atlas. Smaller layers have low alpha as
    // outside view takes over, so they cannot reveal ghosted edge stacks.
    const int count=layeredCards(s,c,v,opticalPortion(alpha,1.f-bodyWeight),out,layers,true);
    if(bodyWeight<=.001f)return count;
    out[count]=body;
    return count+1;
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
        const float diffuse=.64f+.19f*direct+.14f*height+.03f*b.body;
        const float lighting=mix(1.f,diffuse,contrast*day);
        const float darken=.29f*contrast*back*(1.f-edge);
        const float silver=rim*back*edge*(.26f+.13f*side);
        const float lum=clamp(neutral*(lighting-darken+silver),0.f,255.f);
        // Only a restrained cool daylight shadow; never the low-cloud tint.
        const float cool=day*contrast*(1.f-direct)*(1.f-edge)*.065f;
        const RGB daylight{lum*(1.f-cool),lum*(1.f-cool*.45f),lum};
        // Native +64 is useful in daylight but too luminous over a dark sky
        // when applied to a near-opaque cloud. Instead use local sky light
        // with a small diffuse floor, subdued relief and very little tint.
        const float mean=(nightSky.r+nightSky.g+nightSky.b)/3.f;
        const float ambient=clamp((mean*.95f+22.f)*nightLight,0.f,255.f);
        // Native lunar sprite lies in the world direction (0,-100,+15).
        // Weak lunar diffusion only, not a second overbright silver rim.
        const float lunar=moon*std::max(0.f,dot(normal,unit(Vec{0,-100,15})));
        const float relief=mix(.86f,1.08f,height)*mix(.94f,1.05f,b.body)+.15f*lunar;
        const RGB n{clamp(ambient*relief+(nightSky.r-mean)*.18f*nightLight,0.f,255.f),
                    clamp(ambient*relief+(nightSky.g-mean)*.18f*nightLight,0.f,255.f),
                    clamp(ambient*relief+(nightSky.b-mean)*.18f*nightLight,0.f,255.f)};
        return mixRGB(n,daylight,day);
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
