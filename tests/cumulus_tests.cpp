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
    CHECK(cumulus::bakeAtlas(false).size()==atlas.size());
    Settings s;
    CHECK(s.renderMode==1 && s.cumulusImpostors && s.cumulusNightLight==1.f);
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
    CHECK(front.shade(fb,0,0).r>back.shade(bb,0,0).r);
    CHECK(back.shade(bb,0,.8f).r>back.shade(bb,0,0).r+25);
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
    Card cards[MaxPuffs];
    s.cumulusImpostors=false;
    for(int count=1;count<=MaxPuffs;++count) {
        s.puffsPerCloud=count;
        for(const auto& c:f.clouds) {
            CHECK(cumulus::cards(s,c,v,.8f,cards)==count);
            for(int i=0;i<count;++i) {
                const auto& card=cards[i];
                CHECK(length(card.center-c.position)+cardBound(card)<=bound(c));
                CHECK(std::abs(dot(unit(card.right),unit(card.up)))<.001f);
                CHECK(card.alpha>=0.f && card.alpha<=.8f+.0001f);
                CHECK(finite(card.center));
            }
        }
    }
    // A camera facing straight up/down must still have a valid perpendicular basis.
    View zenith=v;zenith.forward={0,0,1};zenith.up={0,-1,0};
    cumulus::cards(s,f.clouds[0],zenith,.8f,cards);
    CHECK(std::abs(dot(unit(cards[0].right),unit(cards[0].up)))<.001f);
    CHECK(std::abs(dot(unit(cards[0].up),cumulus::eyeDirection(cards[0].center,zenith.pos)))<.001f);

    View inside=v;
    cumulus::cards(s,f.clouds[0],v,.8f,cards);
    inside.pos=cards[0].center;
    cumulus::cards(s,f.clouds[0],inside,.8f,cards);
    CHECK(cards[0].alpha==0.f);
    s.nearFadeDistance=0;
    cumulus::cards(s,f.clouds[0],inside,.8f,cards);
    CHECK(cards[0].alpha>0.f);
    s.nearFadeDistance=25;

    // The whole-cloud representation and every close layer are independent
    // of yaw, pitch and roll at a fixed world eye. Exact geometric invariance.
    s.cumulusImpostors=true;
    s.puffsPerCloud=5;
    Cloud fixed=f.clouds[0];
    fixed.size={300,180,85};fixed.position={0,0,500};
    View below=v;below.pos={0,0,0};below.forward={0,0,1};below.up={0,-1,0};
    View rolled=below;rolled.right={0,1,0};rolled.up={1,0,0};rolled.forward={.6f,0,.8f};
    Card rotated[MaxPuffs];
    CHECK(cumulus::cards(s,fixed,below,.90f,cards)==1);
    CHECK(cumulus::cards(s,fixed,rolled,.90f,rotated)==1);
    CHECK(length(cards[0].right-rotated[0].right)==0 && length(cards[0].up-rotated[0].up)==0);
    // The underside uses the X/Y radius (104m), not the 49m altitude radius.
    CHECK(length(cards[0].right)>150.f && length(cards[0].up)>100.f);
    View side=below;side.pos={0,-500,500};
    CHECK(cumulus::cards(s,fixed,side,.90f,cards)==1);
    CHECK(length(cards[0].up)<50.f);
    // A near-horizontal antipodal ray and all zenith azimuths are finite.
    for(int y=-8;y<=8;++y) for(int x=-8;x<=8;++x) {
        Vec direction=unit(Vec{float(x)*.12f,float(y)*.12f,1.f});
        Vec right,up;cumulus::facingAxes(direction,right,up);
        CHECK(finite(right)&&finite(up));
        CHECK(std::abs(length(right)-1.f)<1e-4f && std::abs(length(up)-1.f)<1e-4f);
        CHECK(std::abs(dot(right,direction))<1e-4f && std::abs(dot(up,direction))<1e-4f);
    }
    Vec right,up;cumulus::facingAxes({0,-1,0},right,up);
    CHECK(finite(right) && finite(up) && length(right)>.99f && length(up)>.99f);
    View closeView=below;closeView.pos={0,0,450};
    rolled.pos=closeView.pos;
    for(int body=0;body<2;++body) {
        s.cumulusImpostors=body!=0;
        const int a=cumulus::cards(s,fixed,closeView,.9f,cards);
        const int b=cumulus::cards(s,fixed,rolled,.9f,rotated);
        CHECK(a==b && a>=1 && a<=MaxPuffs);
        for(int i=0;i<a;++i) {
            CHECK(length(cards[i].center-rotated[i].center)==0.f);
            CHECK(length(cards[i].right-rotated[i].right)==0.f);
            CHECK(length(cards[i].up-rotated[i].up)==0.f);
        }
    }
    s.cumulusImpostors=true;

    // Index count, UV bounds and finite lighting across complete geometry at both qualities.
    volatile float sink=0.f;
    int meshCount=0;
    const auto start=Clock::now();
    for(int detail=0;detail<=1;++detail) {
        s.cumulusDetail=detail;
        s.puffsPerCloud=5;
        const int n=cumulus::grid(s);
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
    // Modern opacity maps native low alpha to solid masses only in opt-in mode.
    Cloud c=f.clouds[0]; c.position={0,500,400};c.fade=1.f;c.alpha=.25f;
    Settings old=s;old.renderMode=1;
    CHECK(cloudAlpha(s,c,v,r)>cloudAlpha(old,c,v,r)*2);
    c.fade=0;CHECK(cloudAlpha(s,c,v,r)==0);
    std::cout << "PASS: coherent RGBA atlas, padded UVs, world-locked card basis, solar/lunar response, "
              << "silver lining, night, projected geometry, quality, index/vertex bounds, optical LOD.\n"
              << "CPU diagnostics (not GPU or in-game): bake " << bakeMs << " ms; "
              << meshCount << " card meshes including checks " << meshMs << " ms\n";
}
