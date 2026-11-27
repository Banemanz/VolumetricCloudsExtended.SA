#pragma once
#include "Canopy.hpp"

// Continuous world-space density sampled by camera-position-centred,
// world-axis tessellated shells. No view-dependent sampling orientation.
namespace vc::banks {
constexpr int Width=128, Height=128, Depth=32;
constexpr int DetailWidth=256,DetailDepth=32;
// Sample at fixed radial distances, independent of look direction, FOV,
// scene far plane, and configured cloud range. For a fixed eye and world ray,
// the tessellated surface and every sampled XYZ remain identical under a turn.
constexpr int MaxSlices=156,DetailSlices=200,ShellGrid=12;
constexpr std::size_t ShellVertexCount=6*(ShellGrid+1)*(ShellGrid+1);
constexpr int OccupancyXY=16,OccupancyZ=8;
constexpr float ReferenceStep=90.f; // opacity baked for this path length
struct Vertex {Vec pos;float u{},v{},w{},alpha{},fog{1.f};};
struct Mesh {std::vector<Vertex> vertices;std::vector<std::uint16_t> indices;int slices{};};
struct Lod {
    float start{},end{};
    float billboard(float d) const {return end>start?smooth((d-start)/(end-start)):0.f;}
    float volume(float d) const {return 1.f-billboard(d);}
};
inline Lod lod(const Settings& s,const Range& r,bool available=true,float altitudeOffset=0.f) {
    if(!s.farBillboards||!available)return {r.end,r.end};
    // Upright impostors are progressively deferred when viewed steeply
    // from high altitude; preserve volume for those grazing the top.
    const float start=std::max({s.billboardStartDistance,r.end*s.billboardStartRatio,std::abs(altitudeOffset)*3.f});
    if(start>=r.end-1.f)return {r.end,r.end};
    return {start,std::min(r.end,start+std::max(350.f,r.end*.10f))};
}
inline float radius(const Settings& s,const Range& r) {return lod(s,r).end;}
// Long optical paths accumulate many surfaces; a squared edge taper makes
// the distant bank gradually dissolve instead of terminating in a bright ring.
inline float rangeWeight(const Range& r,float d) {const float f=distanceAlpha(r,d);return f*f;}
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
            const float edgeDetail=(n3-.5f)*(.095f+.055f*(1.f-tower));
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
                    const float ambient=.31f*std::exp(-.13f*(topDepth+sideDepth))+.69f*trans;
                    const float slope=clamp((at(lateralStep,0,0)-at(-lateralStep,0,0)
                        +at(0,lateralStep,0)-at(0,-lateralStep,0))*.5f,-1.f,1.f);
                    const float localRelief=clamp(d-(at(1,0,0)+at(-1,0,0)+at(0,1,0)+at(0,-1,0))*.25f,-.35f,.35f);
                    const float litRim=(1.f-d)*trans;
                    const float crownGlow=.055f*trans*smooth((float(z)/depth-base+.09f)/.28f);
                    const float shade=clamp(.995f+crownGlow-s.cumulusContrast*(.64f*(1.f-ambient)+.15f*d+.23f*slope+.27f*localRelief)
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
// One immutable cube-sphere triangulation. At fixed eye position the
// triangles, world-space UVW, alpha and fog are identical during rotation.
// Frustum culling merely decides which triangles need drawing.
struct ShellTopology {
    std::vector<Vec> directions;
    struct Triangle {std::array<std::uint16_t,3> i;float z0,z1;};
    std::vector<Triangle> triangles;
    explicit ShellTopology(int grid) {
        for(int face=0;face<6;++face) {
            const auto base=std::uint16_t(directions.size());
            for(int y=0;y<=grid;++y)for(int x=0;x<=grid;++x) {
                const float u=2.f*x/grid-1.f,v=2.f*y/grid-1.f;
                const float sign=face%2?1.f:-1.f;
                Vec d=face<2?Vec{sign,u,v}:face<4?Vec{u,sign,v}:Vec{u,v,sign};
                directions.push_back(unit(d));
            }
            const auto tri=[&](std::uint16_t a,std::uint16_t b,std::uint16_t c) {
                const float x=directions[a].z,y=directions[b].z,z=directions[c].z;
                triangles.push_back({{a,b,c},std::min({x,y,z}),std::max({x,y,z})});
            };
            for(int y=0;y<grid;++y)for(int x=0;x<grid;++x) {
                const auto a=std::uint16_t(base+y*(grid+1)+x);
                const auto b=std::uint16_t(a+1),c=std::uint16_t(a+grid+1),d=std::uint16_t(c+1);
                tri(a,b,c);tri(b,d,c);
            }
        }
    }
};
inline const ShellTopology& topology(bool coarse) {static const ShellTopology fine(ShellGrid),nearShell(6);return coarse?nearShell:fine;}
struct RadialSample {float distance,weight;};
inline const std::vector<RadialSample>& samples(bool detail) {
    static const auto make=[](bool high) {
        std::vector<RadialSample> result;
        for(float edge=0.f;edge<12000.f;) {
            const float step=edge<160.f?(high?12.f:16.f):edge<520.f?(high?24.f:32.f):
                edge<1400.f?(high?48.f:64.f):(high?72.f:90.f);
            result.push_back({edge+step*.5f,step});edge+=step;
        }
        return result;
    };
    static const auto low=make(false),high=make(true);
    return detail?high:low;
}
inline Vertex interpolate(const Vertex& a,const Vertex& b,float t) {
    return {a.pos+(b.pos-a.pos)*t,mix(a.u,b.u,t),mix(a.v,b.v,t),mix(a.w,b.w,t),
        mix(a.alpha,b.alpha,t),mix(a.fog,b.fog,t)};
}
inline unsigned directionMask(Vec d,const View& view,float wx,float wy) {
    const float z=dot(d,view.forward),x=dot(d,view.right),y=dot(d,view.up);
    const float e=.0001f;
    return (z<0.f?1u:0u)|(x < -wx*z-e?2u:0u)|(x> wx*z+e?4u:0u)|
        (y < -wy*z-e?8u:0u)|(y> wy*z+e?16u:0u);
}
inline void buildMesh(Mesh& m,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,float wx,float wy,const Volume* volume=nullptr,
                      float weather=0.f,bool billboardAvailable=true) {
    m.vertices.clear();m.indices.clear();m.slices=0;
    const float band=s.maxSpawnZ-s.minSpawnZ;
    if(!s.hybridClouds||!s.targetProcClouds||r.end<5.f||band<1.f||
       !std::isfinite(wx)||!std::isfinite(wy)||wx<=0||wy<=0||wx>10.f||wy>10.f)return;
    const auto levels=lod(s,r,billboardAvailable,view.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
    if(view.pos.z<s.minSpawnZ-levels.end||view.pos.z>s.maxSpawnZ+levels.end)return;
    const auto visibleFor=[&](const ShellTopology& shell) {
        std::array<unsigned,ShellVertexCount> masks{};
        for(std::size_t i=0;i<shell.directions.size();++i)masks[i]=directionMask(shell.directions[i],view,wx,wy);
        std::vector<const ShellTopology::Triangle*> result;
        result.reserve(shell.triangles.size()/3);
        for(const auto& tri:shell.triangles)
            if(!(masks[tri.i[0]]&masks[tri.i[1]]&masks[tri.i[2]]))result.push_back(&tri);
        return result;
    };
    const auto nearTris=visibleFor(topology(true)),farTris=visibleFor(topology(false));
    const float tile=canopy::tileWorldSize(s);
    const float u0=float(double(view.pos.x)/tile-drift.x-std::floor(double(view.pos.x)/tile-drift.x));
    const float v0=float(double(view.pos.y)/tile-drift.y-std::floor(double(view.pos.y)/tile-drift.y));
    const auto& radial=samples(s.cumulusDetail!=0);
    std::array<Vertex,ShellVertexCount> cache{};
    std::array<int,ShellVertexCount> mapped{};
    std::array<bool,ShellVertexCount> ready{};
    // The shell intersects the cloud slab in narrow arcs at long distance.
    // Reject those outside it before making vertices; clip boundary triangles
    // exactly to the slab, so distant shells do not repeatedly shade empty sky.
    for(const auto& sample:radial) {
        const float d=sample.distance;
        if(d>=levels.end||d<view.nearClip)continue;
        const float nearWeight=smooth((d-8.f)/24.f)*mix(smooth((d-view.nearClip)/std::max(80.f,s.nearFadeDistance)),1.f,s.interiorHaze*.22f);
        const float a=sat(sample.weight/ReferenceStep)*rangeWeight(r,d)*levels.volume(d)*nearWeight;
        if(a<=1.f/255.f)continue;
        const bool coarse=d<240.f;
        const auto& shell=topology(coarse);const auto& visible=coarse?nearTris:farTris;
        const float fog=cloudFogFactor(s,d);
        mapped.fill(-1);ready.fill(false);
        const auto before=m.indices.size();
        const float low=(s.minSpawnZ-view.pos.z)/d,high=(s.maxSpawnZ-view.pos.z)/d;
        for(const auto* tri:visible) {
            if(tri->z1<low||tri->z0>high)continue;
            std::array<Vertex,5> poly{},tmp{};
            for(int q=0;q<3;++q) {
                const auto i=tri->i[q];
                if(!ready[i]) {
                    const Vec delta=shell.directions[i]*d,pos=view.pos+delta;
                    cache[i]={pos,u0+delta.x/tile,v0+delta.y/tile,(pos.z-s.minSpawnZ)/band,a,fog};
                    ready[i]=true;
                }
                poly[q]=cache[i];
            }
            const bool unclipped=tri->z0>=low&&tri->z1<=high;
            int count=3;
            if(!unclipped)for(int side=0;side<2&&count>=3;++side) {
                int out=0;
                for(int j=0;j<count;++j) {
                    const auto& p=poly[j];const auto& q=poly[(j+1)%count];
                    const float dp=side?1.f-p.w:p.w,dq=side?1.f-q.w:q.w;
                    if(dp>=0.f)tmp[out++]=p;
                    if((dp<0)!=(dq<0))tmp[out++]=interpolate(p,q,dp/(dp-dq));
                }
                poly=tmp;count=out;
            }
            if(count<3)continue;
            if(volume) {
                float uMin=1e9f,vMin=1e9f,wMin=1e9f,uMax=-1e9f,vMax=-1e9f,wMax=-1e9f;
                for(int j=0;j<count;++j) {
                    const auto& p=poly[j];
                    uMin=std::min(uMin,p.u);uMax=std::max(uMax,p.u);
                    vMin=std::min(vMin,p.v);vMax=std::max(vMax,p.v);
                    wMin=std::min(wMin,p.w);wMax=std::max(wMax,p.w);
                }
                if(!volume->potentiallyOccupied(uMin,vMin,wMin,uMax,vMax,wMax,weather))continue;
            }
            // Leave room for the D3D9 near/far/frustum clipper. Wide custom
            // view windows are bounded by this guard rather than index wrap.
            if(m.vertices.size()+std::size_t(count)>44000)break;
            if(unclipped) {
                for(auto i:tri->i) {
                    if(mapped[i]<0){mapped[i]=int(m.vertices.size());m.vertices.push_back(cache[i]);}
                    m.indices.push_back(std::uint16_t(mapped[i]));
                }
            } else {
                const auto base=std::uint16_t(m.vertices.size());
                for(int j=0;j<count;++j)m.vertices.push_back(poly[j]);
                for(int j=1;j+1<count;++j)m.indices.insert(m.indices.end(),{base,std::uint16_t(base+j),std::uint16_t(base+j+1)});
            }
        }
        if(m.indices.size()!=before)++m.slices;
    }
    // Generate near first so the guarded vertex budget preserves nearby
    // density in extreme custom configurations; draw far first. Reversing
    // triangle winding is harmless (the cloud pass disables culling).
    std::reverse(m.indices.begin(),m.indices.end());
}
} // namespace vc::banks
