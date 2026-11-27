#include "../Cumulus.hpp"
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (false)
using namespace vc;

int main() {
    using Clock=std::chrono::steady_clock;
    const auto begin=Clock::now();
    auto atlas=cumulus::bakeAtlas();
    const double bakeMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
    CHECK(atlas.size()==std::size_t(cumulus::AtlasSize*cumulus::AtlasSize));
    const auto repeat=cumulus::bakeAtlas();
    CHECK(std::memcmp(atlas.data(),repeat.data(),atlas.size()*sizeof(cumulus::Pixel))==0);
    for(int tile=0;tile<cumulus::Tiles;++tile) {
        int opaque=0,transparent=0,fringe=0;
        for(int y=0;y<cumulus::Tile;++y) for(int x=0;x<cumulus::Tile;++x) {
            const auto p=atlas[(tile/cumulus::AtlasColumns*cumulus::Tile+y)*cumulus::AtlasSize+tile%cumulus::AtlasColumns*cumulus::Tile+x];
            CHECK(p.r==p.g && p.g==p.b && p.r>=208);
            if(x==0||y==0||x==cumulus::Tile-1||y==cumulus::Tile-1) CHECK(p.a==0);
            opaque+=p.a>=230; transparent+=p.a==0; fringe+=p.a>0&&p.a<230;
        }
        CHECK(opaque>200 && transparent>1000 && fringe>1000);
        const auto min=cumulus::atlasUV(tile,0,0),max=cumulus::atlasUV(tile,1,1);
        const float col=float(tile%cumulus::AtlasColumns)/cumulus::AtlasColumns,row=float(tile/cumulus::AtlasColumns)/cumulus::AtlasColumns;
        CHECK(min.u>col && min.v>row && max.u<col+.25f && max.v<row+.25f);
    }
    CHECK(std::memcmp(atlas.data(),atlas.data()+cumulus::Tile,cumulus::Tile*sizeof(cumulus::Pixel))==0); // padded first row
    // All three projections of every archetype must differ, not just share UVs.
    for(int tile=0;tile<cumulus::Tiles;++tile) for(int other=0;other<tile;++other) {
        int different=0;
        for(int y=0;y<cumulus::Tile;++y) for(int x=0;x<cumulus::Tile;++x) {
            const auto at=[&](int t){return atlas[(t/cumulus::AtlasColumns*cumulus::Tile+y)*cumulus::AtlasSize+t%cumulus::AtlasColumns*cumulus::Tile+x].a;};
            different+=at(tile)!=at(other);
        }
        CHECK(different>1000);
    }
    Settings s;
    CHECK(s.renderMode==1 && s.cumulusDetail==0 && s.cumulusImpostors && s.cumulusNightLight==1.f);
    std::istringstream in("RenderMode=2\nCumulusDetail=99\nCumulusDensity=-1\nCumulusContrast=nan\nCumulusRimStrength=9\nCumulusNightLight=9\nCumulusImpostors=0\n");
    load(in,s);
    CHECK(s.renderMode==2 && s.cumulusDetail==1 && s.cumulusDensity==0 && s.cumulusContrast==.65f && s.cumulusRimStrength==1 && !s.cumulusImpostors && s.cumulusNightLight==2);
    s=Settings{}; s.renderMode=2;
    const View v{{0,0,400},{1,0,0},{0,0,1},{0,1,0},2500,1};
    const auto r=range(s,v.farClip,2000);
    Field f; f.reset(s); f.update(s,v,r,MaxClouds,{},1);
    const Palette p{{75,148,215},{174,220,250},{0,0,0},{0,0,0}};
    const cumulus::Lighting front(s,p,{0,-.98f,.2f}),back(s,p,{0,.98f,.2f});
    const Card test{{0,100,400},{50,0,0},{0,0,32},1,.5f,0};
    const auto fb=front.basis(test,v),bb=back.basis(test,v);
    CHECK(front.shade(fb,0,.8f).r>front.shade(fb,0,-.8f).r);
    CHECK(back.shade(bb,0,.8f).r<=back.neutral);
    auto noRim=s; noRim.cumulusRimStrength=0;
    const cumulus::Lighting noRimLight(noRim,p,{0,.98f,.2f});
    CHECK(back.shade(bb,0,.8f).r>noRimLight.shade(noRimLight.basis(test,v),0,.8f).r);
    const cumulus::Lighting night(s,{{5,6,12},{14,15,20},{0,0,0},{0,0,0}},{0,1,-1});
    CHECK(night.day==0);
    const auto nightColour=night.shade(night.basis(test,v),0,0);
    CHECK(nightColour.r>15 && nightColour.r<45 && nightColour.b>nightColour.r);
    CHECK(cumulus::moonVisibility(0,0,0,0)==1.f);
    CHECK(cumulus::moonVisibility(12,0,0,0)==0.f);
    CHECK(cumulus::moonVisibility(23,42,1,0)<.2f);
    const cumulus::Lighting lunar(s,{{5,6,12},{14,15,20},{0,0,0},{0,0,0}},{0,1,-1},1.f);
    CHECK(lunar.shade(lunar.basis(test,v),0,0).r>nightColour.r);
    CHECK(std::abs((1.f-cumulus::opticalPortion(.95f,.3f))*
                   (1.f-cumulus::opticalPortion(.95f,.7f))-.05f)<.0001f);
    Card cards[MaxPuffs],rotated[MaxPuffs];
    for(const auto& c:f.clouds) if(c.used) {
        CHECK(cumulus::cards(s,c,v,.8f,cards)==3);
        for(int i=0;i<3;++i) {
            const auto& card=cards[i];
            CHECK(length(card.center-c.position)+cardBound(card)<=bound(c));
            CHECK(std::abs(dot(unit(card.right),unit(card.up)))<.001f);
            CHECK(card.alpha>=0.f && card.alpha<=.8f+.0001f);
            CHECK(finite(card.center));
        }
    }
    // Exact geometry, UV and lighting invariance under yaw, roll, translation,
    // and altitude changes, including a camera directly in the cloud center.
    Cloud fixed=f.clouds[0];
    fixed.size={300,180,85};fixed.position={0,0,500};
    View below=v;below.pos={0,0,0};below.forward={0,0,1};below.up={0,-1,0};
    const float a=.85f;
    CHECK(cumulus::cards(s,fixed,below,a,cards)==3);
    CHECK(cards[0].alpha==0 && cards[1].alpha==0 && std::abs(cards[2].alpha-a)<1e-5f);
    CHECK(std::abs(length(cards[2].right)-150.f)<1e-4f);
    CHECK(std::abs(length(cards[2].up)-90.f)<1e-4f);
    for(int x=-2;x<=2;++x) for(int y=-2;y<=2;++y) for(int z=-3;z<=3;++z) {
        View moved=below;moved.pos=fixed.position+Vec{float(x)*300,float(y)*300,float(z)*200};
        moved.forward={.6f,.8f,0};moved.up={0,0,1};moved.right={.8f,-.6f,0};
        s.cumulusImpostors=(z%2)==0; s.puffsPerCloud=1+(z+3);
        CHECK(cumulus::cards(s,fixed,moved,a,rotated)==3);
        float trans=1.f;
        for(int i=0;i<3;++i) {
            CHECK(length(cards[i].center-rotated[i].center)==0.f);
            CHECK(length(cards[i].right-rotated[i].right)==0.f);
            CHECK(length(cards[i].up-rotated[i].up)==0.f);
            CHECK(cards[i].textureKey==rotated[i].textureKey);
            CHECK(std::isfinite(rotated[i].alpha));
            trans*=1.f-rotated[i].alpha;
            cumulus::LitVertex va[cumulus::MaxVertices],vb[cumulus::MaxVertices];
            cumulus::mesh(cards[i],below,back,3,va);
            cumulus::mesh(rotated[i],moved,back,3,vb);
            for(int n=0;n<9;++n) {
                CHECK(length(va[n].pos-vb[n].pos)==0.f);
                CHECK(va[n].uv.u==vb[n].uv.u && va[n].uv.v==vb[n].uv.v);
                CHECK(va[n].colour.r==vb[n].colour.r && va[n].colour.g==vb[n].colour.g && va[n].colour.b==vb[n].colour.b);
            }
        }
        CHECK(std::abs(trans-(1.f-a))<1e-5f);
        if(z==0 && (x||y)) CHECK(rotated[2].alpha==0);
    }

    // Index count, UV bounds and finite lighting across complete geometry at both qualities.
    volatile float sink=0.f;
    int meshCount=0;
    const auto start=Clock::now();
    for(int detail=0;detail<=1;++detail) {
        s.cumulusDetail=detail;
        s.puffsPerCloud=5;
        const int n=cumulus::grid(s);
        CHECK(n==2+detail);
        CHECK(3*(n-1)*(n-1)*2==(detail?24:6));
        for(const auto& c:f.clouds) {
            const int count=cumulus::cards(s,c,v,1.f,cards);
            for(int i=0;i<count;++i) {
                ++meshCount;
                cumulus::LitVertex mesh[cumulus::MaxVertices];
                CHECK(cumulus::mesh(cards[i],v,back,n,mesh)==n*n);
                std::uint16_t indices[cumulus::MaxIndices];
                const int base=480-n*n;
                const int ni=cumulus::indices(n,base,indices);
                CHECK(ni==(n-1)*(n-1)*6);
                for(int j=0;j<ni;++j)CHECK(indices[j]>=base&&indices[j]<480);
                for(int j=0;j<n*n;++j) {
                    const auto& m=mesh[j];
                    CHECK(finite(m.pos)&&std::isfinite(m.colour.r)&&m.colour.r>=0&&m.colour.r<=255);
                    CHECK(m.uv.u>0&&m.uv.u<1&&m.uv.v>0&&m.uv.v<1);
                    sink+=m.colour.r;
                }
            }
        }
    }
    const double meshMs=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    CHECK(sink>0);
    // Modern opacity uses a bounded, more translucent remapping only in opt-in mode.
    Cloud c=f.clouds[0]; c.position={0,500,400};c.fade=1.f;c.alpha=.25f;
    Settings old=s;old.renderMode=1;
    CHECK(cloudAlpha(s,c,v,r)>cloudAlpha(old,c,v,r));
    CHECK(cloudAlpha(s,c,v,r)<.80f);
    c.fade=0;CHECK(cloudAlpha(s,c,v,r)==0);
    std::cout << "PASS: shared-density projection atlas, padded UVs, invariant world geometry/UV/light, solar/lunar response, "
              << "restrained edge light, night, triangle budget, index/vertex bounds, optical weights.\n"
              << "CPU diagnostics (not GPU or in-game): bake " << bakeMs << " ms; "
              << meshCount << " card meshes including checks " << meshMs << " ms\n";
}
