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
    for(int tile=0;tile<cumulus::Variants;++tile) {
        int opaque=0,transparent=0,fringe=0;
        for(int y=0;y<cumulus::Tile;++y) for(int x=0;x<cumulus::Tile;++x) {
            const auto p=atlas[(tile/2*cumulus::Tile+y)*cumulus::AtlasSize+tile%2*cumulus::Tile+x];
            CHECK(p.r==p.g && p.g==p.b && p.r>=180);
            if(x==0||y==0||x==cumulus::Tile-1||y==cumulus::Tile-1) CHECK(p.a==0);
            opaque+=p.a>=230; transparent+=p.a==0; fringe+=p.a>0&&p.a<230;
        }
        CHECK(opaque>1800 && transparent>1000 && fringe>200);
        const auto min=cumulus::atlasUV(tile,0,0),max=cumulus::atlasUV(tile,1,1);
        const float col=float(tile%2)*.5f,row=float(tile/2)*.5f;
        CHECK(min.u>col && min.v>row && max.u<col+.5f && max.v<row+.5f);
    }
    CHECK(std::memcmp(atlas.data(),atlas.data()+cumulus::Tile,cumulus::Tile*sizeof(cumulus::Pixel))==0); // padded first row
    Settings s;
    CHECK(s.renderMode==1);
    std::istringstream in("RenderMode=2\nCumulusDetail=99\nCumulusDensity=-1\nCumulusContrast=nan\nCumulusRimStrength=9\n");
    load(in,s);
    CHECK(s.renderMode==2 && s.cumulusDetail==1 && s.cumulusDensity==0 && s.cumulusContrast==.65f && s.cumulusRimStrength==1);
    s=Settings{}; s.renderMode=2;
    const View v{{0,0,400},{1,0,0},{0,0,1},{0,1,0},2500,1};
    const auto r=range(s,v.farClip,2000);
    Field f; f.reset(s); f.update(s,v,r,MaxClouds,{},1);
    const Palette p{{75,148,215},{174,220,250},{0,0,0},{0,0,0}};
    const cumulus::Lighting front(s,p,{0,-.98f,.2f}),back(s,p,{0,.98f,.2f});
    const Card test{{0,100,400},{50,0,0},{0,0,32},1,.5f,0};
    const auto fb=front.basis(test,v),bb=back.basis(test,v);
    CHECK(front.shade(fb,0,0).r>back.shade(bb,0,0).r);
    CHECK(back.shade(bb,0,.8f).r>back.shade(bb,0,0).r+25);
    auto noRim=s; noRim.cumulusRimStrength=0;
    const cumulus::Lighting noRimLight(noRim,p,{0,.98f,.2f});
    CHECK(back.shade(bb,0,.8f).r>noRimLight.shade(noRimLight.basis(test,v),0,.8f).r);
    const cumulus::Lighting night(s,{{5,6,12},{14,15,20},{0,0,0},{0,0,0}},{0,1,-1});
    CHECK(night.day==0 && std::abs(night.shade(night.basis(test,v),0,0).r-76.f)<.001f);
    Card cards[MaxPuffs];
    for(int count=1;count<=MaxPuffs;++count) {
        s.puffsPerCloud=count;
        for(const auto& c:f.clouds) {
            CHECK(cumulus::cards(s,c,v,.8f,cards)==count);
            for(int i=0;i<count;++i) {
                const auto& card=cards[i];
                CHECK(length(card.center-c.position)+cardBound(card)<=bound(c));
                CHECK(std::abs(dot(unit(card.right),unit(card.up)))<.001f);
                CHECK(dot(card.up,{0,0,1})>0.f && card.alpha>=0.f && card.alpha<=.8f+.0001f);
                CHECK(finite(card.center));
            }
        }
    }
    // A camera facing straight up/down must still have a valid perpendicular basis.
    View zenith=v;zenith.forward={0,0,1};zenith.up={0,-1,0};
    cumulus::cards(s,f.clouds[0],zenith,.8f,cards);
    CHECK(std::abs(dot(unit(cards[0].right),unit(cards[0].up)))<.001f);
    CHECK(std::abs(dot(unit(cards[0].up),zenith.forward))<.001f);

    View inside=v;
    cumulus::cards(s,f.clouds[0],v,.8f,cards);
    inside.pos=cards[0].center;
    cumulus::cards(s,f.clouds[0],inside,.8f,cards);
    CHECK(cards[0].alpha==0.f);
    s.nearFadeDistance=0;
    cumulus::cards(s,f.clouds[0],inside,.8f,cards);
    CHECK(cards[0].alpha>0.f);
    s.nearFadeDistance=25;

    // Index count, UV bounds and finite lighting across complete geometry at both qualities.
    volatile float sink=0.f;
    const auto start=Clock::now();
    for(int detail=0;detail<=1;++detail) {
        s.cumulusDetail=detail;
        s.puffsPerCloud=5;
        const int n=cumulus::grid(s);
        for(const auto& c:f.clouds) {
            const int count=cumulus::cards(s,c,v,1.f,cards);
            for(int i=0;i<count;++i) {
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
    // Modern opacity maps native low alpha to solid masses only in opt-in mode.
    Cloud c=f.clouds[0]; c.position={0,500,400};c.fade=1.f;c.alpha=.25f;
    Settings old=s;old.renderMode=1;
    CHECK(cloudAlpha(s,c,v,r)>cloudAlpha(old,c,v,r)*2);
    c.fade=0;CHECK(cloudAlpha(s,c,v,r)==0);
    std::cout << "PASS: deterministic RGBA atlas, padded UVs, mode settings, solar response, "
              << "silver lining, night, geometry, quality, index/vertex bounds, opacity.\n"
              << "CPU diagnostics (not GPU or in-game): bake " << bakeMs << " ms; "
              << MaxClouds*5*2 << " card meshes including checks " << meshMs << " ms\n";
}
