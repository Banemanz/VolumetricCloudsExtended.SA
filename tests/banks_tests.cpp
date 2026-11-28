#include "../CloudBillboards.hpp"
#include "../CloudCanopyVolume.hpp"
#include <iostream>
#include <chrono>
#include <cstring>
#include <map>
#define CHECK(x) do {if(!(x)){std::cerr<<"Line "<<__LINE__<<": "<<#x<<'\n';std::exit(1);}}while(false)
using namespace vc;
int main() {
    // Compare the algebraic closest-corner shortcut with the original
    // four-corner loop, including the exceptional 8m near exclusion,
    // diagonal blend threshold and exact boundary cases.
    std::uint32_t rng=0x75c019e1u;
    auto random=[&](){rng=rng*1664525u+1013904223u;return float(rng>>8)/16777216.f;};
    const auto checkAxis=[&](auto axis,Vec lo,Vec hi,float d,float limitSq) {
        constexpr int a=decltype(axis)::value,j=(a+1)%3,k=(a+2)%3;
        bool expected=false;
        for(int q=0;q<4;++q) {
            Vec p=lo;banks::setComponent(p,a,d);
            if(q&1)banks::setComponent(p,j,banks::component(hi,j));
            if(q&2)banks::setComponent(p,k,banks::component(hi,k));
            const float mx=std::max({std::abs(p.x),std::abs(p.y),std::abs(p.z),.001f}),d2=dot(p,p);
            expected|=std::abs(d)>=.799999f*mx&&d2<=limitSq&&d2>63.98f;
        }
        const auto closest=banks::closestCornerDelta(lo,hi);
        CHECK(banks::planeCandidate<a>(closest,lo,hi,d,banks::angularThreshold<a>(closest),limitSq)==expected);
    };
    for(int i=0;i<100000;++i) {
        const float scale=i%3==0?10.f:i%3==1?500.f:8000.f;
        const Vec lo{(random()-.5f)*scale,(random()-.5f)*scale,(random()-.5f)*scale};
        const Vec hi=lo+Vec{random()*scale,random()*scale,random()*scale};
        const float d=(random()-.5f)*scale,limit=random()*scale;
        checkAxis(std::integral_constant<int,0>{},lo,hi,d,limit*limit);
        checkAxis(std::integral_constant<int,1>{},lo,hi,d,limit*limit);
        checkAxis(std::integral_constant<int,2>{},lo,hi,d,limit*limit);
        const banks::Brick box{{},{},lo,hi,0.f};
        const auto mask=banks::axisMask(box,{});
        for(int a=0;a<3;++a) {
            float highestMin=0.f,maxAxis=0.f;
            for(int t=0;t<3;++t) {
                const auto l=banks::component(lo,t),h=banks::component(hi,t);
                if(t==a)maxAxis=std::max(std::abs(l),std::abs(h));
                else highestMin=std::max(highestMin,std::max({l,-h,0.f}));
            }
            CHECK(bool(mask&(1u<<a))==(maxAxis>=highestMin*.80f));
        }
    }
    for(float d:{0.f,.0008f,7.999f,8.f,8.0001f,79.9999f,80.f})for(float limit:{63.98f,64.f,10000.f,500000.f}) {
        checkAxis(std::integral_constant<int,0>{},{0,0,0},{0,10,100},d,limit);
        checkAxis(std::integral_constant<int,1>{},{0,0,0},{10,0,100},d,limit);
        checkAxis(std::integral_constant<int,2>{},{0,0,0},{10,100,0},d,limit);
    }
    std::cout<<"PASS: 300000 closest-corner and axis-bound evaluations match the original predicates, including near exclusion\n";
    Settings s;
    CHECK(s.renderMode==2 && s.hybridClouds && s.cumulusDetail==0);
    canopy::Map m;m.build(512,s.seed);m.shadeWeatherLayers(s);
    banks::Volume b,other;
    const auto begin=std::chrono::steady_clock::now();
    b.build(s,m);
    const auto bake=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
    other.build(s,m);
    for(int i=0;i<2;++i) {
        CHECK(b.pixels[i].size()==std::size_t(banks::Width)*banks::Width*banks::Depth);
        CHECK(std::memcmp(b.pixels[i].data(),other.pixels[i].data(),b.pixels[i].size()*sizeof(canopy::Pixel))==0);
    }
    std::uint64_t total[2]{};
    int varied=0,clear=0,wet=0;
    for(int z=0;z<b.depth;++z)for(int y=0;y<b.width;++y)for(int x=0;x<b.width;++x) {
        const auto i=b.index(x,y,z);
        const auto a=b.pixels[0][i],c=b.pixels[1][i];
        if(z==0||z==b.depth-1)CHECK(a.a==0 && c.a==0);
        total[0]+=a.a;total[1]+=c.a;
        CHECK(a.a<=c.a);
        clear+=a.a>20;wet+=c.a>20;
        if(z>0&&z<b.depth-1)varied+=std::abs(int(b.pixels[0][i].a)-int(b.pixels[0][b.index(x,y,z-1)].a))>10;
        if(a.a>0||c.a>0) {
            const float u=(x+.5f)/b.width,v=(y+.5f)/b.width,w=(z+.5f)/b.depth;
            CHECK(b.potentiallyOccupied(u,v,w,u,v,w,.5f));
        }
    }
    CHECK(total[1]>total[0] && varied>1000 && wet>clear && clear>1000);
    for(int i=0;i<300;++i) {
        const float u=i*.013f,v=i*.027f,w=i/300.f;
        const auto a=b.sample(0,u,v,w),c=b.sample(0,u+1,v-1,w);
        CHECK(std::abs(int(a.a)-int(c.a))<=1);
        // Every trilinear sample with nonzero alpha must survive the conservative occupancy grid.
        if(a.a>0)CHECK(b.potentiallyOccupied(u,v,w,u,v,w,0.f));
    }
    CHECK(b.sample(0,.5f,.5f,-3).a==0 && b.sample(1,.5f,.5f,6).a==0);
    // Prefix lookup must be identical to a conservative block scan, even
    // for negative coordinates, multiple periods and weather endpoints.
    for(int i=0;i<240;++i) {
        const float u0=i*.013f-2.f,v0=i*.031f-3.f,w0=float(i%18)/20.f-.1f;
        const float u1=u0+float(i%43)*.057f,v1=v0+float(i%33)*.071f,w1=w0+.17f;
        for(float weather:{0.f,.5f,1.f}) {
            bool expected=false;
            for(int z=std::max(0,int(std::floor(w0*banks::OccupancyZ)));z<=std::min(banks::OccupancyZ-1,int(std::floor(w1*banks::OccupancyZ)));++z)
                for(int y=int(std::floor(v0*banks::OccupancyXY));y<=int(std::floor(v1*banks::OccupancyXY));++y)
                    for(int x=int(std::floor(u0*banks::OccupancyXY));x<=int(std::floor(u1*banks::OccupancyXY));++x) {
                        const auto j=(z*banks::OccupancyXY+canopy::wrap(y,banks::OccupancyXY))*banks::OccupancyXY+canopy::wrap(x,banks::OccupancyXY);
                        expected|=(weather<.999f&&b.occupied[0][j]>0)||(weather>.001f&&b.occupied[1][j]>0);
                    }
            CHECK(b.potentiallyOccupied(u0,v0,w0,u1,v1,w1,weather)==expected);
        }
    }
    // Exhaust all 65,536 wrapped rectangles and 36 Z intervals, in
    // each of the three weather cases, against the old prefix-sum path.
    banks::Volume prefixOnly;
    prefixOnly.occupiedPrefix=b.occupiedPrefix;
    std::size_t queries=0;
    for(int x=0;x<banks::OccupancyXY;++x)for(int y=0;y<banks::OccupancyXY;++y)
    for(int w=1;w<=banks::OccupancyXY;++w)for(int h=1;h<=banks::OccupancyXY;++h)
    for(int z=0;z<banks::OccupancyZ;++z)for(int top=z+1;top<=banks::OccupancyZ;++top) {
        const banks::Volume::CellBox box{{{{x,x+w},{y,y+h},{z,top}}}};
        for(float weather:{0.f,.001f,.5f,.999f,1.f}) {
            CHECK(b.potentiallyOccupied(box,weather)==prefixOnly.potentiallyOccupied(box,weather));
            ++queries;
        }
    }
    std::cout<<"Exact rectangular mask checks: "<<queries<<"\n";
    banks::Volume endpoints;
    endpoints.occupied[0][0]=255;
    endpoints.occupied[1][1]=255;
    endpoints.buildRectangleMasks();
    CHECK(!endpoints.rectangleMask[2].empty());
    CHECK(endpoints.potentiallyOccupied(0.f,0.f,0.f,0.f,0.f,0.f,.5f));
    CHECK(endpoints.potentiallyOccupied(1.f/16,0.f,0.f,1.f/16,0.f,0.f,.5f));
    CHECK(!endpoints.potentiallyOccupied(0.f,0.f,0.f,0.f,0.f,0.f,1.f));
    CHECK(!endpoints.potentiallyOccupied(1.f/16,0.f,0.f,1.f/16,0.f,0.f,0.f));
    // Actual underneath projection of the volume: test source-resolution
    // optical integration against an independent product of transmittance.
    canopyVolume::MaskPair projection;
    canopyVolume::project(s,b,b.width,projection);
    float variedShade=0.f;std::size_t visibleColumns=0;
    const float path=(s.maxSpawnZ-s.minSpawnZ)/b.depth/banks::ReferenceStep;
    for(int y=0;y<b.width;++y)for(int x=0;x<b.width;++x)for(int layer=0;layer<2;++layer) {
        double trans=1.,shade=0.;
        for(int z=0;z<b.depth;++z) {
            const auto p=b.pixels[layer][b.index(x,y,z)];
            const double t=std::pow(1.-p.a/255.,path);
            shade+=trans*(1.-t)*p.r;trans*=t;
        }
        const auto pixel=projection[layer][std::size_t(y)*b.width+x];
        CHECK(std::abs(pixel.a-canopy::byte(float(1.-trans)*255.f))<=1);
        if(pixel.a>12) {
            CHECK(std::abs(pixel.r-canopy::byte(float(shade/(1.-trans))))<=1);
            ++visibleColumns;variedShade+=255-pixel.r;
        }
        CHECK(pixel.r==pixel.g&&pixel.r==pixel.b);
    }
    CHECK(visibleColumns>1000&&variedShade/visibleColumns>10);
    canopyVolume::Relief relief;relief.build(s,b);
    CHECK(relief.ready&&relief.high-relief.low>5.f&&relief.low>=s.minSpawnZ&&relief.high<=s.maxSpawnZ);
    for(int y=-8;y<8;++y)for(int x=-8;x<8;++x)
        CHECK(relief.at(x,y)==relief.at(x+8,y-8));
    const auto full=cloudRange(s);float previousRange=0.f;
    for(int z=0;z<=1000;++z) {
        const auto local=canopyVolume::volumeRange(s,full,float(z),canopy::handover(s,float(z)));
        CHECK(local.end>=previousRange&&local.end<=full.end&&local.begin<local.end);
        previousRange=local.end;
    }
    CHECK(previousRange==full.end);
    std::cout<<"PASS: canopy shares exact column optical density, fixed periodic relief and continuous player-altitude local volume range\n";
    const View ground{{2266,-1271,30},{1,0,0},{0,0,1},{0,1,0},2200,1};
    const auto r=cloudRange(s);
    canopyVolume::Mesh reliefMesh,turnedRelief;
    const auto local=canopyVolume::volumeRange(s,r,ground.pos.z,0.f);
    canopyVolume::mesh(reliefMesh,relief,s,ground,r,{},.94f,.53f,0.f,&local);
    auto rotated=ground;rotated.forward=unit(Vec{.2f,1,.3f});
    rotated.right=unit(cross(rotated.forward,{0,0,1}));rotated.up=cross(rotated.right,rotated.forward);
    canopyVolume::mesh(turnedRelief,relief,s,rotated,r,{},.94f,.53f,0.f,&local);
    CHECK(!reliefMesh.indices.empty()&&!turnedRelief.indices.empty());
    std::size_t shared=0;
    for(const auto& vertex:reliefMesh.vertices) {
        CHECK(finite(vertex.pos)&&vertex.alpha>=0&&vertex.alpha<=1);
        CHECK(vertex.pos.z>=relief.low&&vertex.pos.z<=relief.high);
        for(const auto& v:turnedRelief.vertices)if(length(vertex.pos-v.pos)<.001f) {
            CHECK(vertex.pos.z==v.pos.z&&vertex.u==v.u&&vertex.v==v.v&&vertex.alpha==v.alpha);
            ++shared;break;
        }
    }
    CHECK(shared>10);
    for(const auto ind:reliefMesh.indices)CHECK(ind<reliefMesh.vertices.size());
    banks::Mesh raw,culled;
    for(int detail:{0,1})for(float alt:{30.f,300.f,523.f,673.f,690.f,1600.f})for(float pitch:{-.7f,0.f,.5f,1.5f})
       for(float az:{0.f,1.57f,3.14f}) {
        s.cumulusDetail=detail;auto v=ground;v.pos.z=alt;
        v.forward={std::sin(az)*std::cos(pitch),std::cos(az)*std::cos(pitch),std::sin(pitch)};
        v.right=unit(cross(v.forward,{0,0,1}));v.up=cross(v.right,v.forward);
        banks::buildMesh(raw,s,v,r,{},.94f,.53f);
        banks::buildMesh(culled,s,v,r,{},.94f,.53f,&b,0);
        CHECK(raw.indices.size()>=culled.indices.size());
        CHECK(culled.vertices.size()<=banks::MaxVertices);
        for(const auto& p:culled.vertices) {
            const float u=p.pos.x/canopy::tileWorldSize(s),vv=p.pos.y/canopy::tileWorldSize(s);
            CHECK(std::abs(u-p.u-std::round(u-p.u))<.001f);
            CHECK(std::abs(vv-p.v-std::round(vv-p.v))<.001f);
            CHECK(std::abs(p.w-(p.pos.z-s.minSpawnZ)/(s.maxSpawnZ-s.minSpawnZ))<.001f);
            CHECK(p.w>=-.0001f&&p.w<=1.0001f && p.alpha>=0 && p.alpha<=1 && finite(p.pos));
        }
        for(auto i:culled.indices)CHECK(i<culled.vertices.size());
    }
    s.cumulusDetail=0;
    auto flight=ground;flight.pos={-1639,1817,523};
    banks::buildMesh(raw,s,flight,r,{},.94f,.53f);
    banks::buildMesh(culled,s,flight,r,{},.94f,.53f,&b,0);
    CHECK(culled.indices.size()>0 && culled.indices.size()<raw.indices.size());
    const auto count=culled.indices.size()/3,unculled=raw.indices.size()/3;

    // Actual same-world-ray sampling comparison, not just phase or normal.
    // Intersect triangle(s) in a fixed world direction across camera turns;
    // use interpolated XYZ/alpha and trilinear density, just as the GPU does.
    const auto raySamples=[&](const banks::Mesh& mesh,const View& view,Vec ray) {
        std::vector<std::array<float,6>> hits;
        for(std::size_t i=0;i<mesh.indices.size();i+=3) {
            const auto& a=mesh.vertices[mesh.indices[i]];
            const auto& q=mesh.vertices[mesh.indices[i+1]];
            const auto& c=mesh.vertices[mesh.indices[i+2]];
            const Vec e=q.pos-a.pos,f=c.pos-a.pos,g=cross(ray,f);
            const float det=dot(e,g);if(std::abs(det)<.00001f)continue;
            const Vec t=view.pos-a.pos;const float u=dot(t,g)/det;
            const Vec h=cross(t,e);const float v=dot(ray,h)/det,d=dot(f,h)/det;
            if(u<0.f||v<0.f||u+v>1.f||d<0.f)continue;
            const float au=a.u+(q.u-a.u)*u+(c.u-a.u)*v;
            const float av=a.v+(q.v-a.v)*u+(c.v-a.v)*v;
            const float aw=a.w+(q.w-a.w)*u+(c.w-a.w)*v;
            const auto p=b.sample(0,au,av,aw);
            hits.push_back({d,au,av,aw,a.alpha+(q.alpha-a.alpha)*u+(c.alpha-a.alpha)*v,float(p.a)});
        }
        std::sort(hits.begin(),hits.end(),[](const auto& a,const auto& b){return a[0]>b[0];});
        return hits;
    };
    auto pose=ground;pose.pos={2164,-1265,490.f};
    pose.forward=unit(Vec{.96f,-.27f,-.04f});pose.right=unit(cross(pose.forward,{0,0,1}));pose.up=cross(pose.right,pose.forward);
    const auto ray=unit(pose.forward+pose.right*.024f+pose.up*.023f);
    canopy::Drift wind;wind.update(s,{1.2f,-.8f,0},2.7f);
    banks::buildMesh(raw,s,pose,r,wind,.94f,.53f,&b,0);
    const auto reference=raySamples(raw,pose,ray);
    CHECK(reference.size()>10);
    for(float deg:{-35.f,-20.f,-8.f,1.f,8.f,20.f,35.f}) {
        const float angle=deg*Pi/180.f;
        auto turned=pose;
        turned.forward=unit(pose.forward*std::cos(angle)+pose.right*std::sin(angle));
        turned.right=unit(cross(turned.forward,{0,0,1}));turned.up=cross(turned.right,turned.forward);
        turned.farClip=450.f;
        banks::buildMesh(culled,s,turned,r,wind,.94f,.53f,&b,0);
        const auto samples=raySamples(culled,turned,ray);
        CHECK(samples.size()==reference.size());
        for(std::size_t i=0;i<samples.size();++i)for(int j=0;j<6;++j)CHECK(std::abs(samples[i][j]-reference[i][j])<.002f);
    }
    for(float pitch:{-.30f,.30f}) {
        auto turned=pose;turned.forward=unit(pose.forward+pose.up*pitch);
        turned.right=unit(cross(turned.forward,{0,0,1}));turned.up=cross(turned.right,turned.forward);
        banks::buildMesh(culled,s,turned,r,wind,.94f,.53f,&b,0);
        const auto samples=raySamples(culled,turned,ray);
        CHECK(samples==reference);
    }
    std::cout<<"PASS: "<<reference.size()<<" identical world-ray intersections through yaw/pitch and camera clip changes\n";
    // Camera translation along one world ray must not drag its density
    // samples along with the camera. Restrict to a common distance interval
    // where the nested lattice is fully coarse at BOTH eye positions;
    // compare actual intersections and texture density, ignoring distance
    // fade and optical weight (which should change with the moved eye).
    for(Vec direction:{unit(Vec{1,.16f,.012f}),unit(Vec{1,.92f,.014f})}) {
        auto still=pose;still.forward=direction;
        still.right=unit(cross(still.forward,{0,0,1}));still.up=cross(still.right,still.forward);
        const auto line=unit(direction+still.right*.017f+still.up*.001f);
        banks::buildMesh(raw,s,still,r,wind,.94f,.53f,&b,0);
        const auto fixed=raySamples(raw,still,line);
        for(float advance:{17.f,61.f,143.f,231.f}) {
            auto moving=still;moving.pos=still.pos+line*advance;
            banks::buildMesh(culled,s,moving,r,wind,.94f,.53f,&b,0);
            const auto shifted=raySamples(culled,moving,line);
            int common=0;
            for(const auto& sample:fixed) {
                if(sample[0]<1700.f||sample[0]>2800.f||sample[4]<.02f)continue;
                const auto found=std::find_if(shifted.begin(),shifted.end(),[&](const auto& v) {
                    return std::abs(v[0]+advance-sample[0])<.02f;
                });
                CHECK(found!=shifted.end());
                CHECK(std::abs(sample[1]-(*found)[1]-std::round(sample[1]-(*found)[1]))<.0001f);
                CHECK(std::abs(sample[2]-(*found)[2]-std::round(sample[2]-(*found)[2]))<.0001f);
                CHECK(std::abs(sample[3]-(*found)[3])<.0001f&&std::abs(sample[5]-(*found)[5])<=1.f);
                ++common;
            }
            CHECK(common>5);
        }
    }
    for(float d=20.f;d<2000.f;d+=13.f) {
        float path=0;for(int i=-4;i<0;++i)path+=banks::planeWeight(i,20.f,d);
        CHECK(std::abs(path-80.f)<.001f);
    }
    Vec previousWeights=banks::axisWeights({1,.70f,0});
    for(float y=.701f;y<1.30f;y+=.001f) {
        const auto w=banks::axisWeights({1,y,0});
        CHECK(std::abs(w.x+w.y+w.z-1.f)<.00001f && length(w-previousWeights)<.015f);
        previousWeights=w;
    }
    std::cout<<"PASS: fixed world intersections during translation, continuous diagonal axis and distance LOD weights\n";
    banks::GpuMesh hardware;
    for(float h:{370.f,450.f,575.f,800.f})for(float w:{0.f,.5f,1.f}) {
        auto test=flight;test.pos.z=h;
        banks::buildMesh(raw,s,test,r,wind,.94f,.53f,&b,w);
        banks::buildMesh(hardware,s,test,r,wind,.94f,.53f,&b,w);
        CHECK(hardware.triangleCount()==raw.indices.size()/3&&hardware.vertices.size()==raw.vertices.size());
        for(std::size_t i=0;i<raw.vertices.size();++i)CHECK(length(hardware.vertices[i].pos-raw.vertices[i].pos)<.0001f);
        // Every retained world vertex lies inside its contiguous brick span.
        std::uint32_t next=0;
        for(const auto& span:hardware.spans) {
            CHECK(span.begin==next&&span.end>span.begin&&span.end<=hardware.vertices.size()&&span.end%4==0);
            for(std::uint32_t i=span.begin;i<span.end;++i) {
                const auto p=hardware.vertices[i].pos;
                CHECK(p.x>=span.lo.x&&p.x<=span.hi.x&&p.y>=span.lo.y&&p.y<=span.hi.y&&p.z>=span.lo.z&&p.z<=span.hi.z);
            }
            next=span.end;
        }
        CHECK(next==hardware.vertices.size());
        // Simulate both sides of several clip distances. The shortened
        // intervals may overinclude but must never exclude a point inside.
        for(float distance:{75.f,450.f,1800.f,12000.f})for(double sign:{-1.,1.}) {
            const auto f=test.forward;
            const double clip=double(distance)+double(dot(test.pos,f));
            banks::ClipPlane plane{{sign*f.x,sign*f.y,sign*f.z,-sign*clip},
                {std::abs(f.x),std::abs(f.y),std::abs(f.z),std::abs(clip)}};
            const auto selection=hardware.rangeFor(plane);
            CHECK(selection.begin<=selection.end&&selection.end<=hardware.vertices.size());
            CHECK(selection.begin%4==0&&selection.end%4==0);
            for(std::uint32_t i=0;i<hardware.vertices.size();++i)if(i<selection.begin||i>=selection.end) {
                const auto p=hardware.vertices[i].pos;
                CHECK(sign*(double(p.x)*f.x+double(p.y)*f.y+double(p.z)*f.z-clip)<0.);
            }
        }
    }
    std::cout<<"PASS: CPU and hardware source lattice and order are identical in 12 altitude/weather combinations\n";

    // Range must actually control volumetric geometry, not just the canopy.
    const auto farthest=[&](const banks::Mesh& mesh) {
        float d=0;for(const auto& p:mesh.vertices)if(p.alpha>0.f)d=std::max(d,length(p.pos-flight.pos));return d;
    };
    float previous=0;
    for(float range:{1800.f,4200.f,6000.f,8000.f,12000.f}) {
        s.cloudDrawDistance=range;auto rr=cloudRange(s);auto lod=banks::lod(s,rr);
        banks::buildMesh(culled,s,flight,rr,{},.94f,.53f,&b,0);
        const auto d=farthest(culled);
        CHECK(d>previous && d>lod.end*.85f && d<=lod.end+.01f);
        CHECK(lod.start>=std::min(range,5000.f) && lod.start>=range*.8f-.01f);
        CHECK(culled.vertices.size()<44000);
        previous=d;std::cout<<"Range "<<range<<": volume reaches "<<d<<", "<<culled.indices.size()/3<<" triangles\n";
    }
    s.cloudDrawDistance=12000;s.farBillboards=false;
    banks::buildMesh(culled,s,flight,cloudRange(s),{},.94f,.53f,&b,0);
    CHECK(farthest(culled)>11000);
    s.farBillboards=true;s.cloudDrawDistance=8000;
    const auto lod=banks::lod(s,cloudRange(s));CHECK(lod.start==6400.f&&lod.end==7200.f);
    for(float d:{6400.f,6500.f,6800.f,7100.f,7200.f})CHECK(std::abs(lod.volume(d)+lod.billboard(d)-1.f)<.0001f);
    for(float x=.25f;x<=.75f;x+=.01f)CHECK(std::abs(billboards::window(x)+billboards::window(1.f-x)-1.f)<.0001f);
    billboards::Atlas atlas;atlas.build(s,b);
    CHECK(atlas.pixels[0].size()==512*512 && atlas.pixels[1].size()==512*512);
    std::size_t countAlpha[2]{};
    for(int i=0;i<2;++i)for(auto p:atlas.pixels[i])countAlpha[i]+=p.a;
    CHECK(countAlpha[1]>countAlpha[0]&&countAlpha[0]>0);
    billboards::buildMesh(culled,s,flight,cloudRange(s),{},atlas,.94f,.53f,0);
    CHECK(culled.vertices.size()>0 && culled.vertices.size()<=billboards::MaxCards*4);
    for(const auto& p:culled.vertices)CHECK(p.u>=0.f&&p.u<=1.f&&p.v>=0.f&&p.v<=1.f&&p.alpha>0.f&&p.alpha<=1.f);
    const auto spriteCount=culled.vertices.size()/4;
    s.cloudDrawDistance=4200;
    billboards::buildMesh(culled,s,flight,cloudRange(s),{},atlas,.94f,.53f,0);
    CHECK(culled.indices.empty());
    s.hybridClouds=false;banks::buildMesh(culled,s,flight,r,{},.94f,.53f,&b,0);
    CHECK(culled.indices.empty());
    std::cout<<"PASS: deterministic density, world-ray invariance, bounded LOD and projected atlas; "<<bake<<" ms CPU volume bake; default flight "<<count<<" triangles vs "<<unculled<<" unculled, "<<spriteCount<<" distant cards\n";
}
