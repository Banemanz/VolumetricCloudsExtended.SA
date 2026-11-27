#include "../CloudCore.hpp"
#include <iostream>
#include <sstream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << "Line " << __LINE__ << ": " << #x << '\n'; std::exit(1); } } while (false)
using namespace vc;
bool close(float x,float y,float e=.0001f) { return std::abs(x-y)<e; }
int used(const Field& f) { return int(std::count_if(f.clouds.begin(),f.clouds.end(),[](auto& c){return c.used;})); }

int main() {
    Settings cfg;
    // Malformed/nonfinite settings cannot create invalid geometry or exceed fixed buffers.
    std::istringstream text("TargetProcClouds=99999999\nWindMoveScale=nan\nPuffsPerCloud=99\n"
        "MinSpawnZ=700\nMaxSpawnZ=-100\nSpawnRadiusMin=9999\nSpawnRadiusMax=100\n"
        "TimecycleTintStrength=.5 ; inline comment\nUseTimecycTint=FALSE\n"
        "UseGameFog=garbage\nCloudTextureNames=cloud2, cloud2, cloud4\n");
    load(text,cfg);
    CHECK(cfg.targetProcClouds==MaxClouds && cfg.puffsPerCloud==MaxPuffs);
    CHECK(close(cfg.windMoveScale,.9f) && cfg.maxSpawnZ>cfg.minSpawnZ);
    CHECK(cfg.spawnRadiusMax>cfg.spawnRadiusMin && close(cfg.timecycTintStrength,.5f));
    CHECK(!cfg.useTimecycTint && !cfg.useGameFog && cfg.textureNames.size()==2);
    std::ostringstream ini; write(ini,cfg);
    Settings roundtrip; std::istringstream input(ini.str()); load(input,roundtrip);
    CHECK(roundtrip.targetProcClouds==cfg.targetProcClouds && roundtrip.textureNames==cfg.textureNames);

    cfg=Settings{};
    View view{{0,0,100},{1,0,0},{0,0,1},{0,1,0},1800,1};
    const Range r=range(cfg,view.farClip,1000);
    CHECK(r.begin<r.end && r.end<view.farClip && r.spawnMax<r.end);
    CHECK(close(distanceAlpha(r,r.begin),1) && close(distanceAlpha(r,r.end),0));
    CHECK(close(distanceAlpha(r,(r.begin+r.end)*.5f),.5f));
    CHECK(range(cfg,300,2000).end<300);
    CHECK(range(cfg,0,1000).end==0);
    CHECK(close(weatherFactor(true,true,.5f,0),1));
    CHECK(close(weatherFactor(true,false,.75f,0),.25f));
    CHECK(close(weatherFactor(false,false,.3f,.7f),.7f));
    CHECK(target(cfg,1)==224 && target(cfg,0)==160);

    Field f,g; f.reset(cfg); g.reset(cfg);
    f.update(cfg,view,r,160,{},1); g.update(cfg,view,r,160,{},1);
    CHECK(used(f)==160);
    int nearCount=0;
    for (int i=0;i<160;++i) {
        const auto& c=f.clouds[i]; const auto& d=g.clouds[i];
        CHECK(close(length(c.position-d.position),0));
        CHECK(c.position.z>=cfg.minSpawnZ && c.position.z<=cfg.maxSpawnZ);
        CHECK(horizontalSq(c.position-view.pos)<=r.spawnMax*r.spawnMax+1.f);
        if (std::sqrt(horizontalSq(c.position-view.pos))<r.spawnMax*.6f) ++nearCount;
    }
    CHECK(nearCount>20);
    const Vec start=f.clouds[0].position;
    f.update(cfg,view,r,224,{1,0,0},1);
    CHECK(used(f)==224 && f.clouds[223].fade<.1f);
    CHECK(close(f.clouds[0].position.x-start.x,cfg.windMoveScale,.001f));
    for (int i=0;i<100;++i) f.update(cfg,view,r,160,{},1);
    CHECK(used(f)==160);
    auto fixed=f.clouds[0].position;
    view.forward={1,0,0}; view.right={0,-1,0};
    f.update(cfg,view,r,160,{},1);
    CHECK(close(length(f.clouds[0].position-fixed),0));
    f.update(cfg,view,r,160,{1,0,0},0);
    CHECK(close(length(f.clouds[0].position-fixed),0));

    // Equal simulated time gives equal advection at different frame rates.
    for(int i=0;i<50;++i) f.update(cfg,view,r,160,{1,0,0},1);
    for(int i=0;i<100;++i) g.update(cfg,view,r,160,{1,0,0},.5f);
    CHECK(close(f.clouds[0].position.x-g.clouds[0].position.x,cfg.windMoveScale,.01f));
    // Teleport: refill around the actual location, not a stale world anchor.
    view.pos={10000,10000,250}; f.update(cfg,view,r,160,{},1);
    CHECK(used(f)==160);
    for (auto& c:f.clouds) if (c.used) CHECK(horizontalSq(c.position-view.pos)<=r.spawnMax*r.spawnMax+2.f);

    Cloud c=f.clouds[0];
    Card cardsOut[MaxPuffs];
    for (int count=1;count<=MaxPuffs;++count) {
        cfg.puffsPerCloud=count;
        CHECK(cards(cfg,c,view,.5f,cardsOut)==count);
        float transmittance=1;
        for (int i=0;i<count;++i) {
            const auto& p=cardsOut[i];
            CHECK(finite(p.center) && finite(p.right) && finite(p.up));
            CHECK(close(dot(unit(p.right),unit(p.up)),0));
            CHECK(close(dot(unit(p.right),view.forward),0));
            CHECK(length(p.center-c.position)+cardBound(p)<=bound(c));
            transmittance*=1-p.alpha;
        }
        CHECK(close(transmittance,.25f));
    }
    cfg.renderMode=0;
    CHECK(cards(cfg,c,view,.5f,cardsOut)==3);
    for(int i=0;i<3;++i) CHECK(cardsOut[i].alpha>=0 && cardsOut[i].alpha<=.5f);

    // Maximum capacity stays bounded; retiring to zero fades out rather than popping.
    f.update(cfg,view,r,MaxClouds,{},1);
    CHECK(used(f)==MaxClouds);
    for(int i=0;i<100;++i) f.update(cfg,view,r,0,{},1);
    CHECK(used(f)==0);

    std::cout << "PASS: INI, clip range, weather transitions, density retirement, spawn distribution, "
                 "wind/timestep, determinism, teleport, geometry, opacity and capacity\n";
}
