#pragma once
#include "Canopy.hpp"
#include <type_traits>

// Continuous world-space density sampled on fixed world-axis lattices.
// Camera motion never translates or rotates the sampling planes.
namespace vc::banks {
constexpr int Width=128, Height=128, Depth=32;
constexpr int DetailWidth=256,DetailDepth=32;
// Fixed nested lattices; finer planes fade in rather than moving the
// coarser samples. Bricks bound overdraw and keep transparent ordering local.
constexpr std::size_t MaxVertices=44000;
constexpr int BrickSteps=32;
constexpr int OccupancyXY=16,OccupancyZ=8;
// Twice-periodic XY prefix sums: constant-time conservative AABB density
// rejection even across the seam of the tiled volume. Max sum 8192.
constexpr int OccupancySpan=2*OccupancyXY+1;
constexpr std::size_t OccupancyPrefixCount=OccupancySpan*OccupancySpan*(OccupancyZ+1);
constexpr float ReferenceStep=90.f; // opacity baked for this path length
struct Vertex {Vec pos;float u{},v{},w{},alpha{},fog{1.f};};
struct Mesh {std::vector<Vertex> vertices;std::vector<std::uint16_t> indices;int slices{};};
// Four corner positions and lattice attributes, no per-frame CPU lighting,
// UV, fog, opacity or projection. A VS2 program computes the same equations.
// A universally supported D3DCOLOR input carries one-hot RGB axis flags
// and a 3-level alpha parity code. Only 16 bytes per streamed vertex.
struct GpuVertex {Vec pos;std::uint32_t lattice;};
static_assert(sizeof(GpuVertex)==16);
inline std::uint32_t latticeCode(int axis,int i) {
    const std::uint32_t parity=i%4==0?255u:i%2==0?128u:0u;
    return (parity<<24)|(255u<<((2-axis)*8));
}
// Each contiguous source span belongs to one world brick. Bounds enclose
// every vertex in the span, even if some patches are omitted by occupancy.
struct VertexSpan {Vec lo,hi;std::uint32_t begin,end;};
struct VertexRange {
    std::uint32_t begin{},end{};
    std::uint32_t count() const{return end-begin;}
};
struct ClipPlane {
    // A positive homogeneous clip distance is visible; error includes
    // absolute terms of the original shader dot products, not just their
    // potentially cancelling difference.
    std::array<double,4> coefficient,absolute;
    bool mayIntersect(const VertexSpan& span) const {
        double maximum=coefficient[3],magnitude=absolute[3];
        const float lo[]={span.lo.x,span.lo.y,span.lo.z},hi[]={span.hi.x,span.hi.y,span.hi.z};
        for(int i=0;i<3;++i) {
            maximum+=coefficient[i]*(coefficient[i]>=0?hi[i]:lo[i]);
            magnitude+=absolute[i]*std::max(std::abs(lo[i]),std::abs(hi[i]));
        }
        // Leave ambiguous/boundary spans to hardware clipping. Cost of a
        // false positive is extra vertices; false negatives are unacceptable.
        return !std::isfinite(maximum)||!std::isfinite(magnitude)||maximum>=-(.001+1.e-5*magnitude);
    }
};
struct GpuMesh {
    std::vector<GpuVertex> vertices;
    std::vector<VertexSpan> spans;
    int slices{};
    std::size_t triangleCount() const{return std::size_t(slices)*2;}
    VertexRange rangeFor(const ClipPlane& plane) const {
        const auto count=std::uint32_t(vertices.size());
        if(spans.empty()||spans.front().begin!=0||spans.back().end!=count)return {0,count};
        // One contiguous range per pass, no draw-call multiplication or
        // reordering. Interior irrelevant spans remain harmlessly clipped.
        auto first=spans.begin(),last=spans.end();
        while(first!=last&&!plane.mayIntersect(*first))++first;
        if(first==last)return {};
        do{--last;}while(last!=first&&!plane.mayIntersect(*last));
        return {first->begin,last->end};
    }
};
inline int verticalSteps(const Settings& s) {
    const float base=s.cumulusDetail?16.f:20.f;
    return std::max(4,int(std::ceil((s.maxSpawnZ-s.minSpawnZ)/(base*4.f)))*4);
}
inline Vec latticeSpacing(const Settings& s) {
    const float base=s.cumulusDetail?16.f:20.f;
    return {base,base,(s.maxSpawnZ-s.minSpawnZ)/verticalSteps(s)};
}
struct TextureOrigin {float u,v,tile,band;};
inline TextureOrigin textureOrigin(const Settings& s,const View& v,const canopy::Drift& drift) {
    const float tile=canopy::tileWorldSize(s);
    return {float(drift.x+std::floor(double(v.pos.x)/tile-drift.x)),
        float(drift.y+std::floor(double(v.pos.y)/tile-drift.y)),tile,s.maxSpawnZ-s.minSpawnZ};
}
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
    std::array<std::array<std::uint16_t,OccupancyPrefixCount>,2> occupiedPrefix{};
    static std::size_t prefixIndex(int x,int y,int z) {
        return (std::size_t(z)*OccupancySpan+y)*OccupancySpan+x;
    }
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
            // Correlated strong coverage has deeper crowns, while the
            // low-coverage formations retain the smaller-scale silhouette.
            const float ceiling=base+.035f+.46f*tower+.18f*(n1-.5f)+.060f*(n2-.5f);
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
        for(int layer=0;layer<2;++layer) {
            auto& p=occupiedPrefix[layer];p.fill(0);
            const auto at=[&](int x,int y,int z){return int(p[prefixIndex(x,y,z)]);};
            for(int z=1;z<=OccupancyZ;++z)for(int y=1;y<OccupancySpan;++y)for(int x=1;x<OccupancySpan;++x) {
                const bool occupiedTexel=occupied[layer][((z-1)*OccupancyXY+(y-1)%OccupancyXY)*OccupancyXY+(x-1)%OccupancyXY]>0;
                p[prefixIndex(x,y,z)]=std::uint16_t(int(occupiedTexel)+at(x-1,y,z)+at(x,y-1,z)+at(x,y,z-1)
                    -at(x-1,y-1,z)-at(x-1,y,z-1)-at(x,y-1,z-1)+at(x-1,y-1,z-1));
            }
        }
    }
    struct CellRange {int first,last;}; // half-open, wrapped XY / clamped Z
    struct CellBox {std::array<CellRange,3> axis;};
    static CellRange cells(float lo,float hi,int axis) {
        const int n=axis==2?OccupancyZ:OccupancyXY;
        int a=int(std::floor(lo*n)),b=int(std::floor(hi*n));
        if(axis==2)return {std::clamp(a,0,n-1),std::clamp(b,0,n-1)+1};
        const int count=std::min(b-a+1,n);
        a=canopy::wrap(a,n);return {a,a+count};
    }
    bool potentiallyOccupied(const CellBox& box,float weather) const {
        const auto [xa,xb]=box.axis[0];
        const auto [ya,yb]=box.axis[1];
        const auto [za,zb]=box.axis[2];
        const auto count=[&](int layer) {
            const auto& p=occupiedPrefix[layer];
            const auto at=[&](int x,int y,int z){return int(p[prefixIndex(x,y,z)]);};
            const auto area=[&](int z){return at(xb,yb,z)-at(xa,yb,z)-at(xb,ya,z)+at(xa,ya,z);};
            return area(zb)-area(za);
        };
        return (weather<.999f&&count(0)>0)||(weather>.001f&&count(1)>0);
    }
    bool potentiallyOccupied(float u0,float v0,float w0,float u1,float v1,float w1,float weather) const {
        if(w1<0.f||w0>1.f)return false;
        return potentiallyOccupied({{cells(u0,u1,0),cells(v0,v1,1),cells(w0,w1,2)}},weather);
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
// Every ray uses its dominant world axis, with a continuous optical
// weight split near diagonals. No camera-facing shells, and no hard axis
// switch when the eye crosses the diagonal. The planes themselves are
// immutable in world space, including during translation.
inline float component(Vec v,int a) {return a==0?v.x:a==1?v.y:v.z;}
inline void setComponent(Vec& v,int a,float x) {if(a==0)v.x=x;else if(a==1)v.y=x;else v.z=x;}
inline Vec axisWeights(Vec delta) {
    const float x=std::abs(delta.x),y=std::abs(delta.y),z=std::abs(delta.z);
    const float m=std::max({x,y,z, .001f});
    Vec w{smooth((x/m-.80f)/.20f),smooth((y/m-.80f)/.20f),smooth((z/m-.80f)/.20f)};
    const float sum=w.x+w.y+w.z;return sum>0.f?w*(1.f/sum):Vec{};
}
inline float planeWeight(int i,float step,float d) {
    const float fine=1.f-smooth((d-140.f)/300.f);
    const float coarse=smooth((d-620.f)/680.f);
    // Sum of fine/middle/coarse optical path lengths is constant even
    // during LOD blends. Coarse planes are an exact subset of fine ones.
    return step*(fine+(i%2==0?2.f*(1.f-fine)*(1.f-coarse):0.f)+
        (i%4==0?4.f*(1.f-fine)*coarse:0.f));
}
struct Brick {
    std::array<int,3> first,last; // half-open world lattice intervals
    Vec lo,hi;
    float nearest{}; // squared radial bound
    unsigned axes{}; // conservative eligible plane-axis mask
};
inline Brick bounds(std::array<int,3> a,std::array<int,3> b,Vec step,Vec origin,Vec eye) {
    Brick out{a,b,{},{},0};
    for(int axis=0;axis<3;++axis) {
        const float l=component(origin,axis)+a[axis]*component(step,axis),
            h=component(origin,axis)+b[axis]*component(step,axis),e=component(eye,axis);
        setComponent(out.lo,axis,l);setComponent(out.hi,axis,h);
        const float n=std::max({l-e,e-h,0.f});
        out.nearest+=n*n;
    }
    return out;
}
inline bool visible(const Brick& b,const View& v,float wx,float wy) {
    const Vec center=(b.lo+b.hi)*.5f-v.pos,half=(b.hi-b.lo)*.5f;
    const auto outside=[&](Vec n) {
        return dot(center,n)+std::abs(n.x)*half.x+std::abs(n.y)*half.y+std::abs(n.z)*half.z<-.02f;
    };
    return !outside(v.forward)&&!outside(v.forward*wx+v.right)&&!outside(v.forward*wx-v.right)
        &&!outside(v.forward*wy+v.up)&&!outside(v.forward*wy-v.up);
}
inline unsigned axisMask(const Brick& b,Vec eye) {
    const Vec lo=b.lo-eye,hi=b.hi-eye;
    const Vec minimum{std::max({lo.x,-hi.x,0.f}),std::max({lo.y,-hi.y,0.f}),std::max({lo.z,-hi.z,0.f})};
    const Vec maximum{std::max(std::abs(lo.x),std::abs(hi.x)),std::max(std::abs(lo.y),std::abs(hi.y)),std::max(std::abs(lo.z),std::abs(hi.z))};
    return unsigned(maximum.x>=std::max(minimum.y,minimum.z)*.80f)
        |(unsigned(maximum.y>=std::max(minimum.x,minimum.z)*.80f)<<1)
        |(unsigned(maximum.z>=std::max(minimum.x,minimum.y)*.80f)<<2);
}
// The nearest corner minimizes both rejection metrics. For the rare
// corner inside the 8m interior exclusion, retain the original predicate
// for the other corners. Squared distance keeps the original XYZ sum.
inline Vec closestCornerDelta(Vec lo,Vec hi) {
    return {std::abs(lo.x)<=std::abs(hi.x)?lo.x:hi.x,
        std::abs(lo.y)<=std::abs(hi.y)?lo.y:hi.y,
        std::abs(lo.z)<=std::abs(hi.z)?lo.z:hi.z};
}
template<int a>
inline float angularThreshold(Vec closest) {
    constexpr int j=(a+1)%3,k=(a+2)%3;
    return .799999f*std::max({std::abs(component(closest,j)),std::abs(component(closest,k)),.001f});
}
template<int a>
inline bool planeCandidate(Vec closest,Vec deltaLo,Vec deltaHi,float axisDelta,float minAngularDistance,float limitSq) {
    constexpr int j=(a+1)%3,k=(a+2)%3;
    const float axisDistance=std::abs(axisDelta);
    // Comparing axisDistance to .799999*itself is always true.
    if(axisDistance<minAngularDistance)return false;
    setComponent(closest,a,axisDelta);
    const float d2=dot(closest,closest);
    if(d2>limitSq)return false;
    if(d2>63.98f)return true;
    for(int q=0;q<4;++q) {
        Vec p=deltaLo;setComponent(p,a,axisDelta);
        if(q&1)setComponent(p,j,component(deltaHi,j));
        if(q&2)setComponent(p,k,component(deltaHi,k));
        const float maxDistance=std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z),.001f});
        const float q2=dot(p,p);
        if(axisDistance>=.799999f*maxDistance&&q2<=limitSq&&q2>63.98f)return true;
    }
    return false;
}
template<class MeshType>
inline void buildMesh(MeshType& m,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,float wx,float wy,const Volume* volume=nullptr,
                      float weather=0.f,bool billboardAvailable=true) {
    using OutputVertex=typename decltype(m.vertices)::value_type;
    constexpr bool gpu=std::is_same_v<OutputVertex,GpuVertex>;
    m.vertices.clear();if constexpr(gpu)m.spans.clear();else m.indices.clear();m.slices=0;
    const float band=s.maxSpawnZ-s.minSpawnZ;
    if(!s.hybridClouds||!s.targetProcClouds||r.end<5.f||band<1.f||
       !std::isfinite(wx)||!std::isfinite(wy)||wx<=0||wy<=0||wx>10.f||wy>10.f)return;
    const auto levels=lod(s,r,billboardAvailable,view.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
    if(view.pos.z<s.minSpawnZ-levels.end||view.pos.z>s.maxSpawnZ+levels.end)return;
    const float base=s.cumulusDetail?16.f:20.f;
    const int nz=verticalSteps(s);
    const Vec step=latticeSpacing(s),origin{0,0,s.minSpawnZ};
    const float brickSize=BrickSteps*base;
    const auto radiusSquared=[](float distance){const float limit=distance+.01f;return limit*limit;};
    const float outerLimitSq=radiusSquared(levels.end),middleLimitSq=radiusSquared(std::min(levels.end,1300.f)),
        innerLimitSq=radiusSquared(std::min(levels.end,440.f));
    const auto tex=textureOrigin(s,view,drift);
    const float tile=tex.tile,uOffset=tex.u,vOffset=tex.v;
    const auto occupied=[&](Vec lo,Vec hi) {
        return !volume||volume->potentiallyOccupied(lo.x/tile-uOffset,lo.y/tile-vOffset,(lo.z-origin.z)/band,
            hi.x/tile-uOffset,hi.y/tile-vOffset,(hi.z-origin.z)/band,weather);
    };
    const std::array<Vec,5> planes{view.forward,view.forward*wx+view.right,view.forward*wx-view.right,
        view.forward*wy+view.up,view.forward*wy-view.up};
    std::array<Vec,5> absPlanes{};
    for(int i=0;i<5;++i)absPlanes[i]={std::abs(planes[i].x),std::abs(planes[i].y),std::abs(planes[i].z)};
    const auto inView=[&](const Brick& b){
        const Vec center=(b.lo+b.hi)*.5f-view.pos,half=(b.hi-b.lo)*.5f;
        for(int i=0;i<5;++i)if(dot(center,planes[i])+dot(half,absPlanes[i])<-.02f)return false;
        return true;
    };
    // Reuse scratch allocations on the render thread, including on the
    // legacy backend. No per-frame allocation after the peak capacity.
    static thread_local std::vector<Brick> bricks,parents;
    bricks.clear();parents.clear();
    if(bricks.capacity()<512)bricks.reserve(512);
    if(parents.capacity()<384)parents.reserve(384);
    const auto closer=[](const Brick& a,const Brick& b) {
        return a.nearest!=b.nearest?a.nearest<b.nearest:a.first<b.first;
    };
    // Split only nearby bricks: identical planes and lattice phase on both
    // sides of subdivision. The smaller patches resolve angular weighting
    // and fog accurately where they subtend a large screen area.
    const auto collect=[&](auto&& self,Brick b,bool pretested)->void {
        if((!pretested&&(b.nearest>=levels.end*levels.end||!inView(b)))||!occupied(b.lo,b.hi))return;
        const int n=std::max({b.last[0]-b.first[0],b.last[1]-b.first[1],b.last[2]-b.first[2]});
        b.axes=axisMask(b,view.pos);
        // Diagonal views need finer patches so alpha interpolation doesn't
        // reveal an axis boundary. The dominant-axis interior is cheaper.
        const float splitFactor=(b.axes&(b.axes-1))?2.25f:.85f;
        if(n>4 && b.nearest < float(n*n)*base*base*splitFactor) {
            std::array<Brick,8> children{};int count=0;
            std::array<int,3> mid{};
            for(int a=0;a<3;++a)mid[a]=(b.first[a]+b.last[a])/2;
            for(int mask=0;mask<8;++mask) {
                auto lo=b.first,hi=b.last;
                for(int a=0;a<3;++a)if(mask&(1<<a))lo[a]=mid[a];else hi[a]=mid[a];
                if(lo[0]>=hi[0]||lo[1]>=hi[1]||lo[2]>=hi[2])continue;
                children[count++]=bounds(lo,hi,step,origin,view.pos);
            }
            // Finish each cell before another parent. Parent/sibling sort
            // preserves ordering for shared rays through unequal-sized cells.
            for(int j=1;j<count;++j)for(int k=j;k>0&&closer(children[k],children[k-1]);--k)
                std::swap(children[k],children[k-1]);
            for(int j=0;j<count;++j)self(self,children[j],false);
        } else bricks.push_back(b);
    };
    const int x0=int(std::floor((view.pos.x-levels.end)/brickSize)),x1=int(std::floor((view.pos.x+levels.end)/brickSize));
    const int y0=int(std::floor((view.pos.y-levels.end)/brickSize)),y1=int(std::floor((view.pos.y+levels.end)/brickSize));
    // Root grid intervals are shared by every vertical brick in a column.
    // Skip entire XY columns outside the radial bound and compute their
    // endpoints only once. Keep the same XYZ accumulation and sort keys.
    struct RootInterval {int first,last;float lo,hi,nearestSq;};
    const auto rootInterval=[](int first,int last,float step,float origin,float eye) {
        const float lo=origin+first*step,hi=origin+last*step;
        const float n=std::max({lo-eye,eye-hi,0.f});
        return RootInterval{first,last,lo,hi,n*n};
    };
    static thread_local std::vector<RootInterval> rootZ;
    rootZ.clear();
    for(int z=0;z<nz;z+=BrickSteps/2)
        rootZ.push_back(rootInterval(z,std::min(z+BrickSteps/2,nz),step.z,origin.z,view.pos.z));
    const float endSq=levels.end*levels.end;
    for(int y=y0;y<=y1;++y) {
        const auto by=rootInterval(y*BrickSteps,(y+1)*BrickSteps,step.y,origin.y,view.pos.y);
        for(int x=x0;x<=x1;++x) {
            const auto bx=rootInterval(x*BrickSteps,(x+1)*BrickSteps,step.x,origin.x,view.pos.x);
            const float horizontalNearest=bx.nearestSq+by.nearestSq;
            if(horizontalNearest>=endSq)continue;
            for(const auto& bz:rootZ) {
                const float nearest=horizontalNearest+bz.nearestSq;
                if(nearest>=endSq)continue;
                const Brick b{{bx.first,by.first,bz.first},{bx.last,by.last,bz.last},
                    {bx.lo,by.lo,bz.lo},{bx.hi,by.hi,bz.hi},nearest};
                if(inView(b))parents.push_back(b);
            }
        }
    }
    // Sibling grid boxes along a shared ray have a monotonic nearest bound.
    // Sorting siblings recursively keeps whole spatial cells in order.
    // Only intersecting planes inside the diagonal crossfade are approximate.
    std::sort(parents.begin(),parents.end(),closer);
    for(const auto& b:parents)collect(collect,b,true);
    // A patch's rectangle is entirely defined by its parent brick, axis
    // and plane position. Sort compact keys directly; no pointer chasing.
    struct Patch {float depth,position;int axis,plane;};
    std::array<Patch,3*BrickSteps> patches;
    for(const auto& b:bricks) {
        std::size_t patchCount=0;
        const auto spanStart=std::uint32_t(m.vertices.size());
        const int stride=b.nearest>=1300.f*1300.f?4:b.nearest>=440.f*440.f?2:1;
        // Two axes of the occupancy box are constant across every slice.
        // Compute them once, respecting the same division and floor as the
        // original query; only the sliced axis changes inside the loop.
        const auto uvCoord=[&](float p,int a){
            return a==0?p/tile-uOffset:a==1?p/tile-vOffset:(p-origin.z)/band;
        };
        Volume::CellBox box{};
        if(volume)for(int a=0;a<3;++a)
            box.axis[a]=Volume::cells(uvCoord(component(b.lo,a),a),uvCoord(component(b.hi,a),a),a);
        // Independent closest corner coordinates minimize both squared distance
        // and the dominant-axis threshold for every plane in this brick.
        const Vec deltaLo=b.lo-view.pos,deltaHi=b.hi-view.pos;
        const Vec closest=closestCornerDelta(deltaLo,deltaHi);
        const Vec center=(b.lo+b.hi)*.5f-view.pos;
        const auto collectAxis=[&](auto axis) {
            constexpr int a=decltype(axis)::value;
            if(!(b.axes&(1u<<a)))return;
            const float eyeAxis=component(view.pos,a),axisOrigin=component(origin,a),axisStep=component(step,a);
            const float minAngularDistance=angularThreshold<a>(closest);
            int i=b.first[a];const int rem=((i%stride)+stride)%stride;if(rem)i+=stride-rem;
            // Adjacent world planes often land in the same baked occupancy cell.
            // Other dimensions and active weather endpoints are constant here.
            int lastCell=-1;bool lastOccupied=false;
            for(;i<b.last[a];i+=stride) {
                // A single lattice plane at the center of its finest cell.
                // Neither its position nor the density UV depends on the eye.
                const float plane=axisOrigin+(i+.5f)*axisStep;
                if(volume) {
                    const float uv=uvCoord(plane,a);
                    if(a==2&&(uv<0.f||uv>1.f))continue;
                    const auto cell=Volume::cells(uv,uv,a);
                    if(lastCell!=cell.first) {
                        auto query=box;query.axis[a]=cell;
                        lastOccupied=volume->potentiallyOccupied(query,weather);
                        lastCell=cell.first;
                    }
                    if(!lastOccupied)continue;
                }
                const float limitSq=i%4==0?outerLimitSq:i%2==0?middleLimitSq:innerLimitSq;
                if(planeCandidate<a>(closest,deltaLo,deltaHi,plane-eyeAxis,minAngularDistance,limitSq)) {
                    Vec middle=center;
                    // Retain the original (lo+hi)*.5 expression for exact depth ordering.
                    setComponent(middle,a,(plane+plane)*.5f-eyeAxis);
                    patches[patchCount++]={dot(middle,middle),plane,a,i};
                }
            }
        };
        collectAxis(std::integral_constant<int,0>{});
        collectAxis(std::integral_constant<int,1>{});
        collectAxis(std::integral_constant<int,2>{});
        std::sort(patches.begin(),patches.begin()+patchCount,[](const Patch& a,const Patch& b){
            return a.depth!=b.depth?a.depth<b.depth:a.axis!=b.axis?a.axis<b.axis:a.plane<b.plane;
        });
        for(std::size_t index=0;index<patchCount;++index) {
            const auto& patch=patches[index];
            if(m.vertices.size()+4>MaxVertices)break;
            const auto offset=std::uint16_t(m.vertices.size());
            const int a=patch.axis,j=(a+1)%3,k=(a+2)%3,i=patch.plane;
            const auto code=latticeCode(a,i);
            Vec lower=b.lo,upper=b.hi;setComponent(lower,a,patch.position);setComponent(upper,a,patch.position);
            for(int q=0;q<4;++q) {
                Vec p=lower;
                if(q&1)setComponent(p,j,component(upper,j));
                if(q&2)setComponent(p,k,component(upper,k));
                if constexpr(gpu) {
                    m.vertices.push_back({p,code});
                } else {
                    const Vec delta=p-view.pos;
                    const float d=length(delta),w=component(axisWeights(delta),a);
                    const float cosine=std::abs(component(delta,a))/std::max(d,.001f);
                    const float path=planeWeight(i,component(step,a),d)/std::max(cosine,.4f);
                    const float nearWeight=smooth((d-8.f)/24.f)*mix(smooth((d-view.nearClip)/std::max(80.f,s.nearFadeDistance)),1.f,s.interiorHaze*.22f);
                    const float alpha=sat(path*w/ReferenceStep)*rangeWeight(r,d)*levels.volume(d)*nearWeight;
                    m.vertices.push_back({p,p.x/tile-uOffset,p.y/tile-vOffset,(p.z-origin.z)/band,alpha,cloudFogFactor(s,d)});
                }
            }
            // The hardware path already owns a static reversed index
            // buffer, so there is no per-frame index allocation or fill.
            if constexpr(!gpu)m.indices.insert(m.indices.end(),{offset,std::uint16_t(offset+1),std::uint16_t(offset+2),
                std::uint16_t(offset+1),std::uint16_t(offset+3),std::uint16_t(offset+2)});
            ++m.slices;
        }
        if constexpr(gpu)if(m.vertices.size()>spanStart)
            m.spans.push_back({b.lo,b.hi,spanStart,std::uint32_t(m.vertices.size())});
        if(m.vertices.size()+4>MaxVertices)break;
    }
    if constexpr(!gpu)std::reverse(m.indices.begin(),m.indices.end()); // far first, culling disabled
}
} // namespace vc::banks
