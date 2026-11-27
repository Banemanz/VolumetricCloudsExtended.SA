#include "../CloudVolumes.hpp"
#include <cstring>
#include <iostream>
#include <sstream>
#define CHECK(x) do { if (!(x)) { std::cerr<<"Line "<<__LINE__<<": "<<#x<<'\n'; std::exit(1); } } while(false)
using namespace vc;
using namespace vc::volume;

std::array<float,Views> allWeights(Vec dir) {
    std::array<float,Views> a{};
    float sum=0.f;
    for(auto w:weights(dir)) {
        CHECK(w.view>=0 && w.view<Views && w.amount>=0.f && w.amount<=1.f);
        a[w.view]+=w.amount;sum+=w.amount;
        CHECK(w.amount<.001f || dot(unit(dir),captures()[w.view].dir)>.50f);
    }
    CHECK(std::abs(sum-1.f)<.0001f);
    return a;
}
int main() {
    Settings s;
    CHECK(s.hybridClouds);
    {Settings temp;std::istringstream in("HybridClouds=0\nRenderMode=1\n");load(in,temp);CHECK(!temp.hybridClouds && temp.renderMode==1);}
    for(int el=-90;el<=90;el+=2) for(int az=0;az<360;az+=2) {
        const float e=el*Pi/180.f,a=az*Pi/180.f;
        const Vec dir{std::sin(a)*std::cos(e),std::cos(a)*std::cos(e),std::sin(e)};
        const auto w=allWeights(dir);
        const auto w2=allWeights(unit(dir+Vec{.0001f,0,.0001f}));
        float diff=0.f;
        for(int j=0;j<Views;++j) diff+=std::abs(w[j]-w2[j]);
        CHECK(diff<.01f);
    }
    for(int i=0;i<Views;++i) CHECK(allWeights(captures()[i].dir)[i]>.999f);
    const Palette p{{36,145,220},{151,220,245},{},{}},dark{{16,25,31},{34,49,48},{},{}};
    const auto day=canopy::lighting(s,p,{-.65f,-.35f,.85f},0,0,0);
    const auto night=canopy::lighting(s,dark,{0,-1,-.8f},0,0,0);
    const auto opposite=canopy::lighting(s,p,{.65f,.35f,.85f},0,0,0);
    Atlas a,b;
    auto start=std::chrono::steady_clock::now();
    a.start(96,s.seed);
    float last=0.f;
    while(!a.ready()) {a.advance(2);CHECK(a.progress()>=last);last=a.progress();}
    a.shade(day,0,a.height());
    const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
    CHECK(a.width()==1024 && a.height()==1024);
    b.start(96,s.seed);while(!b.ready()) b.advance(100);
    b.shade(day,0,b.height());
    CHECK(std::memcmp(a.pixels.data(),b.pixels.data(),a.pixels.size()*sizeof(canopy::Pixel))==0);
    const int tile=a.tileSize();
    for(int slot=0;slot<Forms*Views;++slot) {
        const int ox=slot%a.cols()*tile,oy=slot/a.cols()*tile;
        int covered=0,opaque=0,bright=0,mn=255,mx=0;
        for(int y=0;y<tile;++y)for(int x=0;x<tile;++x) {
            const auto p=a.pixels[std::size_t(oy+y)*a.width()+ox+x];
            if(x<2 || y<2 || x>=tile-2 || y>=tile-2) CHECK(!p.a);
            if(x==2 || y==2 || x==tile-3 || y==tile-3) CHECK(p.a<12);
            if(p.a>96) {++covered;bright+=p.r;mn=std::min(mn,int(p.r));mx=std::max(mx,int(p.r));}
            if(p.a>220)++opaque;
        }
        CHECK(covered>tile*tile/20 && opaque>tile*tile/40);
        CHECK(bright/covered>175 && mx-mn>12);
        for(float d:a.depths(slot/Views,slot%Views))CHECK(std::isfinite(d) && std::abs(d)<=.17f);
    }
    // Same density/alpha, direction changes illumination; night is muted.
    a.shade(opposite,0,a.height());
    int changed=0,solid=0;
    for(std::size_t i=0;i<a.pixels.size();++i) {
        CHECK(a.pixels[i].a==b.pixels[i].a);
        if(a.pixels[i].a>96) {
            ++solid;
            changed+=std::abs(int(a.pixels[i].r)-int(b.pixels[i].r))>3;
        }
    }
    CHECK(changed>solid/8);
    a.shade(night,0,a.height());
    for(std::size_t i=0;i<a.pixels.size();++i)CHECK(a.pixels[i].a==b.pixels[i].a && a.pixels[i].r<65);
    Refresh refresh;refresh.reset(day);
    CHECK(!refresh.advance(b,night,0));
    int uploads=0;
    for(int i=0;i<190;++i) uploads+=refresh.advance(b,night,1);
    CHECK(uploads==1 && !refresh.busy);
    const View v{{2266,-1271,30},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto r=range(s,2200,1600);
    const auto clear=groups(s,v,r,{},0,1),rain=groups(s,v,r,{},1,1);
    CHECK(clear.count>5 && clear.count<=MaxGroups && rain.count>=clear.count);
    auto rotated=v;rotated.right={0,1,0};rotated.up={0,0,-1};rotated.forward={-1,0,0};
    const auto turned=groups(s,rotated,r,{},0,1);
    CHECK(clear.count==turned.count);
    for(int i=0;i<clear.count;++i) {
        const auto& g=clear.items[i];
        const auto it=std::find_if(turned.items.begin(),turned.items.begin()+turned.count,[&](const auto& h){return length(g.center-h.center)==0.f;});
        CHECK(it!=turned.items.begin()+turned.count && it->alpha==g.alpha && it->form==g.form);
        for(auto w:weights(v.pos-g.center)) {
            const auto m=mesh(a,g,w),n=mesh(a,*it,w);
            for(int k=0;k<MeshVertices;++k) {
                CHECK(length(m.vertices[k].pos-n.vertices[k].pos)==0.f && m.vertices[k].alpha==n.vertices[k].alpha);
                CHECK(m.vertices[k].u>=0 && m.vertices[k].u<=1 && m.vertices[k].v>=0 && m.vertices[k].v<=1);
            }
            for(auto index:m.indices) CHECK(index<MeshVertices);
        }
    }
    // Stable locations and dimensions across vertical and horizontal travel,
    // and shared wind in absolute world coordinates.
    auto moved=v;moved.pos.x+=200;moved.pos.z+=180;
    const auto movedGroups=groups(s,moved,r,{},0,1);
    int matched=0;
    for(int i=0;i<clear.count;++i)for(int j=0;j<movedGroups.count;++j) {
        const auto x=clear.items[i],y=movedGroups.items[j];
        if(length(x.center-y.center)==0) {CHECK(x.scale==y.scale && x.form==y.form);++matched;}
    }
    CHECK(matched>3);
    canopy::Drift drift;
    for(int i=0;i<50;++i) drift.update(s,{1,-2,0},1);
    CHECK(std::abs(drift.worldX-50*s.windMoveScale)<.01 && std::abs(drift.worldY+100*s.windMoveScale)<.01);
    CHECK(groups(s,v,r,drift,0,0).count==0);
    s.hybridClouds=false;CHECK(groups(s,v,r,{},0,1).count==0);
    const auto base=canopy::mesh(s,v,r,{}),fallback=canopy::mesh(s,v,r,{},radius(s,r),0);
    for(int i=0;i<canopy::VertexCount;++i)CHECK(base.vertices[i].alpha==fallback.vertices[i].alpha);
    for(int size:{64,128}) {
        Atlas extra;extra.start(size,s.seed);while(!extra.ready())extra.advance(100);
        extra.shade(day,0,extra.height());
        CHECK(extra.pixels.size()==std::size_t(extra.width())*extra.height());
        for(int slot=0;slot<Forms*Views;++slot) {
            float u=0.f,vv=0.f;extra.uv(slot/Views,slot%Views,1.f,1.f,u,vv);
            CHECK(u>0.f && u<1.f && vv>0.f && vv<1.f);
            const int x=slot%extra.cols()*size,y=slot/extra.cols()*size;
            CHECK(extra.pixels[std::size_t(y)*extra.width()+x].a==0);
        }
    }
    std::cout<<"PASS: 78 coherent captures, seamless view weights, gutters, directional/night light, deterministic bake, world-cell invariance, fade and fallback; "<<ms<<" ms bake, "<<clear.count<<" clear / "<<rain.count<<" rainy groups\n";
}
