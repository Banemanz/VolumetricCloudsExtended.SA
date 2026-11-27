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
    CHECK(!roundtrip.useAdditionalTextures && roundtrip.farBillboards && roundtrip.billboardStartDistance==5000.f && roundtrip.billboardStartRatio==.8f);

    // Stock cloud vertices use neutral sky luminance, not low-cloud/underside RGB.
    // An old generated INI must not silently restore the overly dark/tinted look.
    Settings legacy;
    std::istringstream oldIni("UseTimecycTint=1\nTimecycTintStrength=1\nBottomShade=.75\nHazeStrength=.35\n");
    load(oldIni,legacy);
    const Palette dawn{{156,255,255},{255,170,90},{60,30,20},{40,20,10}};
    const Palette clear{{60,128,197},{192,218,236},{40,40,40},{20,20,20}};
    const Palette night{{5,6,12},{14,15,20},{0,0,0},{0,0,0}};
    auto bright=CloudLighting(legacy,dawn).colours(0.f,1.f);
    CHECK(legacy.lightingMode==0 && bright.top.r==255 && bright.top.g==255 && bright.top.b==255);
    CHECK(bright.bottom.r==255 && bright.bottom.g==255 && bright.bottom.b==255);
    const auto clearColour=CloudLighting(legacy,clear).colours(.5f,.9f);
    CHECK(clearColour.top.r==235 && clearColour.top.g==235 && clearColour.top.b==235);
    CHECK(clearColour.bottom.r==clearColour.top.r && clearColour.bottom.g==clearColour.top.g && clearColour.bottom.b==clearColour.top.b);
    const auto nightColour=CloudLighting(legacy,night).colours(0,1);
    CHECK(nightColour.top.r==76 && nightColour.top.g==76 && nightColour.top.b==76);
    // Explicit opt-in to old tint/shading must remain available.
    std::istringstream customMode("LightingMode=1\n");
    load(customMode,legacy);
    auto custom=CloudLighting(legacy,dawn).colours(.5f,0.f);
    CHECK(legacy.lightingMode==1 && close(custom.top.r,52.5f));
    CHECK(close(custom.bottom.r,39.375f) && custom.top.r!=custom.top.b);
    CHECK(CloudLighting(legacy,dawn).colours(.5f,1.f).top.r>custom.top.r);
    legacy.useGameFog=true;
    CHECK(close(CloudLighting(legacy,dawn).colours(.5f,1.f).top.r,custom.top.r));

    cfg=Settings{};
    View view{{0,0,100},{1,0,0},{0,0,1},{0,1,0},1800,1};
    const Range r=range(cfg,view.farClip,1000);
    CHECK(r.begin<r.end && r.end<view.farClip && r.spawnMax<r.end);
    CHECK(close(distanceAlpha(r,r.begin),1) && close(distanceAlpha(r,r.end),0));
    CHECK(close(distanceAlpha(r,(r.begin+r.end)*.5f),.5f));
    CHECK(range(cfg,300,2000).end<300);
    const auto independent=cloudRange(cfg);
    CHECK(independent.end==8000.f && independent.end>view.farClip);
    CHECK(cloudFogFactor(cfg,0)==1 && cloudFogFactor(cfg,cfg.cloudFogStart)==1);
    CHECK(close(cloudFogFactor(cfg,(cfg.cloudFogStart+cfg.cloudFogEnd)*.5f),.5f));
    CHECK(cloudFogFactor(cfg,cfg.cloudDrawDistance)==0);
    std::istringstream fogIni("CloudDrawDistance=6000\nCloudFogEnd=99999\nCloudFogStart=99999\n");
    Settings changed;load(fogIni,changed);
    CHECK(cloudRange(changed).end==6000 && changed.cloudFogStart==6000 && cloudFogFactor(changed,6000)==1);
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
    Card cardsOut[MaxCards],moved[MaxCards];
    const auto verifyFixed=[&](int mode,int puffs){
        cfg.renderMode=mode;cfg.puffsPerCloud=puffs;
        const int n=cards(cfg,c,view,.5f,cardsOut);
        CHECK(n==(mode==0?3:puffs+2) && n<=MaxCards);
        for(int z=-3;z<=3;++z)for(int x=-2;x<=2;++x)for(int y=-2;y<=2;++y) {
            View changed=view;
            changed.pos=c.position+Vec{float(x)*200,float(y)*200,float(z)*120};
            changed.forward={0,0,1};changed.up={0,1,0};changed.right={-1,0,0};
            CHECK(cards(cfg,c,changed,.5f,moved)==n);
            float trans=1.f;
            for(int i=0;i<n;++i) {
                const auto& p=cardsOut[i];const auto& q=moved[i];
                CHECK(finite(q.center)&&finite(q.right)&&finite(q.up));
                CHECK(close(dot(unit(q.right),unit(q.up)),0));
                CHECK(length(q.center-c.position)+cardBound(q)<=bound(c));
                CHECK(length(p.center-q.center)==0 && length(p.right-q.right)==0 && length(p.up-q.up)==0);
                CHECK(p.textureKey==q.textureKey && p.shade==q.shade);
                CHECK(q.alpha>=0.f&&q.alpha<=1.f);
                trans*=1.f-q.alpha;
                // A pure camera rotation must leave even the alpha untouched.
                View rotated=changed;rotated.right={1,0,0};rotated.up={0,0,1};rotated.forward={0,1,0};
                Card rotatedCards[MaxCards];cards(cfg,c,rotated,.5f,rotatedCards);
                CHECK(q.alpha==rotatedCards[i].alpha);
            }
            if(mode==1)CHECK(close(trans,.25f));
        }
    };
    verifyFixed(0,1);
    for(int count=1;count<=MaxPuffs;++count)verifyFixed(1,count);
    for(const auto& c:g.clouds)if(c.used) {
        cfg.renderMode=1;cfg.puffsPerCloud=MaxPuffs;
        CHECK(cards(cfg,c,view,.5f,cardsOut)==MaxPuffs+2);
        for(int i=0;i<MaxPuffs+2;++i)
            CHECK(length(cardsOut[i].center-c.position)+cardBound(cardsOut[i])<=bound(c));
    }
    cfg.renderMode=0;

    // Above the altitude band, initial/recycled clouds retain vertical variation.
    Field highField; highField.reset(cfg);
    View highView=view; highView.pos.z=10000.f;
    highField.update(cfg,highView,r,160,{},1);
    float minZ=cfg.maxSpawnZ,maxZ=cfg.minSpawnZ;
    for (auto& cloud:highField.clouds) if(cloud.used) {
        minZ=std::min(minZ,cloud.position.z); maxZ=std::max(maxZ,cloud.position.z);
    }
    CHECK(minZ>=cfg.minSpawnZ && maxZ<=cfg.maxSpawnZ && maxZ-minZ>100.f);

    // Maximum capacity stays bounded; retiring to zero fades out rather than popping.
    f.update(cfg,view,r,MaxClouds,{},1);
    CHECK(used(f)==MaxClouds);
    for(int i=0;i<100;++i) f.update(cfg,view,r,0,{},1);
    CHECK(used(f)==0);

    std::cout << "PASS: INI, native/custom lighting, clip range, weather transitions, density retirement, spawn distribution, "
                 "wind/timestep, determinism, teleport, geometry, opacity and capacity\n";
}
