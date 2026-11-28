#pragma once
#include "CloudBanks.hpp"

// Directional optical impostors of the SAME periodic 3D density.
// World cells overlap with a partition of unity; both azimuth and
// elevation are preintegrated, so ground views and flight share silhouettes.
namespace vc::billboards {
constexpr int Cells=5,Directions=8,Elevations=5,TileW=32,TileH=32,AtlasSize=1024,Guard=1;
constexpr unsigned SlotCount=Cells*Cells*Directions*Elevations;
constexpr float Support=.60f,HalfWidth=.848529f; // sqrt(2)*Support
constexpr float ElevationStep=45.f*Pi/180.f;
constexpr int MaxCards=4096; // at most 16384 source vertices
static_assert(SlotCount<=AtlasSize/TileW*(AtlasSize/TileH));
inline float window(float x) {return smooth((Support-std::abs(x))/(2.f*Support-1.f));}
inline int slot(int x,int y,int angle,int elevation=Elevations/2) {
    return ((canopy::wrap(y,Cells)*Cells+canopy::wrap(x,Cells))*Directions+
        canopy::wrap(angle,Directions))*Elevations+elevation;
}
inline Vec direction(int angle,int elevation=Elevations/2) {
    const float a=2.f*Pi*float(angle)/Directions,e=(elevation-Elevations/2)*ElevationStep;
    return {std::cos(a)*std::cos(e),std::sin(a)*std::cos(e),std::sin(e)};
}
inline float projectedHalfHeight(float halfWidth,float halfBand,int elevation) {
    const float e=std::abs(elevation-Elevations/2)*ElevationStep;
    return std::cos(e)*halfBand+std::sin(e)*halfWidth;
}
struct Atlas {
    std::array<std::vector<canopy::Pixel>,2> pixels;
    std::array<std::array<bool,SlotCount>,2> occupied{};
    // Exact conservative alpha support, in source tile texels. The
    // one-texel pad includes every bilinear tap and guards rounding.
    struct Bounds {std::uint8_t x0,y0,x1,y1;};
    // Full support plus two possible two-rectangle decompositions.
    // This is a small GL-era sprite-trimming trick applied to the volume
    // impostors. The split has no overlap, so blending does not change.
    using Regions=std::array<Bounds,5>; // whole, left, right, top, bottom
    std::array<std::array<Regions,SlotCount>,2> bounds{};
    std::array<Regions,SlotCount> mixedBounds{};
    std::array<std::uint32_t,2> occupiedCells{};
    static unsigned area(Bounds b) {return unsigned(b.x1-b.x0)*unsigned(b.y1-b.y0);}
    static Bounds unite(Bounds a,Bounds b) {
        if(!a.x1)return b;
        if(!b.x1)return a;
        return {std::min(a.x0,b.x0),std::min(a.y0,b.y0),std::max(a.x1,b.x1),std::max(a.y1,b.y1)};
    }
    void buildBounds() {
        for(int layer=0;layer<2;++layer)for(unsigned i=0;i<SlotCount;++i) {
            const int ox=int(i)%(AtlasSize/TileW)*TileW,oy=int(i)/(AtlasSize/TileW)*TileH;
            auto& out=bounds[layer][i];out={};
            for(int y=Guard;y<TileH-Guard;++y)for(int x=Guard;x<TileW-Guard;++x)
                if(pixels[layer][std::size_t(oy+y)*AtlasSize+ox+x].a) {
                    const int x0=std::max(Guard,x-1),x1=std::min(TileW-Guard,x+2),
                        y0=std::max(Guard,y-1),y1=std::min(TileH-Guard,y+2);
                    const auto add=[&](int part,int l,int t,int r,int b) {
                        if(l<r&&t<b)out[part]=unite(out[part],{std::uint8_t(l),std::uint8_t(t),std::uint8_t(r),std::uint8_t(b)});
                    };
                    add(0,x0,y0,x1,y1);
                    add(1,x0,y0,std::min(x1,TileW/2),y1);
                    add(2,std::max(x0,TileW/2),y0,x1,y1);
                    add(3,x0,y0,x1,std::min(y1,TileH/2));
                    add(4,x0,std::max(y0,TileH/2),x1,y1);
                }
        }
        occupiedCells={};
        for(unsigned i=0;i<SlotCount;++i) {
            for(unsigned part=0;part<mixedBounds[i].size();++part)
                mixedBounds[i][part]=unite(bounds[0][i][part],bounds[1][i][part]);
            for(int layer=0;layer<2;++layer)if(occupied[layer][i])
                occupiedCells[layer]|=1u<<(i/(Directions*Elevations));
        }
    }
    const Regions& supportCached(int index,float weather) const {
        return weather<=.001f?bounds[0][index]:weather>=.999f?bounds[1][index]:mixedBounds[index];
    }
    std::uint32_t cellMask(float weather) const {
        return (weather<.999f?occupiedCells[0]:0u)|(weather>.001f?occupiedCells[1]:0u);
    }
    Regions support(int index,float weather) const {
        if(weather<=.001f)return bounds[0][index];
        if(weather>=.999f)return bounds[1][index];
        Regions out{};
        for(unsigned i=0;i<out.size();++i)out[i]=unite(bounds[0][index][i],bounds[1][index][i]);
        return out;
    }
    void build(const Settings& s,const banks::Volume& volume) {
        const float cell=canopy::tileWorldSize(s)/Cells,band=s.maxSpawnZ-s.minSpawnZ;
        const float width=HalfWidth*cell,halfBand=band*.5f;
        for(int weather=0;weather<2;++weather) {
            auto& tex=pixels[weather];tex.assign(AtlasSize*AtlasSize,{255,255,255,0});
            occupied[weather].fill(false);
            for(int cy=0;cy<Cells;++cy)for(int cx=0;cx<Cells;++cx) {
                // Empty world cells do not need any of their 40 projections.
                if(!volume.potentiallyOccupied((cx+.5f-Support)/Cells,(cy+.5f-Support)/Cells,0,
                    (cx+.5f+Support)/Cells,(cy+.5f+Support)/Cells,1,float(weather)))continue;
                for(int angle=0;angle<Directions;++angle)for(int elevation=0;elevation<Elevations;++elevation) {
                    const int index=slot(cx,cy,angle,elevation),ox=index%(AtlasSize/TileW)*TileW,oy=index/(AtlasSize/TileW)*TileH;
                    const Vec dir=direction(angle,elevation);
                    const float az=2.f*Pi*float(angle)/Directions;
                    const Vec right{std::sin(az),-std::cos(az),0},up=cross(right,dir);
                    const float e=std::abs(elevation-Elevations/2)*ElevationStep;
                    const float rayExtent=std::cos(e)*width+std::sin(e)*halfBand;
                    const float height=projectedHalfHeight(width,halfBand,elevation);
                    const int steps=std::clamp(int(std::ceil(2.f*rayExtent/24.f)),16,64);
                    const float step=2.f*rayExtent/steps;
                    std::array<float,256> optical{};
                    for(int i=0;i<256;++i)optical[i]=-std::log(std::max(1.f-i/255.f,.0001f))*step/banks::ReferenceStep;
                    for(int y=Guard;y<TileH-Guard;++y)for(int x=Guard;x<TileW-Guard;++x) {
                        const float offset=mix(-width,width,(x-Guard+.5f)/(TileW-2*Guard));
                        const float h=mix(height,-height,(y-Guard+.5f)/(TileH-2*Guard));
                        const Vec screen=right*offset+up*h;
                        float trans=1.f,grey=0.f;
                        for(int k=0;k<steps;++k) {
                            const Vec local=screen+dir*(-rayExtent+(k+.5f)*step);
                            const float z=.5f+local.z/band;
                            if(z<=0.f||z>=1.f)continue;
                            const float mask=window(local.x/cell)*window(local.y/cell);
                            if(mask<.001f)continue;
                            const auto p=volume.sample(weather,(cx+.5f+local.x/cell)/Cells,(cy+.5f+local.y/cell)/Cells,z);
                            if(!p.a)continue;
                            const float a=1.f-std::exp(-optical[p.a]*mask);
                            grey+=trans*a*p.r;trans*=1.f-a;
                            if(trans<.002f)break;
                        }
                        const float alpha=1.f-trans;
                        const auto shade=canopy::byte(alpha>0.f?grey/alpha:255.f),a=canopy::byte(alpha*255.f);
                        tex[std::size_t(oy+y)*AtlasSize+ox+x]={shade,shade,shade,a};
                        occupied[weather][index]|=a>0;
                    }
                }
            }
        }
        buildBounds();
    }
    bool any(int index,float weather) const {
        return (weather<.999f&&occupied[0][index])||(weather>.001f&&occupied[1][index]);
    }
};
// Three-way triangular interpolation over the angular grid, instead of
// four bilinear cards per cell. Continuous shared edges and weights sum
// to one. Smooth coordinates reduce angular changes at exact view bins.
struct AngularSample {int azimuth,elevation;float weight;};
inline std::array<AngularSample,3> angularSamples(float azimuth,float elevation) {
    const int a=int(std::floor(azimuth));
    const float e=clamp(elevation/ElevationStep+float(Elevations/2),0.f,float(Elevations-1));
    const int b=std::min(int(std::floor(e)),Elevations-2);
    // Keep a stable projection near each sampled direction; fade only
    // through the central 70% of the interval, still C1 continuous.
    const auto angular=[](float t){return smooth((t-.15f)/.70f);};
    const float x=angular(azimuth-a),y=angular(e-b);
    if(x>=y)return {{{a,b,1.f-x},{a+1,b,x-y},{a+1,b+1,y}}};
    return {{{a,b,1.f-y},{a,b+1,y-x},{a+1,b+1,x}}};
}
struct Card {Vec center,right,up;float alpha,weight,depth,fog;bool transition;int index;};
template<bool Fast=true>
inline void buildMesh(banks::Mesh& mesh,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,const Atlas& atlas,float wx,float wy,float weather,
                      const banks::Lod* sharedLod=nullptr) {
    mesh.vertices.clear();mesh.indices.clear();mesh.slices=0;
    const auto levels=sharedLod?*sharedLod:banks::lod(s,r,true,view.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
    if(levels.start>=levels.end)return;
    const float cell=canopy::tileWorldSize(s)/Cells,base=(s.minSpawnZ+s.maxSpawnZ)*.5f,halfHeight=(s.maxSpawnZ-s.minSpawnZ)*.5f;
    const float halfWidth=cell*HalfWidth,extent=std::sqrt(halfWidth*halfWidth+halfHeight*halfHeight);
    const double ex=double(view.pos.x)/cell-drift.x*Cells,ey=double(view.pos.y)/cell-drift.y*Cells;
    const int ix=int(std::floor(ex)),iy=int(std::floor(ey));
    const int span=int(std::ceil((r.end+extent)/cell));
    if(span>80)return;
    static thread_local std::vector<Card> cards;
    cards.clear();if(cards.capacity()<256)cards.reserve(256);
    // A rotated orthographic rectangle can have a larger corner radius
    // than the physical source bounds; use a conservative quad bound.
    const float maxH=std::sqrt(halfWidth*halfWidth+halfHeight*halfHeight);
    const float cardExtent=std::sqrt(halfWidth*halfWidth+std::max(maxH,halfHeight)*std::max(maxH,halfHeight));
    const float wxExtent=cardExtent*std::sqrt(1+wx*wx),wyExtent=cardExtent*std::sqrt(1+wy*wy);
    const float inner=std::max(0.f,levels.start-cardExtent-1.f),outer=r.end+1.f;
    const float inner2=inner*inner,outer2=outer*outer;
    // Row/column coordinates and the fixed elevation scale are shared
    // by every candidate. Calculate once (bit-exact original arithmetic).
    std::array<float,161> dx{};
    std::array<int,161> cellX{};
    std::array<float,Elevations> heights{};
    struct PoleBasis {Vec right,up;};
    std::array<std::array<PoleBasis,Directions+2>,2> poles{};
    const auto cellMask=atlas.cellMask(weather);
    if constexpr(Fast) {
        for(int i=0;i<=span*2;++i) {
            const int x=ix-span+i;dx[i]=float((x+.5-ex)*cell);cellX[i]=canopy::wrap(x,Cells);
        }
        for(int e=0;e<Elevations;++e)heights[e]=projectedHalfHeight(halfWidth,halfHeight,e);
        for(int p=0;p<2;++p)for(int a=0;a<Directions+2;++a) {
            const float az=2.f*Pi*float(a)/Directions;
            auto& b=poles[p][a];b.right={std::sin(az),-std::cos(az),0};
            b.up=cross(b.right,direction(a,p?Elevations-1:0));
        }
    }
    const auto visible=[&](Vec delta) {
        const float forward=dot(delta,view.forward);
        return forward+cardExtent>0&&std::abs(dot(delta,view.right))<=wx*forward+wxExtent&&
            std::abs(dot(delta,view.up))<=wy*forward+wyExtent;
    };
    for(int y=iy-span;y<=iy+span;++y) {
        const float dy=float((y+.5-ey)*cell);
        const int row=canopy::wrap(y,Cells)*Cells;
        for(int x=ix-span;x<=ix+span;++x) {
            const int sourceCell=row+(Fast?cellX[x-(ix-span)]:canopy::wrap(x,Cells));
            if constexpr(Fast)if(!(cellMask&(1u<<sourceCell)))continue;
            const Vec delta{Fast?dx[x-(ix-span)]:float((x+.5-ex)*cell),dy,base-view.pos.z};
            const float squared=dot(delta,delta);
            if(squared<1.e-6f||squared<inner2||squared>outer2)continue;
            if constexpr(Fast)if(!visible(delta))continue;
            const float d=std::sqrt(squared);
            const float fade=levels.billboard(d)*banks::rangeWeight(r,d);
            const bool transition=d+cardExtent>levels.start&&d-cardExtent<levels.end;
            const float possible=transition?levels.billboard(d+cardExtent)*banks::rangeWeight(r,std::max(0.f,d-cardExtent)):fade;
            if(possible<=1.f/255.f)continue;
            if constexpr(!Fast)if(!visible(delta))continue;
            Vec radial=unit(Vec{delta.x,delta.y,0});
            if(horizontalSq(radial)<.5f)radial={1.f,0.f,0.f};
            float angle=std::atan2(radial.y,radial.x)*Directions/(2.f*Pi);
            if(angle<0.f)angle+=Directions;
            const float pitch=std::asin(clamp(delta.z/d,-1.f,1.f));
            const Vec right=cross(radial,{0,0,1}),up=cross(right,delta*(1.f/d));
            const float fog=cloudFogFactor(s,d);
            for(const auto& sample:angularSamples(angle,pitch)) {
                const float weight=sample.weight;
                const int index=Fast?(sourceCell*Directions+canopy::wrap(sample.azimuth,Directions))*Elevations+sample.elevation:
                    slot(x,y,sample.azimuth,sample.elevation);
                if(weight*possible<=1.f/255.f||!atlas.any(index,weather))continue;
                // Approximate optical-depth splitting to reduce the opacity
                // dip during crossfades, without introducing a second field.
                const float alpha=weight*fade*(1.f+.25f*(1.f-weight*fade));
                Vec cardRight=right,cardUp=up;
                if(sample.elevation==0||sample.elevation==Elevations-1) {
                    // Zenith/nadir images are projections of the world XY
                    // field. Keep their basis world-fixed even through the
                    // singularity directly above/below a cell centre.
                    if constexpr(Fast) {
                        const auto& b=poles[sample.elevation==Elevations-1][sample.azimuth];
                        cardRight=b.right;cardUp=b.up;
                    } else {
                        const float az=2.f*Pi*float(sample.azimuth)/Directions;
                        cardRight={std::sin(az),-std::cos(az),0};
                        cardUp=cross(cardRight,direction(sample.azimuth,sample.elevation));
                    }
                }
                cards.push_back({view.pos+delta,cardRight*halfWidth,cardUp*(Fast?heights[sample.elevation]:
                    projectedHalfHeight(halfWidth,halfHeight,sample.elevation)),alpha,weight,d,fog,transition,index});
            }
        }
    }
    std::sort(cards.begin(),cards.end(),[](const Card& a,const Card& b){
        if(a.depth!=b.depth)return a.depth>b.depth;
        if(a.center.x!=b.center.x)return a.center.x<b.center.x;
        if(a.center.y!=b.center.y)return a.center.y<b.center.y;
        return a.index<b.index;
    });
    if(cards.size()>MaxCards)cards.erase(cards.begin(),cards.begin()+std::ptrdiff_t(cards.size()-MaxCards));
    for(const auto& c:cards) {
        const auto b=Fast?atlas.supportCached(c.index,weather):atlas.support(c.index,weather);
        int first=0,last=0;
        // More triangles are worth it only for larger, substantially
        // sparse sprites. Keep the hard source-vertex budget.
        if(c.depth<3500.f&&cards.size()<=MaxCards/2) {
            const auto whole=Atlas::area(b[0]),horizontal=Atlas::area(b[1])+Atlas::area(b[2]),
                vertical=Atlas::area(b[3])+Atlas::area(b[4]),best=std::min(horizontal,vertical);
            if(best*4<whole*3){first=horizontal<=vertical?1:3;last=first+1;}
        }
        const int ox=c.index%(AtlasSize/TileW)*TileW,oy=c.index/(AtlasSize/TileW)*TileH;
        for(int part=first;part<=last;++part) {
            const auto q=b[part];if(!q.x1||q.x0>=q.x1||q.y0>=q.y1)continue;
            const auto start=std::uint16_t(mesh.vertices.size());
            for(int j=0;j<4;++j) {
                const float x=j&1?q.x1:q.x0,y=j&2?q.y1:q.y0;
                const float u=(x-Guard)/float(TileW-2*Guard),v=(y-Guard)/float(TileH-2*Guard);
                const Vec p=c.center+c.right*(2.f*u-1.f)+c.up*(1.f-2.f*v);
                float alpha=c.alpha;
                if(c.transition) {
                    // The old centre-only switch gave an entire 1 km
                    // cloud the same fade even when its edge was already
                    // within the true volume. Use the identical radial
                    // complement as the world-space volume at its proxy
                    // vertices; do not move or respawn any cloud.
                    const float distance=length(p-view.pos);
                    const float a=c.weight*levels.billboard(distance)*banks::rangeWeight(r,distance);
                    alpha=a*(1.f+.25f*(1.f-a));
                }
                mesh.vertices.push_back({p,(ox+x)/AtlasSize,(oy+y)/AtlasSize,0,alpha,c.fog});
            }
            if(mesh.vertices[start].alpha<=0.f&&mesh.vertices[start+1].alpha<=0.f&&
               mesh.vertices[start+2].alpha<=0.f&&mesh.vertices[start+3].alpha<=0.f) {
                mesh.vertices.resize(start);continue;
            }
            for(int i:{0,2,1,1,2,3})mesh.indices.push_back(std::uint16_t(start+i));
        }
    }
}
inline bool eligible(const Settings& s,const Range& r) {
    return s.farBillboards&&r.end/(canopy::tileWorldSize(s)/Cells)<45.f;
}
// The shared directional ground canopy has no minimum range. Only
// standalone far-flight impostors retain the old distance threshold.
inline bool farEligible(const Settings& s,const Range& r) {
    const auto levels=banks::lod(s,r);
    return eligible(s,r)&&levels.start<levels.end;
}
} // namespace vc::billboards
