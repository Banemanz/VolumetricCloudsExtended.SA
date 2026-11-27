#pragma once
// Original fixed-function cloud sheet. CPU-generated periodic density and lighting;
// the GPU sees a single ordinary RGBA texture on a shallow curved world-XY mesh.
#include "CloudCore.hpp"
#include <cassert>

namespace vc::canopy {
constexpr int Segments=32, Rings=8, VertexCount=1+Segments*Rings;
constexpr int IndexCount=Segments*(1+2*(Rings-1))*3;
constexpr int EconomySize=512, DetailSize=1024;
struct Pixel { std::uint8_t r,g,b,a; };
static_assert(sizeof(Pixel)==4);
inline int textureSize(const Settings& s) { return s.cumulusDetail?DetailSize:EconomySize; }
inline float fract(float x) { return x-std::floor(x); }
inline int wrap(int x,int period) { x%=period;return x<0?x+period:x; }
inline std::uint32_t hash(int x,int y,std::uint32_t seed) {
    std::uint32_t n=std::uint32_t(x)*0x8da6b343u^std::uint32_t(y)*0xd8163841u^seed;
    n^=n>>16;n*=0x7feb352du;n^=n>>15;n*=0x846ca68bu;n^=n>>16;
    return n;
}
inline float random(int x,int y,int period,std::uint32_t seed) {
    return float(hash(wrap(x,period),wrap(y,period),seed)>>8)*(1.f/16777216.f);
}
inline float noise(float u,float v,int period,std::uint32_t seed) {
    const float x=u*period,y=v*period;
    const int i=int(std::floor(x)),j=int(std::floor(y));
    const float a=smooth(x-i),b=smooth(y-j);
    return mix(mix(random(i,j,period,seed),random(i+1,j,period,seed),a),
        mix(random(i,j+1,period,seed),random(i+1,j+1,period,seed),a),b);
}
// Cellular billows, with irregular centers. All octaves wrap on the same tile.
inline float billows(float u,float v,int period,std::uint32_t seed) {
    const float x=u*period,y=v*period;
    const int i=int(std::floor(x)),j=int(std::floor(y));
    float closest=9.f;
    for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
        const int a=i+dx,b=j+dy;
        const float xx=a+.15f+.7f*random(a,b,period,seed)-x;
        const float yy=b+.15f+.7f*random(a,b,period,seed^0x9e3779b9u)-y;
        closest=std::min(closest,xx*xx+yy*yy);
    }
    return 1.f-sat(std::sqrt(closest));
}
inline float shape(float u,float v,std::uint32_t seed) {
    const float x=u+.09f*(noise(u,v,5,seed+91)-.5f);
    const float y=v+.09f*(noise(u,v,5,seed+92)-.5f);
    const float macro=.58f*noise(x,y,7,seed+1)+.42f*noise(x,y,13,seed+2);
    const float medium=.62f*billows(x,y,22,seed+3)+.38f*billows(x,y,38,seed+4);
    const float detail=.50f*billows(x,y,55,seed+5)+.35f*noise(x,y,96,seed+6)+.15f*noise(x,y,185,seed+7);
    // The broad correlated field controls gaps; smaller billows erode and
    // subdivide the silhouette instead of producing isolated round particles.
    return sat(.56f*macro+.30f*medium+.14f*detail);
}
inline float tileWorldSize(const Settings& s) { return 2200.f*s.sizeScale; }
inline float altitude(const Settings& s) { return mix(s.minSpawnZ,s.maxSpawnZ,.52f); }
inline float threshold(const Settings& s,float weather) {
    const float count=sat(float(target(s,weather))/320.f);
    const float storm=sat(weather)*sat((s.badWeatherDensityMultiplier-1.f)*1.7f);
    return clamp(.628f-.17f*count-.13f*storm,.26f,.74f);
}
struct Light {
    float threshold{},day{},storm{},contrast{},rim{},density{};
    Vec sun{};
    RGB bright{},ambient{};
};
inline Light lighting(const Settings& s,const Palette& p,Vec sun,float weather,float rain,float fog) {
    if(!finite(sun)) sun={0,1,0};
    sun=unit(sun);
    const float day=smooth((sun.z+.12f)/.36f);
    const float storm=sat(std::max(rain,weather*.60f));
    // Day clouds scatter nearly white light, not the saturated sky color.
    // Night follows the actual local sky without the vanilla +64 floor.
    const RGB sky=mixRGB(p.skyTop,p.skyBottom,.52f);
    const float skyGrey=(sky.r+sky.g+sky.b)/3.f;
    const float native=nativeCloudColour(s,p).r;
    const float warm=day*(1.f-smooth((sun.z-.03f)/.42f))*(1.f-storm);
    const float nightGrey=clamp((skyGrey*.82f+9.f)*s.cumulusNightLight,0.f,95.f);
    const float white=mix(native,248.f,.72f)*(1.f-.20f*storm-.08f*sat(fog));
    const RGB nightColour=mixRGB({nightGrey,nightGrey,nightGrey},scaleRGB(sky,s.cumulusNightLight),.25f);
    const RGB sunColour{white,white*(1.f-.045f*warm),white*(1.f-.12f*warm)};
    const RGB bright=mixRGB(nightColour,sunColour,day);
    const RGB ambient=mixRGB(scaleRGB(nightColour,.72f),
        scaleRGB(sunColour,.72f-.16f*storm),day);
    return {threshold(s,weather),day,storm,s.cumulusContrast,
        s.cumulusRimStrength,s.cumulusDensity*s.opacity*smooth(float(target(s,weather))/30.f),sun,bright,ambient};
}
inline bool different(const Light& a,const Light& b) {
    return std::abs(a.density-b.density)>.025f || std::abs(a.threshold-b.threshold)>.009f || length(a.sun-b.sun)>.08f ||
        std::abs(a.storm-b.storm)>.04f || std::abs(a.day-b.day)>.035f ||
        std::abs(a.bright.r-b.bright.r)>4.f || std::abs(a.bright.g-b.bright.g)>4.f ||
        std::abs(a.bright.b-b.bright.b)>4.f;
}
inline std::uint8_t byte(float x) { return std::uint8_t(clamp(x,0.f,255.f)+.5f); }

class Map {
    int n=0;
    std::vector<float> field;
    float get(int x,int y) const { return field[std::size_t(y&(n-1))*n+(x&(n-1))]; }
public:
    std::vector<Pixel> pixels;
    int size() const { return n; }
    void build(int size,std::uint32_t seed) {
        assert(size==EconomySize || size==DetailSize);
        n=size;
        field.resize(std::size_t(n)*n);
        pixels.resize(field.size());
        // One deterministic bake per RW lifetime, no game RNG or external assets.
        for(int y=0;y<n;++y) for(int x=0;x<n;++x)
            field[std::size_t(y)*n+x]=shape((x+.5f)/n,(y+.5f)/n,seed);
    }
    void shade(const Light& l,int first,int last) {
        const float horiz=std::sqrt(horizontalSq(l.sun));
        const Vec dir=horiz>.001f?l.sun/horiz:Vec{0,1,0};
        const int step=std::max(1,n/256);
        // Two directional occlusion taps along the sun plus local relief.
        // Integer offsets selected once per bake; no per-texel exp/trig or rays.
        const int dx=int(std::round(dir.x*3*step)),dy=int(std::round(dir.y*3*step));
        const int ex=int(std::round(dir.x*10*step)),ey=int(std::round(dir.y*10*step));
        for(int y=first;y<last;++y) for(int x=0;x<n;++x) {
            const float f=get(x,y), t=f-l.threshold;
            const float body=smooth(t/.14f);
            const float alpha=smooth(t/.060f)*(.80f+.20f*body)*l.density;
            const float slope=(get(x+dx,y+dy)-get(x-dx,y-dy))*7.5f;
            const float shadow=sat((get(x+ex,y+ey)-f)*8.f+.38f*body);
            const float fine=(f-.25f*(get(x-step,y)+get(x+step,y)+get(x,y-step)+get(x,y+step)))*21.f;
            const float rim=smooth(t/.018f)*(1.f-smooth(t/.075f))*l.rim;
            const float relief=sat(.65f+l.contrast*(slope*.62f-shadow*.50f+fine*.45f-.22f*body)
                +.15f*rim*l.day);
            const RGB c=mixRGB(l.ambient,l.bright,relief);
            pixels[std::size_t(y)*n+x]={byte(c.r),byte(c.g),byte(c.b),byte(sat(alpha)*255.f)};
        }
    }
};
// Rate-limit relighting, and amortize it over multiple render frames. The
// previously completed texture remains visible until all rows are ready.
struct Refresh {
    Light shown{},pending{};
    int row=0;
    float elapsed=0.f;
    bool busy=false;
    void reset(const Light& l) { shown=pending=l;row=0;elapsed=0;busy=false; }
    bool advance(Map& map,const Light& l,float dt) {
        elapsed=std::min(50.f,elapsed+(std::isfinite(dt)?clamp(dt,0.f,5.f):0.f));
        if(!busy && dt>0.f && elapsed>=50.f && different(shown,l)) {
            pending=l;row=0;busy=true;
        }
        if(!busy) return false;
        const int last=std::min(map.size(),row+16);
        map.shade(pending,row,last);row=last;
        if(row!=map.size()) return false;
        shown=pending;busy=false;elapsed=0;return true;
    }
};
struct Drift {
    double x=0.,y=0.; // wrapped world displacement in tile units
    double worldX=0.,worldY=0.; // unwrapped displacement for fixed world cells
    void update(const Settings& s,Vec wind,float dt) {
        if(!finite(wind) || !std::isfinite(dt)) return;
        const double k=double(clamp(dt,0.f,5.f))*s.windMoveScale/tileWorldSize(s);
        worldX+=wind.x*k*tileWorldSize(s);worldY+=wind.y*k*tileWorldSize(s);
        x+=wind.x*k;y+=wind.y*k;x-=std::floor(x);y-=std::floor(y);
    }
};
struct Vertex { Vec pos;float u,v,alpha; };
struct Mesh {
    std::array<Vertex,VertexCount> vertices{};
    std::array<std::uint16_t,IndexCount> indices{};
};
inline const auto& directions() {
    static const auto d=[] {
        std::array<Vec,Segments> result{};
        for(int i=0;i<Segments;++i) {
            const float angle=2.f*Pi*i/Segments;
            result[i]={std::cos(angle),std::sin(angle),0};
        }
        return result;
    }();
    return d;
}
inline float flightWeight(const Settings& s,const View& v) {
    const float fade=std::max(70.f,s.nearFadeDistance+55.f);
    return 1.f-smooth(std::abs(v.pos.z-altitude(s))/fade);
}
inline Mesh mesh(const Settings& s,const View& v,const Range& r,const Drift& drift,float hybridRadius=0.f,float hybridFade=0.f) {
    Mesh m;
    const float base=altitude(s),radius=r.end*.96f;
    // A very shallow sag at the horizon, zero camera tilt. Recenter XY only;
    // UVs are derived from world XY and thus never rotate or slide with the view.
    const float sag=std::min((s.maxSpawnZ-s.minSpawnZ)*.55f,radius*.14f);
    const float tile=tileWorldSize(s);
    const double u=double(v.pos.x)/tile-drift.x,vv=double(v.pos.y)/tile-drift.y;
    const float originU=float(u-std::floor(u)),originV=float(vv-std::floor(vv));
    const float fade=std::max(70.f,s.nearFadeDistance+55.f);
    const float strength=s.opacity>0.f && s.targetProcClouds>0?1.f:0.f;
    const auto vertex=[&](Vec delta,float f) {
        const Vec p{v.pos.x+delta.x,v.pos.y+delta.y,base-sag*f*f};
        const float dist=length(p-v.pos);
        const float a=smooth(dist/fade)*smooth((1.f-f)/.18f)*distanceAlpha(r,dist);
        const float blend=hybridRadius>0.f?1.f-.83f*hybridFade*smooth((hybridRadius-length(delta))/(hybridRadius*.38f)):1.f;
        // At aircraft altitude a flat horizon ring is especially obvious.
        // The original canopy-only fallback remains unchanged.
        const float above=smooth((v.pos.z-base)/180.f);
        return Vertex{p,originU+delta.x/tile,originV+delta.y/tile,a*strength*blend*(1.f-above*hybridFade)};
    };
    m.vertices[0]=vertex({},0);
    for(int ring=1;ring<=Rings;++ring) {
        // Denser rings close to the eye; this also localizes the flight fade.
        const float f=std::pow(float(ring)/Rings,1.65f);
        for(int j=0;j<Segments;++j)
            m.vertices[1+(ring-1)*Segments+j]=vertex(directions()[j]*(radius*f),f);
    }
    int k=0;
    const auto index=[](int ring,int j) { return std::uint16_t(1+(ring-1)*Segments+j%Segments); };
    for(int j=0;j<Segments;++j) {
        m.indices[k++]=0;m.indices[k++]=index(1,j);m.indices[k++]=index(1,j+1);
    }
    for(int i=2;i<=Rings;++i) for(int j=0;j<Segments;++j) {
        const auto a=index(i-1,j),b=index(i-1,j+1),c=index(i,j),d=index(i,j+1);
        m.indices[k++]=a;m.indices[k++]=c;m.indices[k++]=b;
        m.indices[k++]=b;m.indices[k++]=c;m.indices[k++]=d;
    }
    return m;
}
} // namespace vc::canopy
