#pragma once
#include "Canopy.hpp"

// Continuous, world-space density. Small periodic volume texture; no impostors,
// meshes of individual puffs, view atlas or per-frame procedural noise.
namespace vc::banks {
constexpr int Width=128, Height=128, Depth=32;
constexpr int DetailWidth=256,DetailDepth=32;
// Constant world-distance sampling. Never stretch the sample interval to
// fit the current frustum: that causes the field to swim during flight.
// Nested sampling lattice: fine near the player, standard in the middle,
// coarse at distance. Contributions crossfade without changing positions.
constexpr float EconomyStep=20.f,DetailStep=14.f;
constexpr int MaxSlices=60,DetailSlices=90,GridX=12,GridY=10;
constexpr int OccupancyXY=16,OccupancyZ=8;
constexpr float ReferenceStep=90.f; // opacity baked for this path length
struct Vertex {Vec pos;float u{},v{},w{},alpha{},fog{1.f};};
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
            // Erode weak edges with a billow-shaped detail octave instead
            // of coating the whole formation in uniform high-frequency fuzz.
            const float edgeDetail=(n3-.5f)*(.07f+.045f*(1.f-tower));
            const float f=broad+.29f*(n1-.5f)+.165f*(n2-.5f)+edgeDetail-topTaper;
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
                    // Soft multiple-scattering proxy: dense sheltered cores
                    // remain shaded, but the sun-facing crown retains a
                    // broad, pale highlight rather than a grey flat mass.
                    const float trans=std::exp(-.43f*topDepth-.64f*sideDepth);
                    const float ambient=.22f*std::exp(-.16f*(topDepth+sideDepth))+.78f*trans;
                    const float slope=clamp((at(lateralStep,0,0)-at(-lateralStep,0,0)
                        +at(0,lateralStep,0)-at(0,-lateralStep,0))*.5f,-1.f,1.f);
                    const float localRelief=clamp(d-(at(1,0,0)+at(-1,0,0)+at(0,1,0)+at(0,-1,0))*.25f,-.35f,.35f);
                    const float litRim=(1.f-d)*trans;
                    const float shade=clamp(.995f-s.cumulusContrast*(.64f*(1.f-ambient)+.18f*d+.19f*slope+.22f*localRelief)
                        +s.cumulusRimStrength*.065f*litRim,.50f,1.f);
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
// A world-locked sampling normal with a small angular dead zone. Tilt
// the lattice only when necessary, about the eye rather than world origin.
// This removes phase jumps from small changes of pitch/yaw at large XYZ.
struct SamplingFrame {
    Vec normal{},lastPos{};
    double phase{},lastWindX{},lastWindY{};
    bool ready=false;
    float lastStep{};
    void update(const View& view,const canopy::Drift& drift,float step,float baseZ) {
        const double period=4.0*step;
        if(!ready||lastStep!=step||length(view.pos-lastPos)>600.f) {
            normal=view.forward;
            phase=double(normal.x)*(double(view.pos.x)-drift.worldX)
                +double(normal.y)*(double(view.pos.y)-drift.worldY)
                +double(normal.z)*(double(view.pos.z)-baseZ);
            ready=true;
        } else {
            const float angle=std::acos(clamp(dot(normal,view.forward),-1.f,1.f));
            constexpr float deadZone=8.f*Pi/180.f;
            if(angle>deadZone) {
                // Exact slerp to the edge of the dead zone; avoid a slow
                // catch-up or a zero normal after a large camera cut.
                const float sinAngle=std::sin(angle);
                normal=std::abs(sinAngle)<.001f?view.forward:
                    unit(normal*(std::sin(deadZone)/sinAngle)+view.forward*(std::sin(angle-deadZone)/sinAngle));
            }
            // The transfer phase is insensitive to rotating in place.
            phase+=double(normal.x)*(double(view.pos.x-lastPos.x)-(drift.worldX-lastWindX))
                +double(normal.y)*(double(view.pos.y-lastPos.y)-(drift.worldY-lastWindY))
                +double(normal.z)*double(view.pos.z-lastPos.z);
        }
        phase-=period*std::floor(phase/period);
        lastPos=view.pos;lastWindX=drift.worldX;lastWindY=drift.worldY;lastStep=step;
    }
};
// Weights are in metres of optical path along the lattice normal. Sum
// across every four consecutive lattice positions is always 4*step at
// any fixed blend value. Near 20m, mid 40m, far 80m (detail 14/28/56).
inline float sampleWeight(int k,float d,float step) {
    const float fine=1.f-smooth((d-160.f)/200.f);
    const float coarse=smooth((d-650.f)/350.f);
    const int parity=((k%4)+4)%4;
    const float mid=parity%2==0?2.f*(1.f+(parity==0?coarse:-coarse)):0.f;
    return step*(fine+(1.f-fine)*mid);
}
inline void buildMesh(Mesh& m,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,float wx,float wy,const Volume* volume=nullptr,float weather=0.f,
                      SamplingFrame* persisted=nullptr) {
    m.vertices.clear();m.indices.clear();m.slices=0;
    const float rad=radius(s,r),band=s.maxSpawnZ-s.minSpawnZ;
    if(!s.hybridClouds||!s.targetProcClouds||rad<5.f||band<1.f||
        !std::isfinite(wx)||!std::isfinite(wy)||wx<=0||wy<=0||wx>10.f||wy>10.f||
        view.pos.z<s.minSpawnZ-r.end||view.pos.z>s.maxSpawnZ+r.end)return;
    const float step=s.cumulusDetail?DetailStep:EconomyStep;
    const int cap=s.cumulusDetail?DetailSlices:MaxSlices;
    SamplingFrame local;
    auto& grid=persisted?*persisted:local;
    grid.update(view,drift,step,s.minSpawnZ);
    // Extremely wide custom FOVs may cross the locked plane's tangent.
    // In that case use the actual view normal, preserving the phase.
    constexpr int per=(GridX+1)*(GridY+1);
    std::array<Vec,per> rays{};
    std::array<float,per> rayLen{},horizontal{};
    float minCos=1e9f;
    auto prepare=[&] {
        minCos=1e9f;
        for(int y=0;y<=GridY;++y)for(int x=0;x<=GridX;++x) {
            const int i=y*(GridX+1)+x;
            const Vec ray=view.forward+view.right*((2.f*x/GridX-1.f)*wx)+view.up*((1.f-2.f*y/GridY)*wy);
            const float cosine=dot(ray,grid.normal);
            minCos=std::min(minCos,cosine);
            rays[i]=ray/std::max(.01f,cosine);
            rayLen[i]=length(rays[i]);horizontal[i]=std::sqrt(horizontalSq(rays[i]));
        }
    };
    prepare();
    if(minCos<.35f){grid.normal=view.forward;prepare();}
    const float z0=s.minSpawnZ-view.pos.z,z1=s.maxSpawnZ-view.pos.z;
    const float lateral=rad*std::sqrt(horizontalSq(grid.normal));
    // Bounding prism and radial range, independent of game far clip.
    const float first=std::max(std::max(view.nearClip,8.f)*minCos,std::min(z0*grid.normal.z,z1*grid.normal.z)-lateral);
    const float last=std::min({r.end,std::max(z0*grid.normal.z,z1*grid.normal.z)+lateral,rad+band});
    if(last-first<1.f)return;
    const int firstPlane=int(std::ceil((first+grid.phase)/step));
    int lastPlane=int(std::floor((last+grid.phase)/step));
    int candidates=0;
    for(int k=firstPlane;k<=lastPlane;++k)if(sampleWeight(k,float(k*step-grid.phase),step)>=.1f)
        if(++candidates>cap){lastPlane=k-1;break;}
    const float tile=canopy::tileWorldSize(s);
    const double uu=double(view.pos.x)/tile-drift.x,vv=double(view.pos.y)/tile-drift.y;
    const float u0=float(uu-std::floor(uu)),v0=float(vv-std::floor(vv));
    const float fade=smooth((rad-5.f)/120.f);
    m.vertices.reserve(std::size_t(cap)*per);m.indices.reserve(std::size_t(cap)*GridX*GridY*6);
    // Fixed normal distance is used for LOD, not direction-dependent
    // screen depth. It stays invariant while the camera pans in the dead zone.
    for(int k=lastPlane;k>=firstPlane;--k) {
        const float d=float(k*step-grid.phase);
        // The narrow near slices do not require a high-resolution mesh:
        // density itself is sampled per pixel. Keep the shared 12x10 ray
        // table but use 6x5 triangles near the eye, where alpha is smooth.
        const int stride=d<180.f?2:1;
        const float weight=sampleWeight(k,d,step);
        if(weight<.1f)continue;
        const int start=int(m.vertices.size());
        for(int i=0;i<per;++i) {
            const Vec delta=rays[i]*d,pos=view.pos+delta;
            const float dist=rayLen[i]*d;
            const float edge=smooth((rad-horizontal[i]*d)/(rad*.34f));
            const float w=(pos.z-s.minSpawnZ)/band;
            const float nearWeight=smooth((dist-8.f)/24.f)*mix(smooth((dist-view.nearClip)/std::max(80.f,s.nearFadeDistance)),1.f,s.interiorHaze*.22f);
            const float a=sat(weight*rayLen[i]/ReferenceStep)*edge*distanceAlpha(r,dist)*nearWeight*fade;
            m.vertices.push_back({pos,u0+delta.x/tile,v0+delta.y/tile,w,a,cloudFogFactor(s,dist)});
        }
        const auto before=m.indices.size();
        for(int y=0;y<GridY;y+=stride)for(int x=0;x<GridX;x+=stride) {
            const auto a=std::uint16_t(start+y*(GridX+1)+x),b=std::uint16_t(a+stride),c=std::uint16_t(a+stride*(GridX+1)),e=std::uint16_t(c+stride);
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
