#pragma once
// Original coherent density fields baked to ordinary RGBA view atlases.
// No game assets, shaders or per-frame raymarch. This header has no RW dependencies.
#include "Canopy.hpp"
#include <chrono>

namespace vc::volume {
constexpr int Azimuths=8, Views=3*Azimuths+2, Forms=3, Grid=64;
constexpr int InteriorGrid=24;
constexpr int MeshSide=5, MeshVertices=MeshSide*MeshSide, MeshIndices=6*(MeshSide-1)*(MeshSide-1);
constexpr int MaxGroups=48;
constexpr float Extent=1.18f, Domain=1.25f, Step=2.f*Domain/(Grid-1);
struct Capture { Vec dir,right,up; };
inline const auto& captures() {
    static const auto result=[] {
        std::array<Capture,Views> a{};
        for(int ring=0;ring<3;++ring) for(int j=0;j<Azimuths;++j) {
            const float az=2.f*Pi*j/Azimuths,el=(ring-1)*Pi/3.f;
            const Vec dir{std::sin(az)*std::cos(el),std::cos(az)*std::cos(el),std::sin(el)};
            const Vec right{std::cos(az),-std::sin(az),0.f};
            a[ring*Azimuths+j]={dir,right,cross(right,dir)};
        }
        a[24]={{0,0,-1},{1,0,0},{0,1,0}};
        a[25]={{0,0,1},{1,0,0},{0,-1,0}};
        return a;
    }();
    return result;
}
struct Weight { int view; float amount; };
inline std::array<Weight,3> weights(Vec d) {
    d=unit(d);
    if(length(d)<.5f) d={0,1,0};
    float x=std::atan2(d.x,d.y)*Azimuths/(2.f*Pi);
    if(x<0.f) x+=Azimuths;
    const int j=int(x)%Azimuths,k=(j+1)%Azimuths;
    const float u=x-std::floor(x),el=std::asin(clamp(d.z,-1.f,1.f))*180.f/Pi;
    std::array<Weight,3> out{};
    if(el<=-60.f || el>=60.f) {
        const int ring=el<0?0:2;
        const float t=sat((90.f-std::abs(el))/30.f);
        out={{{ring*Azimuths+j,t*(1.f-u)},{ring*Azimuths+k,t*u},{el<0?24:25,1.f-t}}};
    } else {
        const int ring=el<0?0:1,lo=ring*Azimuths,hi=lo+Azimuths;
        const float v=sat((el+60.f-ring*60.f)/60.f);
        if(u+v<=1.f) out={{{lo+j,1.f-u-v},{lo+k,u},{hi+j,v}}};
        else out={{{hi+k,u+v-1.f},{hi+j,1.f-u},{lo+k,1.f-v}}};
    }
    float sum=0.f;
    for(auto& w:out) {w.amount=smooth(w.amount)*smooth((dot(d,captures()[w.view].dir)-.50f)/.40f);sum+=w.amount;}
    for(auto& w:out) w.amount/=sum;
    return out;
}
inline float random3(int x,int y,int z,std::uint32_t seed) {
    return float(canopy::hash(x,y,seed^std::uint32_t(z)*0xa511e9b3u)>>8)*(2.f/16777216.f)-1.f;
}
inline float noise(Vec p,float freq,std::uint32_t seed) {
    p=p*freq;
    const int x=int(std::floor(p.x)),y=int(std::floor(p.y)),z=int(std::floor(p.z));
    const float fx=smooth(p.x-x),fy=smooth(p.y-y),fz=smooth(p.z-z);
    const auto layer=[&](int k) {
        return mix(mix(random3(x,y,k,seed),random3(x+1,y,k,seed),fx),
                   mix(random3(x,y+1,k,seed),random3(x+1,y+1,k,seed),fx),fy);
    };
    return mix(layer(z),layer(z+1),fz);
}
struct Lobe { Vec p,r; };
struct Voxel { float rho{}; std::array<std::uint8_t,6> light{}; };
class Density {
    std::vector<Voxel> voxels;
    std::array<Lobe,25> lobes{};
    std::uint32_t seed{};
    int slice=0,lightPlane=0;
    static int index(int x,int y,int z) { return (z*Grid+y)*Grid+x; }
public:
    void clear() {std::vector<Voxel>().swap(voxels);}
    void begin(int form,std::uint32_t baseSeed) {
        seed=baseSeed^std::uint32_t(form+1)*0x9e3779b9u;
        RNG rng{seed?seed:1u};
        const std::array<Lobe,9> base{{
            {{-.53f,.02f,-.16f},{.45f,.38f,.31f}},{{-.12f,.02f,-.19f},{.51f,.45f,.32f}},
            {{.38f,.06f,-.13f},{.49f,.42f,.32f}},{{.05f,-.22f,.16f},{.39f,.38f,.45f}},
            {{-.36f,.13f,.12f},{.32f,.38f,.36f}},{{.38f,.08f,.17f},{.35f,.28f,.43f}},
            {{.05f,.1f,.48f},{.34f,.34f,.25f}},{{.53f,.27f,.12f},{.27f,.23f,.25f}},
            {{-.65f,-.1f,.01f},{.24f,.28f,.22f}}
        }};
        for(int i=0;i<9;++i) {
            lobes[i]=base[i];
            auto& l=lobes[i];
            l.p=l.p+Vec{rng.sample(-.08f,.08f),rng.sample(-.13f,.13f),rng.sample(-.08f,.08f)};
            if(form==1) {l.p.x*=.76f;l.p.z*=1.36f;l.r.x*=.83f;l.r.z*=1.20f;}
            if(form==2) {l.p.x*=1.10f;l.p.y*=1.27f;l.p.z*=.39f;l.r.x*=1.09f;l.r.y*=1.38f;l.r.z*=.52f;}
        }
        for(int i=9;i<int(lobes.size());++i) {
            const auto& b=lobes[rng.next()%9];
            lobes[i]={b.p+Vec{rng.sample(-.19f,.19f),rng.sample(-.19f,.19f),rng.sample(-.10f,.16f)},
                {b.r.x*rng.sample(.43f,.74f),b.r.y*rng.sample(.45f,.77f),b.r.z*rng.sample(.43f,.74f)}};
        }
        voxels.assign(Grid*Grid*Grid,{});
        slice=lightPlane=0;
    }
    bool ready() const {return lightPlane==6*Grid;}
    void advanceOne() {
        if(slice<Grid) {
            const int z=slice++;
            for(int y=0;y<Grid;++y) for(int x=0;x<Grid;++x) {
                const Vec p{-Domain+x*Step,-Domain+y*Step,-Domain+z*Step};
                float sdf=-10.f;
                for(const auto& l:lobes) {
                    const Vec d=p-l.p;
                    const Vec q{d.x/l.r.x,d.y/l.r.y,d.z/l.r.z};
                    const float v=(1.f-length(q))*std::min({l.r.x,l.r.y,l.r.z});
                    const float h=std::max(.10f-std::abs(v-sdf),0.f)/.10f;
                    sdf=std::max(sdf,v)+h*h*.025f;
                }
                // Erode the coherent surface rather than rendering individual lobes.
                if(sdf<-.16f) continue;
                const float detail=.095f*noise(p,5.3f,seed+1)+.064f*noise(p,11.f,seed+2)+.030f*noise(p,20.f,seed+3);
                const float t=sat((sdf+detail+.018f)/.085f);
                // Fade the farthest wisps before the orthographic capture extent;
                // otherwise a wide form can expose the rectangular texture edge.
                const float guard=1.f-smooth((length(p)-1.00f)/.16f);
                voxels[index(x,y,z)].rho=guard*t*std::sqrt(t)*(.78f+.22f*sat(.55f+.5f*noise(p,13.f,seed+4)));
            }
            return;
        }
        if(ready()) return;
        // Cardinal optical-depth prefix sums: six O(N^3) passes, rather
        // than tracing a second light ray for every pixel of every capture.
        const int axis=lightPlane/(2*Grid),sign=(lightPlane/Grid)%2,v=lightPlane%Grid;
        ++lightPlane;
        for(int u=0;u<Grid;++u) {
            float tau=0.f;
            for(int t=0;t<Grid;++t) {
                const int k=sign?t:Grid-1-t;
                const int x=axis==0?k:u,y=axis==1?k:(axis==0?u:v),z=axis==2?k:v;
                auto& voxel=voxels[index(x,y,z)];
                const float a=voxel.rho*Step*4.2f;
                voxel.light[axis*2+sign]=canopy::byte(255.f*std::exp(-tau-.5f*a));
                tau+=a;
            }
        }
    }
    struct Sample { float rho{}; std::array<float,6> light{}; };
    Sample sample(Vec point) const {
        const Vec q=(point+Vec{Domain,Domain,Domain})/Step;
        if(q.x<0.f || q.y<0.f || q.z<0.f || q.x>=Grid-1 || q.y>=Grid-1 || q.z>=Grid-1) return {};
        const int x=int(q.x),y=int(q.y),z=int(q.z);
        const float fx=q.x-x,fy=q.y-y,fz=q.z-z;
        Sample out{};
        for(int dz=0;dz<2;++dz) for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
            const auto& voxel=voxels[index(x+dx,y+dy,z+dz)];
            const float w=(dx?fx:1.f-fx)*(dy?fy:1.f-fy)*(dz?fz:1.f-fz);
            const float r=w*voxel.rho;
            out.rho+=r;
            if(r>0.f) for(int i=0;i<6;++i) out.light[i]+=r*voxel.light[i];
        }
        if(out.rho>1e-6f) for(auto& v:out.light) v/=out.rho*255.f;
        return out;
    }
};
struct Coeff { std::uint8_t alpha{}; std::array<std::uint8_t,6> light{}; std::int8_t depth{}; };
static_assert(sizeof(Coeff)==8);
class Atlas {
    Density density;
    int tile=0,bakeRow=0,formLoaded=-1;
    std::uint32_t seed{};
    std::vector<Coeff> coeff;
    std::array<std::vector<std::uint8_t>,Forms> interior;
    int interiorForm=-1,interiorSlice=0;
    std::array<std::array<float,MeshVertices>,Forms*Views> surface{};
    // Integrate front-to-back in the true 3D density field, including self-occlusion.
    Coeff integrate(const Capture& c,float x,float y) const {
        const Vec p=c.right*x+c.up*y;
        float transmission=1.f,depth=0.f;
        std::array<float,6> light{};
        constexpr float ds=.055f;
        for(float d=1.65f;d>=-1.65f && transmission>.005f;d-=ds) {
            const auto v=density.sample(p+c.dir*d);
            if(v.rho<.001f) continue;
            const float w=transmission*(1.f-std::exp(-v.rho*ds*6.4f));
            transmission-=w;depth+=w*d;
            for(int j=0;j<6;++j) light[j]+=w*v.light[j];
        }
        Coeff result{};
        const float a=1.f-transmission;
        if(a<.002f) return result;
        result.alpha=canopy::byte(a*255.f);
        for(int j=0;j<6;++j) result.light[j]=canopy::byte(255.f*light[j]/a);
        result.depth=std::int8_t(std::round(clamp(depth/a,-1.f,1.f)*127.f));
        return result;
    }
    void bakeLine() {
        const int slot=bakeRow/tile,row=bakeRow%tile,form=slot/Views;
        if(form!=formLoaded) {density.begin(form,seed);formLoaded=form;}
        if(!density.ready()) {density.advanceOne();return;}
        if(interiorForm!=form) {interiorForm=form;interiorSlice=0;interior[form].resize(InteriorGrid*InteriorGrid*InteriorGrid);}
        if(interiorSlice<InteriorGrid) {
            const int z=interiorSlice++;
            constexpr float step=2.f*Domain/(InteriorGrid-1);
            for(int y=0;y<InteriorGrid;++y)for(int x=0;x<InteriorGrid;++x)
                interior[form][(z*InteriorGrid+y)*InteriorGrid+x]=canopy::byte(255.f*density.sample(
                    {-Domain+x*step,-Domain+y*step,-Domain+z*step}).rho);
            return;
        }
        const int ox=slot%cols()*tile,oy=slot/cols()*tile;
        if(row>=2 && row<tile-2) {
            const float y=Extent*(1.f-2.f*(row-2+.5f)/(tile-4));
            for(int x=2;x<tile-2;++x) {
                const float u=Extent*(2.f*(x-2+.5f)/(tile-4)-1.f);
                coeff[std::size_t(oy+row)*width()+ox+x]=integrate(captures()[slot%Views],u,y);
            }
        }
        ++bakeRow;
        if(row!=tile-1) return;
        // Coarse, fixed world-space depth proxy. Average the integrated depth
        // over local opaque texels to avoid silhouette spikes from tiny wisps.
        for(int y=0;y<MeshSide;++y) for(int x=0;x<MeshSide;++x) {
            const int cx=2+(tile-5)*x/(MeshSide-1),cy=2+(tile-5)*y/(MeshSide-1);
            const int radius=std::max(2,tile/10);
            float d=0.f,w=0.f;
            for(int j=std::max(2,cy-radius);j<=std::min(tile-3,cy+radius);++j)
                for(int i=std::max(2,cx-radius);i<=std::min(tile-3,cx+radius);++i) {
                    const auto& a=coeff[std::size_t(oy+j)*width()+ox+i];
                    d+=float(a.depth)*a.alpha;w+=a.alpha;
                }
            surface[slot][y*MeshSide+x]=w>0.f?clamp(d/(127.f*w),-.7f,.7f)*.24f:0.f;
        }
        if(ready()) density.clear();
    }
public:
    std::vector<canopy::Pixel> pixels;
    int width() const {return tile==128?2048:1024;}
    int height() const {return tile==64?512:1024;}
    int cols() const {return width()/tile;}
    int tileSize() const {return tile;}
    bool ready() const {return tile && bakeRow==Forms*Views*tile;}
    void start(int size,std::uint32_t s) {
        assert(size==64 || size==96 || size==128);
        tile=size;seed=s;bakeRow=0;formLoaded=-1;interiorForm=-1;interiorSlice=0;interior={};
        coeff.assign(std::size_t(width())*height(),{});
        pixels.assign(coeff.size(),{});
        density.clear();surface={};
    }
    void advance(int milliseconds=2) {
        if(!tile || ready()) return;
        const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
        do {bakeLine();} while(!ready() && std::chrono::steady_clock::now()<end);
    }
    float progress() const {return tile?float(bakeRow)/(Forms*Views*tile):0.f;}
    const auto& depths(int form,int view) const {return surface[form*Views+view];}
    // Only 3*24^3 bytes persist for inexpensive flight-through sampling.
    float interiorDensity(int form,Vec p) const {
        if(form<0 || form>=Forms || interior[form].empty() || !finite(p)) return 0.f;
        const Vec q=(p+Vec{Domain,Domain,Domain})*((InteriorGrid-1)/(2.f*Domain));
        if(q.x<0.f || q.y<0.f || q.z<0.f || q.x>=InteriorGrid-1 || q.y>=InteriorGrid-1 || q.z>=InteriorGrid-1) return 0.f;
        const int x=int(q.x),y=int(q.y),z=int(q.z);
        const float fx=q.x-x,fy=q.y-y,fz=q.z-z;
        float a=0.f;
        for(int dz=0;dz<2;++dz)for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx)
            a+=interior[form][((z+dz)*InteriorGrid+y+dy)*InteriorGrid+x+dx]
                *(dx?fx:1.f-fx)*(dy?fy:1.f-fy)*(dz?fz:1.f-fz);
        return a/255.f;
    }
    void shade(const canopy::Light& l,int first,int last) {
        const Vec sun=unit(l.sun);
        const float sum=std::max(.001f,std::abs(sun.x)+std::abs(sun.y)+std::abs(sun.z));
        const std::array<float,6> sw{{std::max(0.f,sun.x)/sum,std::max(0.f,-sun.x)/sum,
            std::max(0.f,sun.y)/sum,std::max(0.f,-sun.y)/sum,std::max(0.f,sun.z)/sum,std::max(0.f,-sun.z)/sum}};
        // Soft, restrained multiple-scattering approximation. Night has no
        // residual daylight contrast and no additive halo.
        const RGB ambient=mixRGB(l.ambient,scaleRGB(l.bright,.64f-.15f*l.storm),l.day);
        std::array<float,Views> phase{};
        for(int j=0;j<Views;++j) {
            const float f=std::max(0.f,-dot(captures()[j].dir,sun));
            phase[j]=f*f*f*.18f*l.rim*l.day;
        }
        for(int y=first;y<last;++y) for(int x=0;x<width();++x) {
            const auto i=std::size_t(y)*width()+x;
            const auto& a=coeff[i];
            const int slot=y/tile*cols()+x/tile;
            if(x>=cols()*tile || slot>=Forms*Views || !a.alpha) {
                // Straight-alpha RGB gutter: neutral colour, zero coverage.
                pixels[i]={canopy::byte(l.bright.r),canopy::byte(l.bright.g),canopy::byte(l.bright.b),0};continue;
            }
            float direct=0.f,squared=0.f;
            // Blend directional transmittance with a lower-bound-like
            // harmonic optical response: more occlusion where directions
            // disagree, without an expensive logarithm for every texel.
            for(int j=0;j<6;++j) {
                const float t=a.light[j]/255.f;
                direct+=sw[j]*t;squared+=sw[j]*t*t;
            }
            direct=sat(direct-.42f*std::max(0.f,squared-direct*direct));
            const float rim=phase[slot%Views]*(1.f-a.alpha/255.f);
            // A little ambient bounce at the shadowed base and edge leakage;
            // no independent bright sprites or color boost at midnight.
            const float diffuse=std::sqrt(direct);
            const float edge=(1.f-a.alpha/255.f)*.20f*l.day;
            const float t=sat(mix(.82f,diffuse,l.contrast)+rim+edge);
            const auto c=mixRGB(ambient,l.bright,t);
            pixels[i]={canopy::byte(c.r),canopy::byte(c.g),canopy::byte(c.b),a.alpha};
        }
    }
    void uv(int form,int view,float x,float y,float& u,float& v) const {
        const int slot=form*Views+view;
        u=(slot%cols()*tile+2.f+x*(tile-4))/width();
        v=(slot/cols()*tile+2.f+y*(tile-4))/height();
    }
};
struct Refresh {
    canopy::Light shown{},pending{};
    int row=0;
    float elapsed=0.f;
    bool busy=false;
    void reset(const canopy::Light& l) {shown=pending=canopy::relativeLight(l);row=0;elapsed=0;busy=false;}
    bool advance(Atlas& atlas,const canopy::Light& live,float dt) {
        const auto l=canopy::relativeLight(live);
        elapsed=std::min(30.f,elapsed+clamp(dt,0.f,5.f));
        if(!busy && dt>0 && elapsed>=30.f && canopy::directionalDifference(shown,l)>.13f) {busy=true;pending=l;row=0;}
        if(!busy) return false;
        // Direction is the only delayed quantity, and large sun jumps
        // supersede an unfinished snapshot instead of publishing stale rows.
        if(canopy::directionalDifference(pending,l)>.45f) {pending=l;row=0;}
        const int last=std::min(atlas.height(),row+32);
        atlas.shade(pending,row,last);row=last;
        if(row!=atlas.height()) return false;
        shown=pending;busy=false;elapsed=0;return true;
    }
};
struct Group { Vec center;float scale,alpha;int form,order; float occupancy=1.f; };
struct Groups { std::array<Group,MaxGroups> items{};int count{}; };
inline float radius(const Settings& s,const Range& r) {return std::min(1480.f*s.sizeScale,r.end*.84f);}
inline Groups groups(const Settings& s,const View& view,const Range& r,const canopy::Drift& drift,const weather::State& climate,float ready) {
    Groups result;
    if(ready<=0.f || !s.hybridClouds || !target(s,climate.coverage)) return result;
    const float rad=radius(s,r),spacing=440.f*s.sizeScale,tile=canopy::tileWorldSize(s);
    const double dx=drift.worldX,dy=drift.worldY;
    const double cx=(view.pos.x-dx)/spacing,cy=(view.pos.y-dy)/spacing;
    // Only ordinary playable world coordinates are relevant, guard int conversion.
    if(std::abs(cx)>1e8 || std::abs(cy)>1e8) return result;
    const int ix=int(std::floor(cx)),iy=int(std::floor(cy)),span=int(std::ceil(rad/spacing))+1;
    const float threshold=canopy::threshold(s,climate.coverage)+.055f*climate.extraSunny;
    for(int j=-span;j<=span;++j) for(int i=-span;i<=span;++i) {
        const int x=ix+i,y=iy+j;
        const auto h=canopy::hash(x,y,std::uint32_t(s.seed));
        RNG rng{h?h:1u};
        const double wx=(x+rng.sample(.18f,.82f))*spacing,wy=(y+rng.sample(.18f,.82f))*spacing;
        const float z=mix(s.minSpawnZ,s.maxSpawnZ,rng.sample(.35f,.75f));
        const Vec pos{float(wx+dx),float(wy+dy),z};
        const float scale=rng.sample(150.f,215.f)*s.sizeScale;
        const int form=int(rng.next()%Forms);
        const Vec delta=pos-view.pos;
        const float horizontal=std::sqrt(horizontalSq(delta)),distance=length(delta);
        const float edge=smooth((rad-horizontal)/(rad*.28f));
        const float flight=smooth((distance/scale-.72f)/.58f);
        const float clip=distanceAlpha(r,distance);
        if(edge*clip<.015f) continue;
        const float coverage=smooth((canopy::shape(float(wx/tile),float(wy/tile),s.seed)-threshold+.075f)/.14f);
        const float occupancy=sat(coverage*edge*clip*s.cumulusDensity*s.opacity*ready
            *weather::formWeight(climate,form)*weather::visibility(climate)*weather::clearOpacity(climate));
        const float alpha=occupancy*flight;
        if(occupancy<.015f || result.count>=MaxGroups) continue;
        result.items[result.count++]={pos,scale,alpha,form,(j+span)*(2*span+1)+i+span,occupancy};
    }
    // Fixed identity tie-break, not camera-relative enumeration, for overlapping layers.
    std::sort(result.items.begin(),result.items.begin()+result.count,[&](const Group& a,const Group& b) {
        const float da=viewDepth(view,a.center),db=viewDepth(view,b.center);
        if(da!=db) return da>db;
        return a.order<b.order;
    });
    return result;
}
inline Groups groups(const Settings& s,const View& view,const Range& r,const canopy::Drift& drift,float coverage,float ready) {
    return groups(s,view,r,drift,weather::State{sat(coverage),sat(coverage),0,0,0,false},ready);
}
// Mild center-weighted interior haze. Sample actual density along nine
// short view rays; do not modify global fog, the game's weather or far clip.
constexpr int HazeSide=3,HazeVertices=9,HazeIndices=24;
struct Haze {std::array<float,HazeVertices> alpha{};float maxAlpha{};};
inline Haze interiorHaze(const Settings& s,const Atlas& atlas,const Groups& groups,const View& view,float wx,float wy) {
    Haze haze;
    if(s.interiorHaze<=0.f || !atlas.ready() || !std::isfinite(wx) || !std::isfinite(wy))return haze;
    // A finite, near-camera path keeps the overlay local and avoids
    // filling the entire sky with fog from an ordinary distant cloud.
    for(int i=0;i<groups.count;++i) {
        const auto& g=groups.items[i];
        const Vec delta=view.pos-g.center;
        const float rel=length(delta)/g.scale;
        if(rel>=1.34f || g.occupancy<=.001f) continue;
        const float proximity=smooth((1.34f-rel)/.49f);
        const Vec origin=delta/g.scale;
        for(int y=0;y<HazeSide;++y)for(int x=0;x<HazeSide;++x) {
            const Vec ray=unit(view.forward+view.right*((x-1)*wx)+view.up*((1-y)*wy));
            float optical=0.f;
            // World path capped at 90m; normalized traversal matches
            // the actual density field even when SizeScale changes.
            constexpr int steps=6;
            const float ds=std::min(90.f/g.scale,.8f)/steps;
            for(int j=0;j<steps;++j)
                optical+=atlas.interiorDensity(g.form,origin+ray*((j+.5f)*ds))*ds;
            const float a=(1.f-std::exp(-optical*8.f))*proximity*g.occupancy*s.interiorHaze;
            auto& h=haze.alpha[y*HazeSide+x];
            h=1.f-(1.f-h)*(1.f-sat(a));
            haze.maxAlpha=std::max(haze.maxAlpha,h);
        }
    }
    return haze;
}
// Concentrate the view blend slightly for close formations. Small angular
// weights contribute less double contour; still continuous at all boundaries.
inline std::array<Weight,3> renderWeights(Vec delta,float scale) {
    auto result=weights(delta);
    const float strength=smooth((scale/std::max(scale*.5f,length(delta))-.10f)/.45f);
    float total=0.f;
    for(auto& w:result) {w.amount=mix(w.amount,w.amount*w.amount,strength);total+=w.amount;}
    if(total>0.f) for(auto& w:result)w.amount/=total;
    return result;
}
inline float canopyBlend(const Groups& groups,float fade) {
    float strongest=0.f;
    for(int i=0;i<groups.count;++i) strongest=std::max(strongest,groups.items[i].occupancy);
    return fade*smooth(strongest/.35f);
}
struct Mesh {
    std::array<canopy::Vertex,MeshVertices> vertices{};
    std::array<std::uint16_t,MeshIndices> indices{};
};
inline Mesh mesh(const Atlas& atlas,const Group& group,Weight w) {
    const auto& c=captures()[w.view];
    const auto& depth=atlas.depths(group.form,w.view);
    Mesh m;
    // Approximate optical-depth split for typical opaque cores. Deliberately
    // avoid additive colour; near transparent silhouettes remain approximate.
    const float reference=.87f*group.alpha;
    const float alpha=reference>.001f?(1.f-std::pow(1.f-reference,w.amount))/.87f:0.f;
    for(int y=0;y<MeshSide;++y) for(int x=0;x<MeshSide;++x) {
        const int i=y*MeshSide+x;
        const float u=float(x)/(MeshSide-1),v=float(y)/(MeshSide-1);
        auto& p=m.vertices[i];
        p.pos=group.center+(c.right*(Extent*(2.f*u-1.f))+c.up*(Extent*(1.f-2.f*v))+c.dir*depth[i])*group.scale;
        atlas.uv(group.form,w.view,u,v,p.u,p.v);
        p.alpha=sat(alpha);
    }
    int k=0;
    for(int y=0;y<MeshSide-1;++y) for(int x=0;x<MeshSide-1;++x) {
        const auto a=std::uint16_t(y*MeshSide+x),b=std::uint16_t(a+1),c=std::uint16_t(a+MeshSide),d=std::uint16_t(c+1);
        for(auto n:{a,c,b,b,c,d})m.indices[k++]=n;
    }
    return m;
}
} // namespace vc::volume
