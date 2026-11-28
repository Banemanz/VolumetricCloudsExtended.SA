#pragma once
#include "CloudBanks.hpp"
#include <limits>

// Cheap underside impostor of the *actual* 3D density. It uses the same
// endpoint weather masks, baked lighting, periodic tile and wind phase.
// No ray integration during gameplay, no extra textures or pixel shader.
namespace vc::canopyVolume {
using MaskPair=std::array<std::vector<canopy::Pixel>,2>;
struct Column {float premultiplied{},alpha{};};
inline void project(const Settings& s,const banks::Volume& volume,int size,MaskPair& masks) {
    const int n=volume.width;
    const float path=(s.maxSpawnZ-s.minSpawnZ)/volume.depth/banks::ReferenceStep;
    std::array<float,256> optical{};
    for(int a=0;a<256;++a)optical[a]=1.f-std::pow(1.f-a/255.f,path);
    std::vector<Column> columns(std::size_t(n)*n);
    struct Tap {int first,last;float f;};
    std::vector<Tap> taps(size);
    for(int i=0;i<size;++i) {
        const float uv=(i+.5f)*n/size-.5f;
        const int a=int(std::floor(uv));
        taps[i]={canopy::wrap(a,n),canopy::wrap(a+1,n),uv-a};
    }
    for(int layer=0;layer<2;++layer) {
        // Looking up from below: front-to-back compositing in the same
        // luminance/alpha field as the volume. Store premultiplied values
        // until resampling so transparent white texels cannot create halos.
        // Walk contiguous volume slices, not strided individual Z columns.
        // The alpha member temporarily stores unscattered transmittance.
        std::fill(columns.begin(),columns.end(),Column{0.f,1.f});
        const auto planeSize=std::size_t(n)*n;
        for(int z=0;z<volume.depth;++z) {
            const auto* src=volume.pixels[layer].data()+std::size_t(z)*planeSize;
            for(std::size_t i=0;i<planeSize;++i) {
                const auto pixel=src[i];
                if(!pixel.a)continue;
                auto& c=columns[i];
                const float a=optical[pixel.a];
                c.premultiplied+=c.alpha*a*pixel.r;
                c.alpha*=1.f-a;
            }
        }
        for(auto& c:columns)c.alpha=1.f-c.alpha;
        auto& out=masks[layer];out.resize(std::size_t(size)*size);
        for(int y=0;y<size;++y)for(int x=0;x<size;++x) {
            const auto tx=taps[x],ty=taps[y];
            const auto a=columns[std::size_t(ty.first)*n+tx.first],b=columns[std::size_t(ty.first)*n+tx.last],
                c=columns[std::size_t(ty.last)*n+tx.first],d=columns[std::size_t(ty.last)*n+tx.last];
            const float opacity=mix(mix(a.alpha,b.alpha,tx.f),mix(c.alpha,d.alpha,tx.f),ty.f);
            const float shade=mix(mix(a.premultiplied,b.premultiplied,tx.f),mix(c.premultiplied,d.premultiplied,tx.f),ty.f);
            const auto g=canopy::byte(opacity>0.f?shade/opacity:255.f);
            out[std::size_t(y)*size+x]={g,g,g,canopy::byte(opacity*255.f)};
        }
    }
}
constexpr int ReliefSize=8;
struct Relief {
    std::array<float,ReliefSize*ReliefSize> heights{};
    float low{},high{};
    bool ready{};
    void build(const Settings& s,const banks::Volume& v) {
        ready=false;
        if(v.width<ReliefSize||v.width>banks::DetailWidth||v.width%ReliefSize||!v.depth||
           v.pixels[0].empty()||v.pixels[1].empty())return;
        // Each fixed world grid vertex is the density-weighted centre of
        // a local column neighbourhood. Use the union of weather endpoints
        // to avoid changing geometry when the game switches weather.
        const int side=v.width/ReliefSize,half=side/2;
        const float base=canopy::altitude(s),band=s.maxSpawnZ-s.minSpawnZ;
        low=s.maxSpawnZ;high=s.minSpawnZ;
        std::array<std::uint64_t,ReliefSize*ReliefSize> weight{},height{};
        std::array<int,banks::DetailWidth> block{};
        for(int i=0;i<v.width;++i)block[i]=((i+half)/side)%ReliefSize;
        // Same weighted centroid, exact integer accumulation, sequential
        // traversal of the two already resident endpoint buffers.
        for(int z=0;z<v.depth;++z)for(int y=0;y<v.width;++y) {
            const auto row=(std::size_t(z)*v.width+y)*v.width;
            const int cellRow=block[y]*ReliefSize;
            for(int x=0;x<v.width;++x) {
                const auto a=std::max(v.pixels[0][row+x].a,v.pixels[1][row+x].a);
                const int i=cellRow+block[x];
                weight[i]+=a;height[i]+=unsigned(a)*(2*z+1);
            }
        }
        for(int i=0;i<ReliefSize*ReliefSize;++i) {
            const float centre=weight[i]>0?s.minSpawnZ+float(double(height[i])/(2.*weight[i]*v.depth))*band:base;
            // Keep the proxy below the nominal cloud centre where appropriate,
            // without producing tall, sharp terrain-like peaks.
            const float z=clamp(centre,base-band*.28f,base+band*.23f);
            heights[i]=z;
            low=std::min(low,z);high=std::max(high,z);
        }
        ready=true;
    }
    float at(int x,int y) const {
        return heights[std::size_t(canopy::wrap(y,ReliefSize))*ReliefSize+canopy::wrap(x,ReliefSize)];
    }
};
struct Mesh {
    std::vector<canopy::Vertex> vertices;
    std::vector<std::uint16_t> indices;
};
struct Cell {int x,y;float squared;};
// A small real-volume zone is present even on the ground. Its radius
// (not its shape or density) expands with the original player-Z handover.
inline Range volumeRange(const Settings& s,const Range& full,float playerZ,float blend) {
    if(blend>=1.f)return full;
    const float h=canopy::altitude(s);
    const float climb=smooth((playerZ-s.minSpawnZ*.4f)/std::max(1.f,h*.85f-s.minSpawnZ*.4f));
    const float nearestRange=clamp((s.maxSpawnZ-s.minSpawnZ)*1.4f,600.f,900.f);
    const float local=std::min(full.end,mix(nearestRange,1200.f,climb));
    const float end=mix(local,full.end,blend);
    return {end,mix(local*.72f,full.begin,blend),0.f,end,end};
}
// One continuous, wind-anchored relief sheet; source triangles are capped
// by a bounded grid, with invisible cells culled before emitting vertices.
// No extra overlay/fill pass. Unlike a camera-recentred relief mesh,
// the same absolute XY node always has the same height and UV.
inline void mesh(Mesh& out,const Relief& relief,const Settings& s,const View& v,const Range& r,
                 const canopy::Drift& drift,float wx,float wy,float blend,const Range* volume=nullptr) {
    out.vertices.clear();out.indices.clear();
    if(!relief.ready||r.end<=1.f||s.opacity<=0.f||!s.targetProcClouds||!finite(v.pos)||
       !std::isfinite(wx)||!std::isfinite(wy)||wx<=0||wy<=0)return;
    const float tile=canopy::tileWorldSize(s),unit=tile/ReliefSize;
    // Actual volume supplies the nearby detail; the distant relief proxy
    // needs only the coarser world grid. Devices without volume keep finer.
    int stride=volume?2:1;
    // Keep a hard worst-case source grid size even with extreme user INIs.
    while(r.end/(unit*stride)>30.f)stride*=2;
    if(stride>ReliefSize) {
        // Extremely tiny user-selected cloud scale: retain the bounded
        // original radial sheet with the same volume-derived texture.
        const auto flat=canopy::mesh(s,v,r,drift,blend);
        out.vertices.assign(flat.vertices.begin(),flat.vertices.end());
        out.indices.assign(flat.indices.begin(),flat.indices.end());
        if(volume)for(auto& v:out.vertices)
            v.alpha*=mix(1.f-banks::rangeWeight(*volume,v.distance),1.f,blend);
        return;
    }
    const float step=unit*stride;
    const double ex=double(v.pos.x)/step-drift.x*ReliefSize/stride,
                 ey=double(v.pos.y)/step-drift.y*ReliefSize/stride;
    const int ix=int(std::floor(ex)),iy=int(std::floor(ey)),span=int(std::ceil(r.end/step))+1;
    const int x0=ix-span,y0=iy-span,width=span*2+2;
    const float end2=r.end*r.end,fade=std::max(70.f,s.nearFadeDistance+55.f);
    static thread_local std::vector<Cell> cells;
    static thread_local std::vector<std::uint16_t> indices;
    cells.clear();indices.assign(std::size_t(width)*width,std::numeric_limits<std::uint16_t>::max());
    const std::array<Vec,5> planes{v.forward,v.forward*wx+v.right,v.forward*wx-v.right,
        v.forward*wy+v.up,v.forward*wy-v.up};
    std::array<float,5> ext{};
    for(int p=0;p<5;++p)ext[p]=(.5f*step)*(std::abs(planes[p].x)+std::abs(planes[p].y))
        +(relief.high-relief.low)*.5f*std::abs(planes[p].z);
    const float centerZ=(relief.low+relief.high)*.5f-v.pos.z;
    // During full-volume flight an underlay survives only at large distance.
    const float inner=blend>=1.f?r.end*.58f:volume?
        std::min(volume->begin,r.end*.58f):0.f;
    const float inner2=inner*inner;
    const float maxZ=std::max(std::abs(relief.low-v.pos.z),std::abs(relief.high-v.pos.z));
    for(int y=y0;y<iy+span;++y) {
        const float dy=float((y+.5-ey)*step),ny=std::max(0.f,std::abs(dy)-step*.5f);
        if(ny*ny>=end2)continue;
        for(int x=x0;x<ix+span;++x) {
            const float dx=float((x+.5-ex)*step),nx=std::max(0.f,std::abs(dx)-step*.5f);
            if(nx*nx+ny*ny>=end2)continue;
            const float maxX=std::abs(dx)+step*.5f,maxY=std::abs(dy)+step*.5f;
            if(maxX*maxX+maxY*maxY+maxZ*maxZ<=inner2)continue;
            const Vec centre{dx,dy,centerZ};
            bool visible=true;
            for(int p=0;p<5;++p)if(dot(centre,planes[p])+ext[p]<-.02f){visible=false;break;}
            if(visible)cells.push_back({x,y,dot(centre,centre)});
        }
    }
    // Relief may overlap along grazing rays; keep approximate far-first order
    // independent of camera yaw, rather than alpha-blending arbitrary grid order.
    std::sort(cells.begin(),cells.end(),[](const Cell& a,const Cell& b) {
        return a.squared!=b.squared?a.squared>b.squared:a.y!=b.y?a.y<b.y:a.x<b.x;
    });
    const int uBase=int(std::floor(ex*stride/ReliefSize))*ReliefSize,
              vBase=int(std::floor(ey*stride/ReliefSize))*ReliefSize;
    const auto vertex=[&](int x,int y) {
        auto& index=indices[std::size_t(y-y0)*width+x-x0];
        if(index!=std::numeric_limits<std::uint16_t>::max())return index;
        const Vec p{float(double(v.pos.x)+(x-ex)*step),float(double(v.pos.y)+(y-ey)*step),relief.at(x*stride,y*stride)};
        const float d=length(p-v.pos);
        const float radial=smooth((1.f-d/r.end)/.18f)*distanceAlpha(r,d);
        const float originalUnderlay=.22f*smooth((d-r.end*.58f)/(r.end*.29f));
        const float handover=mix(volume?1.f-banks::rangeWeight(*volume,d):1.f,originalUnderlay,blend);
        const float alpha=smooth(d/fade)*radial*handover;
        index=std::uint16_t(out.vertices.size());
        out.vertices.push_back({p,float(x*stride-uBase)/ReliefSize,float(y*stride-vBase)/ReliefSize,alpha,d});
        return index;
    };
    for(const auto& c:cells) {
        const auto a=vertex(c.x,c.y),b=vertex(c.x+1,c.y),d=vertex(c.x,c.y+1),e=vertex(c.x+1,c.y+1);
        out.indices.insert(out.indices.end(),{a,d,b,b,d,e});
    }
}
} // namespace vc::canopyVolume
