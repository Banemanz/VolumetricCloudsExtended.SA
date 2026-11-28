#pragma once
#include "CloudBanks.hpp"

// Very distant impostors derived from the SAME density and optical shading
// as the volume. Not unrelated stock cloud sprites. World-wind-anchored
// cells overlap with a partition of unity; adjacent azimuths crossfade.
namespace vc::billboards {
constexpr int Cells=4,Directions=8,TileW=64,TileH=32,AtlasSize=512,Guard=2;
constexpr float Support=.75f,HalfWidth=1.061f; // sqrt(2)*Support
constexpr int MaxCards=4096; // at most 16384 source vertices
static_assert(Cells*Cells*Directions==AtlasSize/TileW*(AtlasSize/TileH));
inline float window(float x) {return smooth((Support-std::abs(x))/.5f);}
inline int slot(int x,int y,int angle) {
    return (canopy::wrap(y,Cells)*Cells+canopy::wrap(x,Cells))*Directions+canopy::wrap(angle,Directions);
}
inline Vec direction(int angle) {
    const float a=2.f*Pi*float(angle)/Directions;
    return {std::cos(a),std::sin(a),0};
}
struct Atlas {
    std::array<std::vector<canopy::Pixel>,2> pixels;
    std::array<std::array<bool,Cells*Cells*Directions>,2> occupied{};
    void build(const Settings& s,const banks::Volume& volume) {
        const float cell=canopy::tileWorldSize(s)/Cells;
        const int steps=std::clamp(int(std::ceil(2.f*HalfWidth*cell/28.f)),20,56);
        const float step=2.f*HalfWidth/steps;
        // Use a LUT for conversion from reference-step alpha to optical depth.
        std::array<float,256> optical{};for(int i=0;i<256;++i)optical[i]=-std::log(std::max(1.f-i/255.f,.0001f))*step*cell/banks::ReferenceStep;
        for(int weather=0;weather<2;++weather) {
            auto& tex=pixels[weather];tex.assign(AtlasSize*AtlasSize,{255,255,255,0});
            occupied[weather].fill(false);
            for(int cy=0;cy<Cells;++cy)for(int cx=0;cx<Cells;++cx)for(int angle=0;angle<Directions;++angle) {
                const int index=slot(cx,cy,angle),ox=index%(AtlasSize/TileW)*TileW,oy=index/(AtlasSize/TileW)*TileH;
                const Vec dir=direction(angle),right=cross(dir,{0,0,1});
                for(int y=Guard;y<TileH-Guard;++y)for(int x=Guard;x<TileW-Guard;++x) {
                    const float offset=mix(-HalfWidth,HalfWidth,(x-Guard+.5f)/(TileW-2*Guard));
                    const float w=1.f-(y-Guard+.5f)/(TileH-2*Guard);
                    float trans=1.f,grey=0.f;
                    // Front-to-back integration into a straight-alpha texel.
                    for(int k=0;k<steps;++k) {
                        const float along=-HalfWidth+(k+.5f)*step;
                        const Vec local=right*offset+dir*along;
                        const float mask=window(local.x)*window(local.y);
                        if(mask<.001f)continue;
                        const auto p=volume.sample(weather,(cx+.5f+local.x)/Cells,(cy+.5f+local.y)/Cells,w);
                        if(!p.a)continue;
                        const float a=1.f-std::exp(-optical[p.a]*mask);
                        grey+=trans*a*p.r;trans*=1.f-a;
                        if(trans<.002f)break;
                    }
                    const float alpha=1.f-trans;
                    const auto shade=canopy::byte(alpha>0.f?grey/alpha:255.f);
                    const auto a=canopy::byte(alpha*255.f);
                    tex[std::size_t(oy+y)*AtlasSize+ox+x]={shade,shade,shade,a};
                    occupied[weather][index]|=a>0;
                }
            }
        }
    }
    bool any(int index,float weather) const {
        return (weather<.999f&&occupied[0][index])||(weather>.001f&&occupied[1][index]);
    }
};
struct Card {Vec center,right;float alpha,depth,fog;int index;};
inline void buildMesh(banks::Mesh& mesh,const Settings& s,const View& view,const Range& r,
                      const canopy::Drift& drift,const Atlas& atlas,float wx,float wy,float weather) {
    mesh.vertices.clear();mesh.indices.clear();mesh.slices=0;
    const auto levels=banks::lod(s,r,true,view.pos.z-(s.minSpawnZ+s.maxSpawnZ)*.5f);
    if(levels.start>=levels.end)return;
    const float cell=canopy::tileWorldSize(s)/Cells,base=(s.minSpawnZ+s.maxSpawnZ)*.5f,halfHeight=(s.maxSpawnZ-s.minSpawnZ)*.5f;
    const float halfWidth=cell*HalfWidth,extent=std::sqrt(halfWidth*halfWidth+halfHeight*halfHeight);
    // Use wrapped drift and camera-relative cell indices. The atlas repeats
    // exactly every tile, and wrapping the wind never relocates a card.
    const double ex=double(view.pos.x)/cell-drift.x*Cells,ey=double(view.pos.y)/cell-drift.y*Cells;
    const int ix=int(std::floor(ex)),iy=int(std::floor(ey));
    // At extreme tiny SizeScale, enumerating millions of subpixel cells is
    // worse than leaving the volume active. Caller checks eligibility.
    const int span=int(std::ceil((r.end+extent)/cell));
    if(span>80)return;
    static thread_local std::vector<Card> cards;
    cards.clear();if(cards.capacity()<256)cards.reserve(256);
    const float wxExtent=extent*std::sqrt(1+wx*wx),wyExtent=extent*std::sqrt(1+wy*wy);
    // Reject the strictly zero-fade regions before a square root.
    // Keep a world-unit guard around each endpoint for rounding.
    const float inner=std::max(0.f,levels.start-1.f),outer=r.end+1.f;
    const float inner2=inner*inner,outer2=outer*outer;
    for(int y=iy-span;y<=iy+span;++y)for(int x=ix-span;x<=ix+span;++x) {
        const Vec delta{float((x+.5-ex)*cell),float((y+.5-ey)*cell),base-view.pos.z};
        const float squared=dot(delta,delta);
        if(squared<inner2||squared>outer2)continue;
        const float d=std::sqrt(squared);
        // The center controls the smooth optical LOD, no arbitrary per-frame
        // recycling. Conservative sphere/frustum reject, independent of yaw.
        const float fade=levels.billboard(d)*banks::rangeWeight(r,d);
        if(fade<=1.f/255.f)continue;
        const float forward=dot(delta,view.forward);
        if(forward+extent<=0||std::abs(dot(delta,view.right))>wx*forward+wxExtent||
            std::abs(dot(delta,view.up))>wy*forward+wyExtent)continue;
        const Vec radial=unit(Vec{delta.x,delta.y,0});
        if(horizontalSq(radial)<.5f)continue;
        float angle=std::atan2(radial.y,radial.x)*Directions/(2.f*Pi);
        if(angle<0.f)angle+=Directions;
        const int first=int(std::floor(angle));
        const float blend=smooth(angle-first);
        const Vec right=cross(radial,{0,0,1})*halfWidth;
        for(int i=0;i<2;++i) {
            const float weight=i?blend:1.f-blend;
            const int index=slot(x,y,first+i);
            if(weight*fade<=1.f/255.f||!atlas.any(index,weather))continue;
            cards.push_back({view.pos+delta,right,weight*fade,d,cloudFogFactor(s,d),index});
        }
    }
    // Sort by actual radial distance, not heading-dependent view depth:
    // changing camera orientation alone never changes overlapping card order.
    std::sort(cards.begin(),cards.end(),[](const Card& a,const Card& b){
        if(a.depth!=b.depth)return a.depth>b.depth;
        if(a.center.x!=b.center.x)return a.center.x<b.center.x;
        if(a.center.y!=b.center.y)return a.center.y<b.center.y;
        return a.index<b.index;
    });
    if(cards.size()>MaxCards)cards.erase(cards.begin(),cards.begin()+std::ptrdiff_t(cards.size()-MaxCards));
    for(const auto& c:cards) {
        const float u=(c.index%(AtlasSize/TileW)*TileW+Guard)/float(AtlasSize);
        const float v=(c.index/(AtlasSize/TileW)*TileH+Guard)/float(AtlasSize);
        const float du=float(TileW-2*Guard)/AtlasSize,dv=float(TileH-2*Guard)/AtlasSize;
        const auto start=std::uint16_t(mesh.vertices.size());
        for(int j=0;j<4;++j) {
            // Fixed upright facing the viewer POSITION, never camera orientation.
            const Vec p=c.center+c.right*(j&1?1.f:-1.f)+Vec{0,0,j&2?-halfHeight:halfHeight};
            mesh.vertices.push_back({p,u+(j&1?du:0.f),v+(j&2?dv:0.f),0,c.alpha,c.fog});
        }
        for(int i:{0,2,1,1,2,3})mesh.indices.push_back(std::uint16_t(start+i));
    }
}
inline bool eligible(const Settings& s,const Range& r) {
    return banks::lod(s,r).start<banks::lod(s,r).end &&
        r.end/(canopy::tileWorldSize(s)/Cells)<45.f;
}
} // namespace vc::billboards
