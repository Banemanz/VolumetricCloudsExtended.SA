#pragma once

// Platform-independent cloud simulation and geometry. No GTA RNG, globals or GPU resources.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace vc {
constexpr int MaxClouds = 512, MaxPuffs = 8, MaxCards = 12;
static_assert(MaxPuffs+2<=MaxCards);
constexpr float Pi = 3.14159265359f;
inline float clamp(float v, float lo, float hi) { return std::clamp(v, lo, hi); }
inline float sat(float v) { return std::isfinite(v) ? clamp(v, 0.f, 1.f) : 0.f; }
inline float smooth(float v) { v = sat(v); return v*v*(3.f-2.f*v); }
inline float mix(float a, float b, float t) { return a + (b-a)*t; }

struct Vec {
    float x{}, y{}, z{};
    Vec operator+(Vec b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec operator-(Vec b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec operator*(float f) const { return {x*f,y*f,z*f}; }
    Vec operator/(float f) const { return *this*(1.f/f); }
};
inline float dot(Vec a, Vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec cross(Vec a,Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float length(Vec a) { return std::sqrt(dot(a,a)); }
inline float horizontalSq(Vec a) { return a.x*a.x+a.y*a.y; }
inline Vec unit(Vec a) { const float d = length(a); return d > 1e-5f ? a/d : Vec{}; }
inline bool finite(Vec a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

struct Settings {
    int targetProcClouds = 120;
    float badWeatherDensityMultiplier = 1.4f;
    float minSpawnZ = 450.f, maxSpawnZ = 800.f;
    float spawnRadiusMin = 300.f, spawnRadiusMax = 2200.f;
    float renderMaxDist = 2200.f, respawnMaxDist = 2800.f; // legacy cards only
    float cloudDrawDistance = 8000.f, cloudFogStart = 1600.f, cloudFogEnd = 8000.f; // mode 2 independent atmosphere
    bool farHalfRes = true; // guarded distant-only premultiplied half-resolution pass
    bool farBillboards = true; // distant volume-projected impostors
    float billboardStartDistance = 5000.f, billboardStartRatio = .65f;
    float windMoveScale = .9f, minSpacing2D = 45.f, spawnFadeInSpeed = .02f;
    float quadFacingMin = .2f, farClipFadeRange = 450.f, farClipFadeMargin = 80.f;
    float timecycFarClipScale = 1.8f;
    int lightingMode = 0; // 0: native volumetric cloud colour; 1: custom tint/shade/haze
    bool useTimecycTint = true;
    float timecycTintStrength = .20f, fluffyCloudBottomBlend = 0.f;
    int colorBrightnessBoost = 64;
    bool useAdditionalTextures = false, includeOriginalTextureInRandom = true;
    std::vector<std::string> textureNames{"cloud2", "cloud3", "cloud4"};
    int renderMode = 2, puffsPerCloud = 5; // 0: crossed, 1: clustered, 2: connected 3D cloud bank + canopy
    float cumulusDensity = .95f, cumulusContrast = .65f, cumulusRimStrength = .85f;
    bool hybridClouds = true; // mode 2: continuous 3D density layer
    float cumulusNightLight = 1.f;
    float interiorHaze = .85f; // near-cloud density visibility in mode 2
    int cumulusDetail = 0; // 0: economy, 1: higher texture detail
    float sizeScale = 1.5f, opacity = 1.f, nearFadeDistance = 25.f;
    float bottomShade = 1.f, hazeStrength = 0.f;
    bool useGameFog = false;
    int seed = 17321;
};

inline std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    return first == std::string::npos ? "" : s.substr(first, s.find_last_not_of(" \t\r\n")-first+1);
}
inline bool number(const std::string& s, float& v) {
    char* end = nullptr;
    const float f = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end || !std::isfinite(f)) return false;
    v = f;
    return true;
}
inline bool boolean(const std::string& s, bool& v) {
    std::string lower = s;
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c += 'a'-'A';
    if (lower=="true" || lower=="yes" || lower=="on" || lower=="1") { v=true; return true; }
    if (lower=="false" || lower=="no" || lower=="off" || lower=="0") { v=false; return true; }
    return false;
}

// One table defines parsing, generated INI and validation. Legacy aliases still work.
#define VC_FLOATS(X) \
    X(BadWeatherDensityMultiplier,badWeatherDensityMultiplier,0.f,3.f) \
    X(MinSpawnZ,minSpawnZ,-500.f,5000.f) X(MaxSpawnZ,maxSpawnZ,-500.f,6000.f) \
    X(SpawnRadiusMin,spawnRadiusMin,0.f,20000.f) X(SpawnRadiusMax,spawnRadiusMax,50.f,20000.f) \
    X(CloudDrawDistance,cloudDrawDistance,300.f,12000.f) X(CloudFogStart,cloudFogStart,0.f,12000.f) X(CloudFogEnd,cloudFogEnd,0.f,12000.f) \
    X(BillboardStartDistance,billboardStartDistance,5000.f,12000.f) X(BillboardStartRatio,billboardStartRatio,.5f,.95f) \
    X(RenderMaxDist,renderMaxDist,100.f,20000.f) X(RespawnMaxDist,respawnMaxDist,150.f,25000.f) \
    X(WindMoveScale,windMoveScale,0.f,50.f) X(MinSpacing2D,minSpacing2D,0.f,2000.f) \
    X(SpawnFadeInSpeed,spawnFadeInSpeed,.001f,1.f) X(QuadFacingMin,quadFacingMin,0.f,1.f) \
    X(FarClipFadeRange,farClipFadeRange,10.f,10000.f) X(FarClipFadeMargin,farClipFadeMargin,0.f,5000.f) \
    X(TimecycFarClipScale,timecycFarClipScale,1.f,4.f) X(TimecycTintStrength,timecycTintStrength,0.f,1.f) \
    X(FluffyCloudBottomBlend,fluffyCloudBottomBlend,0.f,1.f) X(SizeScale,sizeScale,.1f,4.f) \
    X(Opacity,opacity,0.f,2.f) X(NearFadeDistance,nearFadeDistance,0.f,500.f) \
    X(BottomShade,bottomShade,0.f,1.f) X(HazeStrength,hazeStrength,0.f,1.f) \
    X(CumulusDensity,cumulusDensity,0.f,1.f) X(CumulusContrast,cumulusContrast,0.f,1.f) \
    X(CumulusRimStrength,cumulusRimStrength,0.f,1.f) X(CumulusNightLight,cumulusNightLight,0.f,2.f) X(InteriorHaze,interiorHaze,0.f,1.f)
#define VC_INTS(X) X(TargetProcClouds,targetProcClouds,0,MaxClouds) \
    X(ColorBrightnessBoost,colorBrightnessBoost,0,255) X(LightingMode,lightingMode,0,1) X(RenderMode,renderMode,0,2) X(CumulusDetail,cumulusDetail,0,1) \
    X(PuffsPerCloud,puffsPerCloud,1,MaxPuffs) X(Seed,seed,1,16777215)
#define VC_BOOLS(X) X(UseTimecycTint,useTimecycTint) X(UseAdditionalTextures,useAdditionalTextures) \
    X(IncludeOriginalTextureInRandom,includeOriginalTextureInRandom) X(UseGameFog,useGameFog) \
    X(HybridClouds,hybridClouds) X(FarBillboards,farBillboards) X(FarHalfRes,farHalfRes)

inline void load(std::istream& in, Settings& c) {
    std::string line;
    while (std::getline(in,line)) {
        const auto comment = line.find_first_of(";#");
        const auto eq = line.find('=');
        if (eq==std::string::npos || eq>=comment) continue;
        auto key = trim(line.substr(0,eq));
        auto val = trim(line.substr(eq+1,comment==std::string::npos ? comment : comment-eq-1));
        if (key=="TimecycleTintStrength") key="TimecycTintStrength";
        if (key=="TimecycTintSkyInfluence") key="FluffyCloudBottomBlend";
        float f;
#define READ_FLOAT(name,member,lo,hi) if (key==#name && number(val,f)) c.member=clamp(f,lo,hi);
        VC_FLOATS(READ_FLOAT)
#undef READ_FLOAT
#define READ_INT(name,member,lo,hi) if (key==#name && number(val,f)) c.member=static_cast<int>(clamp(f,float(lo),float(hi)));
        VC_INTS(READ_INT)
#undef READ_INT
#define READ_BOOL(name,member) if (key==#name) boolean(val,c.member);
        VC_BOOLS(READ_BOOL)
#undef READ_BOOL
        if (key=="CloudTextureNames") {
            c.textureNames.clear();
            std::size_t begin=0;
            while (begin<=val.size() && c.textureNames.size()<15) {
                const auto end=val.find(',',begin);
                const auto name=trim(val.substr(begin,end==std::string::npos ? end : end-begin));
                if (!name.empty() && name.size()<=31 && std::find(c.textureNames.begin(),c.textureNames.end(),name)==c.textureNames.end())
                    c.textureNames.push_back(name);
                if (end==std::string::npos) break;
                begin=end+1;
            }
        }
    }
    c.cloudFogEnd=std::min(c.cloudFogEnd,c.cloudDrawDistance);
    c.cloudFogStart=std::min(c.cloudFogStart,c.cloudFogEnd);
    c.maxSpawnZ=std::max(c.maxSpawnZ,c.minSpawnZ+10.f);
    c.spawnRadiusMin=std::min(c.spawnRadiusMin,c.spawnRadiusMax-1.f);
    c.respawnMaxDist=std::max(c.respawnMaxDist,c.renderMaxDist+50.f);
}
inline void write(std::ostream& out, const Settings& c) {
    // Fresh files expose the useful controls, not every historical experiment.
    // Old INIs are still read, never overwritten; legacy card tuning remains
    // supported via the parser and documented in README.
    out << "; VolumetricCloudsExtended - restart the game after editing.\n"
           "; Distances and heights are in GTA world units (roughly metres).\n"
           "; The defaults are intended for older GPUs.\n\n";
#define FIELD(name,member,comment) out << "; " comment "\n" #name "=" << c.member << "\n";
    out << "[Renderer]\n";
    FIELD(RenderMode,renderMode,"2 = 3D cloud banks (recommended), 1 = clustered classic clouds, 0 = classic crossed clouds.")
    FIELD(HybridClouds,hybridClouds,"Mode 2: 1 = directional canopy + nearby 3D; flight range expands from 70% of canopy height. 0 = inexpensive flat canopy.")
    FIELD(FarHalfRes,farHalfRes,"Economy: half-resolution distant clouds beyond scene far clip (compatible targets only).")
    FIELD(CumulusDetail,cumulusDetail,"Mode 2: 0 = economy (recommended); 1 = finer textures and more samples.")
    out << "\n[Clouds]\n";
    FIELD(TargetProcClouds,targetProcClouds,"How much of the sky is covered (0-512; 0 disables all custom clouds). Classic modes use this as the group count.")
    FIELD(BadWeatherDensityMultiplier,badWeatherDensityMultiplier,"How much more cloud cover appears in bad weather (1 = unchanged; 0 = none).")
    FIELD(MinSpawnZ,minSpawnZ,"Bottom of the cloud layer above sea level.")
    FIELD(MaxSpawnZ,maxSpawnZ,"Top of the cloud layer above sea level. Must be higher than the bottom.")
    FIELD(SizeScale,sizeScale,"Horizontal pattern scale (1 = original, 1.5 = broader, fewer formations).")
    FIELD(WindMoveScale,windMoveScale,"How fast clouds drift with the game's wind (0 = stationary).")
    FIELD(Opacity,opacity,"Overall cloud thickness (0 = invisible, 1 = normal, 2 = thicker).")
    FIELD(Seed,seed,"Change this number for a different repeatable cloud pattern.")
    out << "\n[Atmosphere]\n";
    FIELD(CloudDrawDistance,cloudDrawDistance,"Mode 2: visibility of 3D clouds and distant layers, independent of the world.")
    FIELD(FarBillboards,farBillboards,"Mode 2: enable directional cloud canopy and far 3D impostors (1 = recommended; 0 = legacy hybrid).")
    FIELD(BillboardStartDistance,billboardStartDistance,"Minimum distance before the far billboard transition may start (at least 5000).")
    FIELD(BillboardStartRatio,billboardStartRatio,"Also start no sooner than this fraction of CloudDrawDistance (0.65 = 65%).")
    FIELD(CloudFogStart,cloudFogStart,"Mode 2: distance where clouds begin to blend into the sky.")
    FIELD(CloudFogEnd,cloudFogEnd,"Mode 2: distance where cloud colour fully blends into the sky. Set equal to CloudFogStart to disable colour fog.")
    FIELD(CumulusContrast,cumulusContrast,"Mode 2: cloud shading strength (0 = flat, 1 = stronger shadows).")
    FIELD(CumulusNightLight,cumulusNightLight,"Mode 2: cloud brightness at night (0 = black, 1 = normal, 2 = brighter).")
    out << "\n[ClassicModes]\n; Only used with RenderMode=0 or 1 (or if procedural textures cannot load).\n";
    FIELD(PuffsPerCloud,puffsPerCloud,"Mode 1: number of overlapping puffs per group (1-8).")
    FIELD(UseAdditionalTextures,useAdditionalTextures,"Opt in to extra textures from particle.txd; configure the names below first.")
#undef FIELD
    out << "; Comma-separated extra texture names; missing ones are ignored.\nCloudTextureNames=";
    for (std::size_t i=0;i<c.textureNames.size();++i) out << (i?",":"") << c.textureNames[i];
    out << '\n';
}
#undef VC_FLOATS
#undef VC_INTS
#undef VC_BOOLS

// The native high/volumetric cloud pass does NOT use the low-cloud or fluffy
// bottom RGB values. It averages all six sky-top/bottom channels, adds 64,
// clamps to 255, and passes that neutral luminance to every card vertex.
// Verified in the SA 1.0 US IDA dump at 0x716693..0x7166EB.
struct RGB { float r,g,b; };
struct Palette { RGB skyTop,skyBottom,lowCloud,fluffyBottom; };
inline RGB mixRGB(RGB a, RGB b, float t) {
    return {mix(a.r,b.r,t),mix(a.g,b.g,t),mix(a.b,b.b,t)};
}
inline RGB scaleRGB(RGB c,float v) { return {c.r*v,c.g*v,c.b*v}; }
inline RGB nativeCloudColour(const Settings& s,const Palette& p) {
    const int sum=int(p.skyTop.r)+int(p.skyTop.g)+int(p.skyTop.b)+
                  int(p.skyBottom.r)+int(p.skyBottom.g)+int(p.skyBottom.b);
    const float grey=float(std::clamp(sum/6+s.colorBrightnessBoost,0,255));
    return {grey,grey,grey};
}
struct CardColours { RGB top,bottom; };
class CloudLighting {
    RGB base,sky;
    bool custom;
    float shade,haze;
public:
    CloudLighting(const Settings& s,const Palette& p)
        : base(nativeCloudColour(s,p)),sky(p.skyBottom),custom(s.lightingMode==1),
          shade(s.bottomShade),haze(s.useGameFog?0.f:s.hazeStrength) {
        if (custom && s.useTimecycTint)
            base=mixRGB(base,mixRGB(p.lowCloud,p.fluffyBottom,s.fluffyCloudBottomBlend),s.timecycTintStrength);
    }
    CardColours colours(float puffShade,float distanceNorm) const {
        // Native mode bypasses all custom colour multipliers (even stale values
        // in an older INI). The original texture provides its own shading.
        if (!custom) return {base,base};
        const RGB lit=scaleRGB(base,mix(shade,1.f,sat(puffShade)));
        const RGB top=mixRGB(lit,sky,haze*smooth(distanceNorm));
        return {top,scaleRGB(top,shade)};
    }
};

struct View { Vec pos, right, up, forward; float farClip, nearClip; };
struct Range { float end, begin, spawnMin, spawnMax, recycle; };
inline Range range(const Settings& c, float cameraFar, float timecycFar) {
    // Camera projection is a hard ceiling. Timecycle distance may only reduce it, never extend it.
    if (!std::isfinite(cameraFar) || cameraFar<=1.f) return {};
    float extent=std::min(c.renderMaxDist,cameraFar);
    if (std::isfinite(timecycFar) && timecycFar>1.f)
        extent=std::min(extent,timecycFar*c.timecycFarClipScale);
    const float end=extent-std::min(c.farClipFadeMargin,extent*.15f);
    const float begin=std::max(0.f,end-std::min(c.farClipFadeRange,end*.65f));
    const float radius=std::min(c.spawnRadiusMax,end*.90f);
    const float inner=radius*(c.spawnRadiusMin/c.spawnRadiusMax);
    return {end,begin,inner,radius,std::min(c.respawnMaxDist,std::max(end*1.35f,end+300.f*c.sizeScale))};
}
// Modern cloud visibility is not limited by the scene fog or far plane.
// D3D9 projects far clouds onto the existing depth buffer's far boundary.
inline Range cloudRange(const Settings& s) {
    const float end=s.cloudDrawDistance;
    return {end,end*.75f,0.f,end,end};
}
inline float cloudFogFactor(const Settings& s,float distance) {
    const float width=s.cloudFogEnd-s.cloudFogStart;
    return width>1.f?1.f-smooth((distance-s.cloudFogStart)/width):1.f;
}
inline float distanceAlpha(const Range& r, float distance) {
    return r.end>r.begin ? smooth((r.end-distance)/(r.end-r.begin)) : 0.f;
}
inline float weatherFactor(bool oldBad, bool newBad, float t, float rain) {
    return std::max(mix(oldBad?1.f:0.f,newBad?1.f:0.f,sat(t)),sat(rain));
}
inline int target(const Settings& c,float weather) {
    return std::clamp(int(c.targetProcClouds*mix(1.f,c.badWeatherDensityMultiplier,sat(weather))+.5f),0,MaxClouds);
}

struct RNG {
    std::uint32_t state=1;
    std::uint32_t next() {
        state ^= state<<13; state ^= state>>17; state ^= state<<5; return state;
    }
    float sample(float a,float b) { return mix(a,b,float(next()>>8)*(1.f/16777216.f)); }
};
struct Puff { Vec offset; float width, height, roll, shade; };
struct Cloud {
    bool used{};
    Vec position{}, size{};
    float fade{}, alpha{};
    std::uint32_t textureKey{};
    std::array<Puff,MaxPuffs> puffs{};
};
inline float bound(const Cloud& c) { return length(c.size)*.85f; }

class Field {
    RNG rng;
    bool initialized=false;
    Vec previous{};
    void spawn(Cloud& c, const Settings& s, const View& v, const Range& r, bool outer) {
        // Sample by area, not radius. For replacement, place toward the perimeter,
        // where distance fade masks births. Spacing is enforced only at birth.
        const float inner=outer ? std::max(r.spawnMin,r.spawnMax*.7f) : r.spawnMin;
        Vec best{};
        float bestSpacing=-1.f;
        for (int attempt=0;attempt<8;++attempt) {
            const float angle=rng.sample(0.f,2.f*Pi);
            const float radius=std::sqrt(rng.sample(inner*inner,r.spawnMax*r.spawnMax));
            // Absolute world altitude: flying upward does not drag the cloud layer with you.
            // Bias towards visible altitude when clip is short, but never violate the configured layer.
            float low=std::max(v.pos.z-r.end*.5f,s.minSpawnZ);
            float high=std::min(v.pos.z+r.end*.5f,s.maxSpawnZ);
            if (high-low < (s.maxSpawnZ-s.minSpawnZ)*.1f) {
                low=s.minSpawnZ; high=s.maxSpawnZ; // never flatten the cloud layer at extreme altitudes
            }
            const Vec p{v.pos.x+radius*std::cos(angle),v.pos.y+radius*std::sin(angle),rng.sample(low,high)};
            float spacing=1e30f;
            for (const auto& other : clouds)
                if (&other!=&c && other.used)
                    spacing=std::min(spacing,horizontalSq(p-other.position));
            if (spacing>bestSpacing) { bestSpacing=spacing; best=p; }
            if (spacing>=s.minSpacing2D*s.minSpacing2D) break;
        }
        const float scale=rng.sample(1.f,5.f)*s.sizeScale;
        c={};
        c.used=true;
        c.position=best;
        c.size={scale*rng.sample(30.f,100.f),scale*rng.sample(30.f,100.f),scale*rng.sample(20.f,40.f)};
        c.alpha=rng.sample(36.f,128.f)/255.f;
        c.textureKey=rng.next();
        for (auto& p : c.puffs) {
            p.offset={c.size.x*rng.sample(-.27f,.27f),c.size.y*rng.sample(-.27f,.27f),c.size.z*rng.sample(-.25f,.25f)};
            p.width=std::max(c.size.x,c.size.y)*rng.sample(.44f,.80f);
            p.height=c.size.z*rng.sample(.65f,1.2f);
            p.roll=rng.sample(-.28f,.28f);
            p.shade=sat(.5f+p.offset.z/std::max(c.size.z,1.f));
        }
        // A central puff anchors the silhouette; random satellites add depth.
        c.puffs[0].offset={};
        c.puffs[0].width=std::max(c.size.x,c.size.y)*.85f;
        c.puffs[0].height=c.size.z;
        c.puffs[0].shade=.5f;
    }
public:
    std::array<Cloud,MaxClouds> clouds{};
    void reset(const Settings& s) {
        clouds={}; initialized=false; rng.state=std::uint32_t(std::max(1,s.seed));
    }
    void update(const Settings& s,const View& v,const Range& r,int desired,Vec wind,float timeStep) {
        if (!finite(v.pos) || r.end<=1.f) return;
        const float dt=std::isfinite(timeStep)?clamp(timeStep,0.f,5.f):0.f; // native timestep units (50/s)
        const float fadeStep=dt*s.spawnFadeInSpeed;
        if (!initialized || horizontalSq(v.pos-previous)>r.recycle*r.recycle || std::abs(v.pos.z-previous.z)>r.recycle) {
            reset(s); initialized=true;
            for (int i=0;i<desired;++i) { spawn(clouds[i],s,v,r,false); clouds[i].fade=1.f; }
        }
        previous=v.pos;
        if (!finite(wind)) wind={};
        for (int i=0;i<MaxClouds;++i) {
            auto& c=clouds[i];
            if (c.used) c.position=c.position+wind*(dt*s.windMoveScale);
            if (i>=desired) {
                if (c.used) { c.fade=std::max(0.f,c.fade-fadeStep); if (c.fade<=0.f) c={}; }
                continue;
            }
            // Only recycle after the entire cloud is outside the visible region.
            if (!c.used) spawn(c,s,v,r,false);
            else {
                const float limit=std::max(r.recycle,r.end+bound(c));
                if (horizontalSq(c.position-v.pos)>limit*limit) spawn(c,s,v,r,true);
            }
            c.fade=std::min(1.f,c.fade+fadeStep);
        }
    }
};

struct Card { Vec center, right, up; float alpha, shade; std::uint32_t textureKey; Vec lightOffset{}; };
inline float cloudAlpha(const Settings& s,const Cloud& c,const View& v,const Range& r) {
    const float dist=length(c.position-v.pos);
    // Fade before the cloud's far edge reaches the range. Near fade reduces cockpit/aircraft intersections.
    const float edge=dist+bound(c)*.45f;
    const float nearFade=s.nearFadeDistance>0.f ? smooth(dist/(s.nearFadeDistance+bound(c)*.30f)) : 1.f;
    const float density=c.alpha;
    return sat(density*s.opacity*smooth(c.fade)*distanceAlpha(r,edge)*nearFade);
}
inline int cards(const Settings& s,const Cloud& c,const View& v,float alpha,Card* out) {
    if (s.renderMode==0) {
        const Vec direction=unit(c.position-v.pos);
        out[0]={c.position,{c.size.x*.5f,0,0},{0,0,c.size.z*.5f},alpha*std::max(s.quadFacingMin,std::abs(direction.y)),.5f,c.textureKey};
        out[1]={c.position,{c.size.x*.5f,0,0},{0,c.size.y*.5f,0},alpha*std::max(s.quadFacingMin,std::abs(direction.z)),.5f,c.textureKey};
        out[2]={c.position,{0,c.size.y*.5f,0},{0,0,c.size.z*.5f},alpha*std::max(s.quadFacingMin,std::abs(direction.x)),.5f,c.textureKey};
        return 3;
    }
    // Mode 1: retain clustered native textures without screen-facing cards.
    // A fixed crossed core guarantees coverage even with PuffsPerCloud=1.
    // Each extra puff is a single world-fixed, deterministically tilted card.
    const int puffs=std::clamp(s.puffsPerCloud,1,MaxPuffs);
    const float yaw=float((c.textureKey>>8)&1023u)*(2.f*Pi/1024.f);
    const Vec x{std::cos(yaw),std::sin(yaw),0},y{-x.y,x.x,0},z{0,0,1};
    const auto& core=c.puffs[0];
    const float width=core.width*.5f,height=core.height*.5f;
    out[0]={c.position,x*width,z*height,0,core.shade,c.textureKey};
    out[1]={c.position,y*width,z*height,0,core.shade,c.textureKey};
    out[2]={c.position,x*(c.size.x*.5f),y*(c.size.y*.5f),0,core.shade,c.textureKey};
    const int count=puffs+2;
    std::array<float,MaxCards> weights{};
    // Use the actual eye rather than camera rotation, for visibility only.
    const auto dir=unit(v.pos-c.position);
    const float dx=dot(x,dir),dy=dot(y,dir),dz=dir.z;
    const float coreSum=dx*dx+dy*dy+dz*dz;
    weights[0]=coreSum>1e-5f?dy*dy/coreSum:1.f/3.f;
    weights[1]=coreSum>1e-5f?dx*dx/coreSum:1.f/3.f;
    weights[2]=coreSum>1e-5f?dz*dz/coreSum:1.f/3.f;
    float sum=1.f;
    for(int i=1;i<puffs;++i) {
        const auto& p=c.puffs[i];
        const float a=yaw+i*2.39996323f+p.roll;
        const Vec right{std::cos(a),std::sin(a),0};
        const Vec across{-right.y,right.x,0};
        // Alternate upright and slanted textures to provide depth without
        // exactly perpendicular quads disappearing together.
        const float tilt=(i%3==0?0.f:(i%3==1?.69f:-.69f));
        const Vec up=z*std::cos(tilt)+across*std::sin(tilt);
        const Vec center=c.position+p.offset;
        const auto normal=cross(right,up);
        const float facing=std::abs(dot(normal,unit(v.pos-center)));
        weights[i+2]=facing*facing;
        sum+=weights[i+2];
        out[i+2]={center,right*(p.width*.5f),up*(p.height*.5f),0,p.shade,c.textureKey};
    }
    // Normalize the ideal full-mask transmittance, as in the old mode 1.
    for(int i=0;i<count;++i)
        out[i].alpha=weights[i]>0.f?1.f-std::pow(1.f-sat(alpha),2.f*weights[i]/sum):0.f;
    return count;
}
inline float cardBound(const Card& c) { return std::sqrt(dot(c.right,c.right)+dot(c.up,c.up)); }
inline float viewDepth(const View& v,Vec p) { return dot(p-v.pos,v.forward); }
} // namespace vc
