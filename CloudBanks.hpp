#pragma once
#include "Canopy.hpp"

// Continuous, world-space density. Small periodic volume texture; no impostors,
// meshes of individual puffs, view atlas or per-frame procedural noise.
namespace vc::banks {
constexpr int Width=128, Height=128, Depth=32;
constexpr int DetailWidth=256,DetailDepth=32;
constexpr int MaxSlices=16, DetailSlices=24, GridX=12,GridY=10;
constexpr int OccupancyXY=16,OccupancyZ=8;
constexpr float ReferenceStep=65.f;
struct Vertex {Vec pos;float u{},v{},w{},alpha{};};
struct Mesh {std::vector<Vertex> vertices;std::vector<std::uint16_t> indices;int slices{};};
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
        const float clearThreshold=canopy::threshold(s,0),wetThreshold=canopy::threshold(s,1);
        const auto macro=[&](float u,float v) {
            return .28f*map.sample(u,v)+.18f*(map.sample(u+.025f,v)+map.sample(u-.025f,v)
                +map.sample(u,v+.025f)+map.sample(u,v-.025f));
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
            const float tower=smooth((broad-std::min(clearThreshold,wetThreshold)+.025f)/.17f);
            const float floor=base-.14f+.035f*(n1-.5f);
            const float ceiling=base+.035f+.29f*tower+.15f*(n1-.5f)+.04f*(n2-.5f);
            const float vertical=smooth((h-floor)/.09f)*smooth((ceiling-h)/.085f);
            const float topTaper=.045f*smooth((h-base-.02f)/.24f);
            const float f=broad+.23f*(n1-.5f)+.13f*(n2-.5f)+.065f*(n3-.5f)-topTaper;
            const auto i=index(x,y,z);
            coverage[i]=f;
            rho[i]=z==0||z==depth-1?0.f:vertical;
        }
        for(int weather=0;weather<2;++weather) {
            pixels[weather].resize(count);
            const float threshold=weather?wetThreshold:clearThreshold;
            const float density=s.cumulusDensity*s.opacity*smooth(float(target(s,float(weather)))/30.f);
            for(int y=0;y<width;++y)for(int x=0;x<width;++x) {
                // Overhead optical depth, plus two cheap neighbouring taps
                // for baked directional relief. Brighten thin edges, shade
                // dense undersides; actual sky RGB applied per frame.
                float column=0.f;
                for(int z=depth-1;z>=0;--z) {
                    const auto i=index(x,y,z);
                    const float cover=smooth((coverage[i]-threshold+.018f)/.080f);
                    const float d=rho[i]*cover;
                    const auto neighbor=[&](int dx,int dy,int dz) {
                        const auto j=index(x+dx,y+dy,z+dz);
                        return rho[j]*smooth((coverage[j]-threshold+.018f)/.080f);
                    };
                    const float lateral=.52f*(neighbor(-2,-1,1)+neighbor(-4,-2,2))-.52f*(neighbor(2,1,1)+neighbor(4,2,2));
                    // A darker underside and soft occlusion are closer to
                    // observed daylight cumulus than uniformly white cards.
                    const float transmission=std::exp(-column*.22f);
                    const float shade=clamp(1.f-s.cumulusContrast*(.42f*(1.f-transmission)+.25f*lateral)
                        +s.cumulusRimStrength*.08f*(1.f-d),.50f,1.f);
                    const float alpha=1.f-std::exp(-d*1.15f*density);
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
// Render a finite stack of view-aligned sample planes. The geometry aligns
// with the view for efficient compositing, but the texture coordinates are
// absolute world XYZ: no cloud object can swing with camera orientation.
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
    const int slices=std::clamp(int(std::ceil((last-first)/ReferenceStep)),1,cap);
    const float step=(last-first)/slices;
    const float tile=canopy::tileWorldSize(s);
    // Keep UVs near zero to retain precision in high-speed flight.
    const double uu=double(view.pos.x)/tile-drift.x,vv=double(view.pos.y)/tile-drift.y;
    const float u0=float(uu-std::floor(uu)),v0=float(vv-std::floor(vv));
    const float refOpacity=std::min(1.f,step/ReferenceStep);
    const float fade=smooth((rad-5.f)/120.f);
    const int per=(GridX+1)*(GridY+1);
    m.vertices.reserve(std::size_t(cap)*per);m.indices.reserve(std::size_t(cap)*GridX*GridY*6);
    for(int k=slices-1;k>=0;--k) {
        const float d=first+(k+.5f)*step;
        const int start=int(m.vertices.size());
        for(int y=0;y<=GridY;++y)for(int x=0;x<=GridX;++x) {
            const Vec ray=view.forward+view.right*((2.f*x/GridX-1.f)*wx)+view.up*((1.f-2.f*y/GridY)*wy);
            const Vec delta=ray*d,pos=view.pos+delta;
            const float horizontal=std::sqrt(horizontalSq(delta));
            const float rangeWeight=distanceAlpha(r,length(delta));
            const float edge=smooth((rad-horizontal)/(rad*.34f));
            // The density texture is clamped vertically to empty border
            // slices. Geometry outside the layer therefore contributes zero.
            const float w=(pos.z-s.minSpawnZ)/band;
            const float nearWeight=mix(smooth((length(delta)-view.nearClip)/std::max(80.f,s.nearFadeDistance)),1.f,s.interiorHaze);
            const float a=sat(refOpacity*length(ray))*edge*rangeWeight*nearWeight*fade;
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
