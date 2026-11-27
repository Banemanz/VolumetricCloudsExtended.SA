#pragma once
#include "Canopy.hpp"

// Continuous, world-space density. Small periodic volume texture; no impostors,
// meshes of individual puffs, view atlas or per-frame procedural noise.
namespace vc::banks {
constexpr int Width=128, Height=128, Depth=32;
constexpr int DetailWidth=256,DetailDepth=32;
// Constant world-distance sampling. Never stretch the sample interval to
// fit the current frustum: that causes the field to swim during flight.
constexpr float EconomyStep=40.f,DetailStep=28.f;
constexpr int MaxSlices=36,DetailSlices=52,GridX=12,GridY=10;
constexpr int OccupancyXY=16,OccupancyZ=8;
constexpr float ReferenceStep=90.f; // opacity baked for this path length
struct Vertex {Vec pos;float u{},v{},w{},alpha{};};
struct Mesh {std::vector<Vertex> vertices;std::vector<std::uint16_t> indices;int slices{};};
static_assert(DetailSlices*(GridX+1)*(GridY+1)<65536);
inline float radius(const Settings& s,const Range& r) {
    return std::min({canopy::tileWorldSize(s)*.62f,r.end*.90f,1400.f});
}
inline float random3(int x,int y,int z,int n,int nz,std::uint32_t seed) {
    return float(canopy::hash(canopy::wrap(x,n),canopy::wrap(y,n),seed^
        (std::uint32_t(canopy::wrap(z,nz))*0x9e3779b9u))>>8)*(1.f/16777216.f);
}
inline float noise3(float u,float v,float w,int n,int nz,std::uint32_t seed) {
    const float x=u*n,y=v*n,z=w*nz;
    const int i=int(std::floor(x)),j=int(std::floor(y)),k=int(std::floor(z));
    const float a=smooth(x-i),b=smooth(y-j),c=smooth(z-k);
    const auto plane=[&](int iz) {
        return mix(mix(random3(i,j,iz,n,nz,seed),random3(i+1,j,iz,n,nz,seed),a),
                   mix(random3(i,j+1,iz,n,nz,seed),random3(i+1,j+1,iz,n,nz,seed),a),b);
    };
    return mix(plane(k),plane(k+1),c);
}
struct Volume {
    int width{},depth{};
    std::array<std::array<std::uint8_t,OccupancyXY*OccupancyXY*OccupancyZ>,2> occupied{};
    std::array<std::vector<canopy::Pixel>,2> pixels; // exact clear and overcast endpoints
    std::size_t index(int x,int y,int z) const {
        return (std::size_t(std::clamp(z,0,depth-1))*width+canopy::wrap(y,width))*width+canopy::wrap(x,width);
    }
    void build(const Settings& s,const canopy::Map& map) {
        width=s.cumulusDetail?DetailWidth:Width;
        depth=s.cumulusDetail?DetailDepth:Depth;
        const auto count=std::size_t(width)*width*depth;
        std::vector<float> rho(count),coverage(count);
        const auto seed=std::uint32_t(s.seed);
        const float base=sat((canopy::altitude(s)-s.minSpawnZ)/std::max(1.f,s.maxSpawnZ-s.minSpawnZ));
        const float clearThreshold=canopy::threshold(s,0)+.026f,wetThreshold=canopy::threshold(s,1)+.026f;
        const auto macro=[&](float u,float v) {
            return .52f*map.sample(u,v)+.12f*(map.sample(u+.018f,v)+map.sample(u-.018f,v)
                +map.sample(u,v+.018f)+map.sample(u,v-.018f));
        };
        // The canopy supplies broad coverage, not vertical extrusion of
        // every fine 2D detail. 3D billow noise distorts both shape and
        // footprint with height; dense cores have rounded, tapering tops.
        for(int z=0;z<depth;++z)for(int y=0;y<width;++y)for(int x=0;x<width;++x) {
            const float u=(x+.5f)/width,v=(y+.5f)/width,h=(z+.5f)/depth;
            const float n1=noise3(u,v,h,16,5,seed+711);
            const float n2=noise3(u,v,h,36,12,seed+713);
            const float n3=noise3(u,v,h,72,24,seed+715);
            const float warpU=u+.038f*(noise3(u,v,h,9,4,seed+719)-.5f);
            const float warpV=v+.038f*(noise3(u,v,h,9,4,seed+721)-.5f);
            const float broad=macro(warpU,warpV);
            // Lower, wide bases and rounded crowns instead of pillars.
            const float tower=smooth((broad-std::min(clearThreshold,wetThreshold)+.012f)/.20f);
            const float floor=base-.24f+.045f*(n1-.5f)+.035f*(1.f-tower);
            const float ceiling=base+.045f+.39f*tower+.18f*(n1-.5f)+.060f*(n2-.5f);
            const float vertical=smooth((h-floor)/.075f)*smooth((ceiling-h)/.075f);
            const float topTaper=.065f*smooth((h-base+.01f)/.27f);
            const float f=broad+.28f*(n1-.5f)+.15f*(n2-.5f)+.075f*(n3-.5f)-topTaper;
            const auto i=index(x,y,z);
            coverage[i]=f;
            rho[i]=z==0||z==depth-1?0.f:vertical;
        }
        for(int weather=0;weather<2;++weather) {
            pixels[weather].resize(count);
            const float threshold=weather?wetThreshold:clearThreshold;
            const float density=s.cumulusDensity*s.opacity*smooth(float(target(s,float(weather)))/30.f);
            // Reuse a scalar endpoint field for a short baked light integration.
            // This is a one-time CPU calculation, not a shader/raymarch at draw.
            std::vector<float> dField(count);
            for(std::size_t i=0;i<count;++i)
                dField[i]=rho[i]*smooth((coverage[i]-threshold+.006f)/.065f);
            const float xyStep=canopy::tileWorldSize(s)/width;
            const float zStep=(s.maxSpawnZ-s.minSpawnZ)/depth;
            const int lateralStep=std::max(1,int(std::round(35.f/std::max(1.f,xyStep))));
            const int riseStep=std::max(1,int(std::round(22.f/std::max(1.f,zStep))));
            for(int y=0;y<width;++y)for(int x=0;x<width;++x) {
                float column=0.f;
                for(int z=depth-1;z>=0;--z) {
                    const auto i=index(x,y,z);
                    const float d=dField[i];
                    if(d<=0.f){pixels[weather][i]={255,255,255,0};continue;}
                    const auto at=[&](int dx,int dy,int dz) {
                        return dField[index(x+dx,y+dy,z+dz)];
                    };
                    // A shallow, broad light path reveals relief from above,
                    // where overhead extinction alone washed every top white.
                    float oblique=0.f;
                    for(int k=1;k<=4;++k)
                        oblique+=at(-k*lateralStep,-k*lateralStep/2,k*riseStep);
                    const float topDepth=column*zStep/90.f;
                    const float sideDepth=oblique*.45f;
                    const float ambient=std::exp(-.38f*topDepth-.56f*sideDepth);
                    const float slope=clamp((at(lateralStep,0,0)-at(-lateralStep,0,0)
                        +at(0,lateralStep,0)-at(0,-lateralStep,0))*.5f,-1.f,1.f);
                    const float shade=clamp(1.f-s.cumulusContrast*(.59f*(1.f-ambient)+.22f*d+.15f*slope)
                        +s.cumulusRimStrength*.035f*(1.f-d),.50f,1.f);
                    // Reference alpha for 90m of cloud; individual slices
                    // scale this by their actual path length.
                    const float alpha=1.f-std::exp(-d*(.32f+.95f*d)*density);
                    const auto gray=canopy::byte(shade*255.f);
                    pixels[weather][i]={gray,gray,gray,canopy::byte(alpha*255.f)};
                    column+=d;
                }
            }
        }
        buildOccupancy();
    }
    void buildOccupancy() {
        // Conservative block maximum including the one-texel filtering
        // halo, valid even when sampling across a periodic texture seam.
        for(int layer=0;layer<2;++layer)for(int z=0;z<OccupancyZ;++z)
            for(int y=0;y<OccupancyXY;++y)for(int x=0;x<OccupancyXY;++x) {
                std::uint8_t a=0;
                for(int k=z*depth/OccupancyZ-1;k<=(z+1)*depth/OccupancyZ;++k)
                    for(int j=y*width/OccupancyXY-1;j<=(y+1)*width/OccupancyXY;++j)
                        for(int i=x*width/OccupancyXY-1;i<=(x+1)*width/OccupancyXY;++i)
                            a=std::max(a,pixels[layer][index(i,j,k)].a);
                occupied[layer][(z*OccupancyXY+y)*OccupancyXY+x]=a;
            }
    }
    bool potentiallyOccupied(float u0,float v0,float w0,float u1,float v1,float w1,float weather) const {
        if(w1<0.f||w0>1.f)return false;
        const int xa=int(std::floor(u0*OccupancyXY)),xb=int(std::floor(u1*OccupancyXY));
        const int ya=int(std::floor(v0*OccupancyXY)),yb=int(std::floor(v1*OccupancyXY));
        const int za=std::clamp(int(std::floor(w0*OccupancyZ)),0,OccupancyZ-1),
                  zb=std::clamp(int(std::floor(w1*OccupancyZ)),0,OccupancyZ-1);
        // A large cell spans too many blocks; drawing costs less than a lengthy CPU scan.
        if(xb-xa>OccupancyXY||yb-ya>OccupancyXY)return true;
        for(int z=za;z<=zb;++z)for(int y=ya;y<=yb;++y)for(int x=xa;x<=xb;++x) {
            const auto i=(z*OccupancyXY+canopy::wrap(y,OccupancyXY))*OccupancyXY+canopy::wrap(x,OccupancyXY);
            if((weather<.999f&&occupied[0][i]>0)||(weather>.001f&&occupied[1][i]>0))return true;
        }
        return false;
    }
    canopy::Pixel sample(int layer,float u,float v,float w) const {
        if(!width||pixels[layer].empty())return {};
        const float x=canopy::fract(u)*width-.5f,y=canopy::fract(v)*width-.5f,z=sat(w)*depth-.5f;
        const int ix=int(std::floor(x)),iy=int(std::floor(y)),iz=int(std::floor(z));
        const float a=x-ix,b=y-iy,c=z-iz;
        const auto interp=[&](auto member) {
            const auto row=[&](int dy,int dz){return mix(float(pixels[layer][index(ix,iy+dy,iz+dz)].*member),
                                                       float(pixels[layer][index(ix+1,iy+dy,iz+dz)].*member),a);};
            return canopy::byte(mix(mix(row(0,0),row(1,0),b),mix(row(0,1),row(1,1),b),c));
        };
        auto g=interp(&canopy::Pixel::r);
        return {g,g,g,interp(&canopy::Pixel::a)};
    }
};
// The bank lighting is intentionally independent of the existing canopy.
// SA applies a further colour filter after RenderEffects (particularly strong
// at dawn); reserve headroom instead of painting every dense top near 255.
inline RGB bankLight(const canopy::Light& l,const weather::State& w) {
    if(w.flash)return l.bright;
    const float lowSun=l.day*(1.f-smooth((l.sun.z-.06f)/.44f));
    const float gain=1.f-l.day*.10f-lowSun*.18f;
    return scaleRGB(l.bright,gain);
}
// Parallel, view-facing integration planes, anchored to a fixed world phase.
// Moving the camera or changing its far clip does not stretch or slide the
// sampling grid. Pitch/yaw can still change quadrature (not world density).
// Avoid intersecting axis-aligned cells, which expose their edges in flight.
inline void buildMesh(Mesh& m,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,float wx,float wy,const Volume* volume=nullptr,float weather=0.f) {
    m.vertices.clear();m.indices.clear();m.slices=0;
    const float rad=radius(s,r),band=s.maxSpawnZ-s.minSpawnZ;
    if(!s.hybridClouds||!s.targetProcClouds||rad<5.f||band<1.f||
        !std::isfinite(wx)||!std::isfinite(wy)||wx<=0||wy<=0||wx>10.f||wy>10.f||
        view.pos.z<s.minSpawnZ-r.end||view.pos.z>s.maxSpawnZ+r.end)return;
    // Project bounding prism to view depth; intersect with actual camera clip.
    const float z0=s.minSpawnZ-view.pos.z,z1=s.maxSpawnZ-view.pos.z;
    const float lateral=rad*std::sqrt(horizontalSq(view.forward));
    const float first=std::max(std::max(view.nearClip,8.f),std::min(z0*view.forward.z,z1*view.forward.z)-lateral);
    const float last=std::min(r.end,std::max(z0*view.forward.z,z1*view.forward.z)+lateral);
    if(last-first<1.f)return;
    const int cap=s.cumulusDetail?DetailSlices:MaxSlices;
    const float step=s.cumulusDetail?DetailStep:EconomyStep;
    const float tile=canopy::tileWorldSize(s);
    // Anchor the phase to the wind-advected density rather than the eye.
    // Use doubles for the phase so crossing large world coordinates does
    // not cause the lattice to snap. Only whole planes enter/leave range.
    const double phase=double(view.forward.x)*(double(view.pos.x)-drift.worldX)
        +double(view.forward.y)*(double(view.pos.y)-drift.worldY)
        +double(view.forward.z)*(double(view.pos.z)-s.minSpawnZ);
    const double phaseOffset=phase-step*std::floor(phase/step);
    const int firstPlane=int(std::ceil((first+phaseOffset)/step-.5));
    const int lastPlane=std::min(firstPlane+cap-1,int(std::floor((last+phaseOffset)/step-.5)));
    if(lastPlane<firstPlane)return;
    // Keep UVs near zero to retain precision in high-speed flight.
    const double uu=double(view.pos.x)/tile-drift.x,vv=double(view.pos.y)/tile-drift.y;
    const float u0=float(uu-std::floor(uu)),v0=float(vv-std::floor(vv));
    const float refOpacity=std::min(1.f,step/ReferenceStep);
    const float fade=smooth((rad-5.f)/120.f);
    const int per=(GridX+1)*(GridY+1);
    m.vertices.reserve(std::size_t(cap)*per);m.indices.reserve(std::size_t(cap)*GridX*GridY*6);
    for(int k=lastPlane;k>=firstPlane;--k) {
        const float d=float((k+.5)*step-phaseOffset);
        const int start=int(m.vertices.size());
        for(int y=0;y<=GridY;++y)for(int x=0;x<=GridX;++x) {
            const Vec ray=view.forward+view.right*((2.f*x/GridX-1.f)*wx)+view.up*((1.f-2.f*y/GridY)*wy);
            const Vec delta=ray*d,pos=view.pos+delta;
            const float horizontal=std::sqrt(horizontalSq(delta));
            const float rangeWeight=distanceAlpha(r,length(delta));
            const float edge=smooth((rad-horizontal)/(rad*.34f));
            const float capFade=smooth((first+cap*step-d)/(step*2.f));
            // The density texture is clamped vertically to empty border
            // slices. Geometry outside the layer therefore contributes zero.
            const float w=(pos.z-s.minSpawnZ)/band;
            const float nearWeight=mix(smooth((length(delta)-view.nearClip)/std::max(80.f,s.nearFadeDistance)),1.f,s.interiorHaze*.22f);
            const float a=sat(refOpacity*length(ray))*edge*rangeWeight*nearWeight*fade*capFade;
            m.vertices.push_back({pos,u0+delta.x/tile,v0+delta.y/tile,w,a});
        }
        const auto before=m.indices.size();
        for(int y=0;y<GridY;++y)for(int x=0;x<GridX;++x) {
            const auto a=std::uint16_t(start+y*(GridX+1)+x),b=std::uint16_t(a+1),c=std::uint16_t(a+GridX+1),e=std::uint16_t(c+1);
            const auto& A=m.vertices[a];const auto& B=m.vertices[b];const auto& C=m.vertices[c];const auto& E=m.vertices[e];
            if(std::max({A.alpha,B.alpha,C.alpha,E.alpha})<=1.f/255.f ||
               std::max({A.w,B.w,C.w,E.w})<0.f||std::min({A.w,B.w,C.w,E.w})>1.f)continue;
            if(volume&&!volume->potentiallyOccupied(
                 std::min({A.u,B.u,C.u,E.u}),std::min({A.v,B.v,C.v,E.v}),std::min({A.w,B.w,C.w,E.w}),
                 std::max({A.u,B.u,C.u,E.u}),std::max({A.v,B.v,C.v,E.v}),std::max({A.w,B.w,C.w,E.w}),weather))continue;
            for(auto i:{a,c,b,b,c,e})m.indices.push_back(i);
        }
        if(m.indices.size()==before)m.vertices.resize(start);else ++m.slices;
    }
}
} // namespace vc::banks
