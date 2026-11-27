// GTA SA 1.0 US / Plugin-SDK. Replaces only the volumetric-cloud call in RenderEffects.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin.h"
#include "CClouds.h"
#include "CGame.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CTimer.h"
#include "CTxdStore.h"
#include "CScene.h"
#include "CloudCore.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {
constexpr std::uintptr_t CloudCall = 0x53E1B4;
constexpr std::uintptr_t NativeRenderer = 0x716380;
constexpr int BatchVertices = 480; // multiple of 6; comfortably below the RW Im3D buffer limit
vc::Settings settings;
vc::Field field;
bool haveFrame = false;
unsigned int lastFrame = 0;

std::filesystem::path ModulePath() {
    HMODULE module = nullptr;
    wchar_t path[32768]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&settings), &module)) {
        const DWORD len = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
        if (len > 0 && len < std::size(path))
            return std::filesystem::path(path).parent_path();
    }
    return L".";
}

void Log(const char* message) {
    std::ofstream out(ModulePath()/L"VolumetricCloudsExtended.log", std::ios::app);
    if (out) out << message << '\n';
}

void LoadIni() {
    const auto path = ModulePath()/L"VolumetricCloudsExtended.ini";
    std::ifstream in(path);
    if (in) {
        vc::load(in,settings);
        return;
    }
    std::error_code ec;
    // Never truncate an existing INI (including one that could not be opened).
    if (!std::filesystem::exists(path,ec) && !ec) {
        std::ofstream out(path);
        if (out) vc::write(out,settings);
    }
}

void ResetField() {
    field.reset(settings);
    haveFrame=false;
}

vc::Vec Vec(const RwV3d& v) { return {v.x,v.y,v.z}; }

bool BadWeather(short wt) {
    switch (wt) {
    case WEATHER_CLOUDY_LA:
    case WEATHER_CLOUDY_SF:
    case WEATHER_RAINY_SF:
    case WEATHER_CLOUDY_VEGAS:
    case WEATHER_CLOUDY_COUNTRYSIDE:
    case WEATHER_RAINY_COUNTRYSIDE:
        return true;
    default: return false;
    }
}

// All texture pointers are borrowed for this pass only. No RwTextureRead fallback,
// no owned references, and no GPU objects that need to survive a device reset.
struct Textures {
    std::array<RwRaster*,16> rasters{};
    std::size_t count=0;
    void add(RwTexture* tex) {
        RwRaster* raster=tex?RwTextureGetRaster(tex):nullptr;
        if (raster && count<rasters.size() &&
            std::find(rasters.begin(),rasters.begin()+count,raster)==rasters.begin()+count)
            rasters[count++]=raster;
    }
    RwRaster* get(std::uint32_t key) const { return count?rasters[key%count]:nullptr; }
};

Textures GetTextures() {
    Textures out;
    RwTexture* original=CClouds::ms_vc.m_pTex;
    if (settings.includeOriginalTextureInRandom) out.add(original);
    if (settings.useAdditionalTextures && CTxdStore::ms_pTxdPool) {
        const int slot=CTxdStore::FindTxdSlot("particle");
        if (slot>=0) {
            // Access the loaded dictionary directly rather than mutating the global
            // current TXD or calling the game's loose-file texture loader.
            const auto* def=CTxdStore::ms_pTxdPool->GetAt(slot);
            if (def && def->m_pRwDictionary) {
                for (const auto& name : settings.textureNames)
                    out.add(RwTexDictionaryFindNamedTexture(def->m_pRwDictionary,name.c_str()));
            }
        }
    }
    if (!out.count) out.add(original);
    return out;
}

// Save every RenderWare state that this renderer explicitly changes. If a getter
// fails, skip the custom pass rather than writing an unrestorable state.
class RenderStates {
    struct Entry { RwRenderState key; void* value{}; };
    std::array<Entry,13> old{{
        {rwRENDERSTATEZWRITEENABLE},{rwRENDERSTATEZTESTENABLE},
        {rwRENDERSTATEVERTEXALPHAENABLE},{rwRENDERSTATESRCBLEND},
        {rwRENDERSTATEDESTBLEND},{rwRENDERSTATETEXTURERASTER},
        {rwRENDERSTATEFOGENABLE},{rwRENDERSTATECULLMODE},
        {rwRENDERSTATETEXTUREFILTER},{rwRENDERSTATETEXTUREADDRESSU},
        {rwRENDERSTATETEXTUREADDRESSV},{rwRENDERSTATEALPHATESTFUNCTION},
        {rwRENDERSTATESHADEMODE}
    }};
    bool active=false;
    static void set(RwRenderState state, std::uintptr_t value) {
        RwRenderStateSet(state,reinterpret_cast<void*>(value));
    }
public:
    bool begin() {
        for (auto& e:old) if (!RwRenderStateGet(e.key,&e.value)) return false;
        active=true;
        set(rwRENDERSTATEZWRITEENABLE,FALSE);
        set(rwRENDERSTATEZTESTENABLE,TRUE);
        set(rwRENDERSTATEVERTEXALPHAENABLE,TRUE);
        set(rwRENDERSTATESRCBLEND,rwBLENDSRCALPHA);
        set(rwRENDERSTATEDESTBLEND,rwBLENDINVSRCALPHA);
        set(rwRENDERSTATEFOGENABLE,settings.useGameFog?TRUE:FALSE);
        set(rwRENDERSTATECULLMODE,rwCULLMODECULLNONE);
        set(rwRENDERSTATETEXTUREFILTER,rwFILTERLINEAR);
        set(rwRENDERSTATETEXTUREADDRESSU,rwTEXTUREADDRESSCLAMP);
        set(rwRENDERSTATETEXTUREADDRESSV,rwTEXTUREADDRESSCLAMP);
        // The game's alpha-test threshold can otherwise slice off soft fade edges.
        set(rwRENDERSTATEALPHATESTFUNCTION,rwALPHATESTFUNCTIONALWAYS);
        set(rwRENDERSTATESHADEMODE,rwSHADEMODEGOURAUD);
        return true;
    }
    ~RenderStates() {
        if (active) for (auto it=old.rbegin();it!=old.rend();++it)
            RwRenderStateSet(it->key,it->value);
    }
};

struct Colour { float r,g,b; };
Colour Blend(Colour a,Colour b,float t) {
    return {vc::mix(a.r,b.r,t),vc::mix(a.g,b.g,t),vc::mix(a.b,b.b,t)};
}
Colour BaseColour() {
    const auto& cc=CTimeCycle::m_CurrentColours;
    const float grey=std::min(255.f,(cc.m_nSkyTopRed+cc.m_nSkyTopGreen+cc.m_nSkyTopBlue+
        cc.m_nSkyBottomRed+cc.m_nSkyBottomGreen+cc.m_nSkyBottomBlue)/6.f+settings.colorBrightnessBoost);
    const Colour fallback{grey,grey,grey};
    if (!settings.useTimecycTint) return fallback;
    const Colour low{float(cc.m_nLowCloudsRed),float(cc.m_nLowCloudsGreen),float(cc.m_nLowCloudsBlue)};
    const Colour bottom{float(cc.m_nFluffyCloudsBottomRed),float(cc.m_nFluffyCloudsBottomGreen),float(cc.m_nFluffyCloudsBottomBlue)};
    return Blend(fallback,Blend(low,bottom,settings.fluffyCloudBottomBlend),settings.timecycTintStrength);
}
unsigned char Byte(float x) { return static_cast<unsigned char>(vc::clamp(x,0.f,255.f)+.5f); }

bool Visible(RwCamera* camera, vc::Vec p, float radius) {
    RwSphere sphere{{p.x,p.y,p.z},radius};
    return RwCameraFrustumTestSphere(camera,&sphere)!=rwSPHEREOUTSIDE;
}

struct DrawCard {
    vc::Card card;
    float depth;
    int order;
    RwRaster* raster;
};
std::array<DrawCard,vc::MaxClouds*vc::MaxPuffs> drawList;
std::array<RwIm3DVertex,BatchVertices> vertices;

void Flush(int& count,RwRaster* raster) {
    if (!count) return;
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER,raster);
    if (RwIm3DTransform(vertices.data(),count,nullptr,rwIM3D_VERTEXXYZ|rwIM3D_VERTEXUV|rwIM3D_VERTEXRGBA)) {
        RwIm3DRenderPrimitive(rwPRIMTYPETRILIST);
        RwIm3DEnd();
    }
    count=0;
}

void Append(const vc::Card& card,Colour colour,int& count) {
    // Two triangles, identical UV convention to SA: top-left (0,0).
    constexpr int corners[6]={0,1,2,1,3,2};
    const auto normal=vc::unit(vc::Vec{
        card.right.y*card.up.z-card.right.z*card.up.y,
        card.right.z*card.up.x-card.right.x*card.up.z,
        card.right.x*card.up.y-card.right.y*card.up.x});
    for (int index:corners) {
        const bool right=(index&1)!=0, bottom=(index&2)!=0;
        const auto p=card.center+card.right*(right?1.f:-1.f)+card.up*(bottom?-1.f:1.f);
        const auto shade=bottom?settings.bottomShade:1.f;
        auto& v=vertices[count++];
        RwIm3DVertexSetPos(&v,p.x,p.y,p.z);
        RwIm3DVertexSetNormal(&v,normal.x,normal.y,normal.z);
        RwIm3DVertexSetU(&v,right?1.f:0.f);
        RwIm3DVertexSetV(&v,bottom?1.f:0.f);
        RwIm3DVertexSetRGBA(&v,Byte(colour.r*shade),Byte(colour.g*shade),Byte(colour.b*shade),Byte(card.alpha*255.f));
    }
}

void __cdecl RenderClouds() {
    if (!CGame::CanSeeOutSideFromCurrArea() || CWeather::UnderWaterness>.5f) return;
    RwCamera* camera=Scene.m_pCamera;
    if (!camera || RwCameraGetCurrentCamera()!=camera) return;
    RwFrame* frame=RwCameraGetFrame(camera);
    if (!frame) return;
    const RwMatrix* matrix=RwFrameGetLTM(frame);
    if (!matrix) return;
    const vc::View view{Vec(matrix->pos),vc::unit(Vec(matrix->right)),vc::unit(Vec(matrix->up)),
        vc::unit(Vec(matrix->at)),RwCameraGetFarClipPlane(camera),RwCameraGetNearClipPlane(camera)};
    if (!vc::finite(view.pos) || !vc::finite(view.forward) || vc::length(view.forward)<.9f ||
        !vc::finite(view.up) || !vc::finite(view.right) || vc::length(view.up)<.9f || vc::length(view.right)<.9f)
        return;
    const auto& cc=CTimeCycle::m_CurrentColours;
    const auto range=vc::range(settings,view.farClip,cc.m_fFarClip);
    if (range.end<=1.f) return;
    const Textures textures=GetTextures();
    if (!textures.count) return;

    // Simulate at most once per game frame, using the actual render camera.
    const auto frameId=CTimer::m_FrameCounter;
    if (!haveFrame || frameId!=lastFrame) {
        const float dt=CTimer::m_UserPause || CTimer::m_CodePause ? 0.f : CTimer::ms_fTimeStep;
        const float weather=vc::weatherFactor(BadWeather(CWeather::OldWeatherType),BadWeather(CWeather::NewWeatherType),
            CWeather::InterpolationValue,CWeather::Rain);
        const auto& wind=CWeather::WindDir;
        field.update(settings,view,range,vc::target(settings,weather),
            vc::Vec{wind.x,wind.y,0.f}*CWeather::Wind,dt);
        haveFrame=true;
        lastFrame=frameId;
    }

    int drawCount=0;
    for (const auto& c:field.clouds) {
        if (!c.used) continue;
        const float a=vc::cloudAlpha(settings,c,view,range);
        if (a<1.f/255.f || !Visible(camera,c.position,vc::bound(c))) continue;
        vc::Card cards[vc::MaxPuffs];
        const int count=vc::cards(settings,c,view,a,cards);
        for (int i=0;i<count;++i) {
            auto& card=cards[i];
            const float depth=vc::viewDepth(view,card.center);
            const float radius=vc::cardBound(card);
            if (card.alpha<1.f/255.f || depth+radius<view.nearClip ||
                depth-radius>view.farClip || !Visible(camera,card.center,radius)) continue;
            drawList[drawCount]={card,depth,drawCount,textures.get(card.textureKey)};
            ++drawCount;
        }
    }
    if (!drawCount) return;
    // Stable ties avoid shimmer between intersecting crossed cards; sort by view depth,
    // not radial distance, so translucent clouds compose correctly with camera rotation.
    // Insertion order is deterministic; explicit tie index avoids stable_sort heap allocations.
    std::sort(drawList.begin(),drawList.begin()+drawCount,[](const DrawCard& a,const DrawCard& b) {
        if (a.depth!=b.depth) return a.depth>b.depth;
        return a.order<b.order;
    });

    RenderStates states;
    if (!states.begin()) return;
    const Colour base=BaseColour();
    const Colour sky{float(cc.m_nSkyBottomRed),float(cc.m_nSkyBottomGreen),float(cc.m_nSkyBottomBlue)};
    int count=0;
    RwRaster* raster=nullptr;
    for (int i=0;i<drawCount;++i) {
        const auto& d=drawList[i];
        // Batch only consecutive matching rasters: no texture regrouping that breaks alpha order.
        if (count+6>BatchVertices || raster!=d.raster) Flush(count,raster);
        raster=d.raster;
        const auto shade=vc::mix(settings.bottomShade,1.f,d.card.shade);
        const Colour lit{base.r*shade,base.g*shade,base.b*shade};
        const float haze=settings.useGameFog?0.f:settings.hazeStrength*vc::smooth(vc::length(d.card.center-view.pos)/range.end);
        Append(d.card,Blend(lit,sky,haze),count);
    }
    Flush(count,raster);
}

class VolumetricCloudsExtended {
public:
    VolumetricCloudsExtended() {
        LoadIni();
        if (!plugin::IsGameVersion10us()) {
            Log("Unsupported executable: expected GTA SA 1.0 US. No hooks installed.");
            return;
        }
        // Refuse to overwrite an unknown instruction or another mod's call redirect.
        const auto* code=reinterpret_cast<const unsigned char*>(CloudCall);
        std::int32_t displacement=0;
        std::memcpy(&displacement,code+1,sizeof(displacement));
        if (code[0]!=0xE8 || CloudCall+5+displacement!=NativeRenderer) {
            Log("Volumetric cloud call does not target 0x716380; left untouched.");
            return;
        }
        ResetField();
        plugin::patch::RedirectCall(CloudCall,RenderClouds);
        plugin::Events::initGameEvent += ResetField;
        plugin::Events::reInitGameEvent += ResetField;
        plugin::Events::shutdownRwEvent += ResetField;
        Log("Procedural volumetric cloud renderer installed (SA 1.0 US).");
    }
} gPlugin;
} // namespace
