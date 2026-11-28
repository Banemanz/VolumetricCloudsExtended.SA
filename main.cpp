// GTA SA 1.0 US / Plugin-SDK. Replaces only the volumetric-cloud call in RenderEffects.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "plugin.h"
#include "CClouds.h"
#include "CClock.h"
#include "CGame.h"
#include "CWeather.h"
#include "CTimeCycle.h"
#include "CTimer.h"
#include "CTxdStore.h"
#include "CScene.h"
#include "common.h"
#include "CloudD3D9.hpp"
#include "CloudBillboards.hpp"
#include "CloudCache.hpp"
#include "CloudProfiler.hpp"
#include <cstdio>
#include <new>

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
std::array<IDirect3DTexture9*,2> canopyTextures{};
std::array<std::vector<vc::canopy::Pixel>,2> canopyPixels;
IDirect3DDevice9* canopyDevice=nullptr; // borrowed
int canopySize=0;
std::array<bool,2> canopyHasPixels{};
vc::d3d9::Options canopyOptions;
vc::d3d9::CloudProfiler cloudProfiler;
vc::d3d9::StateScope cloudState;
vc::d3d9::ProjectedMesh canopyProjected,bankProjected;
vc::canopyVolume::Relief canopyRelief;
vc::canopyVolume::Mesh canopyReliefMesh;
vc::billboards::Atlas billboardAtlas;
vc::banks::Mesh billboardMesh;
vc::d3d9::ProjectedMesh billboardProjected;
std::array<IDirect3DTexture9*,2> billboardTextures{};
vc::d3d9::Options billboardOptions;
bool billboardPrepared=false,billboardFailed=false;
bool canopyReady=false;
vc::canopy::Drift canopyDrift;
bool canopyAttempted=false,canopyUploadFailed=false;
unsigned int lastFrame = 0;
vc::banks::Volume bankVolume;
vc::banks::Mesh bankMesh;
vc::banks::GpuMesh bankGpuMesh;
vc::d3d9::GpuStream bankGpu;
vc::d3d9::FarLayer bankFarLayer;
std::array<IDirect3DVolumeTexture9*,2> bankTextures{};
IDirect3DDevice9* bankDevice=nullptr; // borrowed
bool bankFailed=false,bankPrepared=false,bankDrawLogged=false,bankEmptyLogged=false;
vc::d3d9::Options bankOptions;
IDirect3DQuery9* bankPixelQuery=nullptr; // One nonblocking diagnostic, not a per-frame readback.
bool bankPixelQueryAttempted=false,bankDownsampled=false;
int bankQueryFrames=0;
IDirect3DDevice9* CurrentD3DDevice() {
    // SA 1.0 US: _RwD3DDevice at C97C28. The SDK's public getter at
    // 7F9D50 clears RW's matrix/stream cache every call; do not call it.
    return *reinterpret_cast<IDirect3DDevice9**>(0xC97C28);
}
void ReleaseBankTextures() {
    bankFarLayer.release();
    bankGpu.release();
    if(bankPixelQuery){bankPixelQuery->Release();bankPixelQuery=nullptr;}
    bankPixelQueryAttempted=false;bankQueryFrames=0;bankDownsampled=false;
    for(auto& texture:bankTextures) {
        if(texture)texture->Release();
        texture=nullptr;
    }
    for(auto& texture:billboardTextures){if(texture)texture->Release();texture=nullptr;}
    billboardFailed=false;
    bankDevice=nullptr;bankFailed=false;bankDrawLogged=false;bankEmptyLogged=false;
}
void ReleaseBanks() {
    ReleaseBankTextures();
    bankVolume={};bankMesh={};bankGpuMesh={};bankProjected={};bankPrepared=false;
    billboardAtlas={};billboardMesh={};billboardProjected={};billboardPrepared=false;
}

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

void ReleaseCanopyTextures() {
    cloudState.release();
    cloudProfiler.release();
    for(auto& texture:canopyTextures){if(texture)texture->Release();texture=nullptr;}
    canopyDevice=nullptr;canopyUploadFailed=false;
}
void ReleaseCanopy() {
    ReleaseCanopyTextures();
    canopyPixels={};canopyProjected={};canopyRelief={};canopyReliefMesh={};canopySize=0;
    canopyReady=false;canopyAttempted=false;
    ReleaseBanks();
}
bool PrepareSchematic() {
    if(!canopyAttempted) {
        canopyAttempted=true;
        const auto cachePath=ModulePath()/L"VolumetricCloudsExtended.cache";
        const auto build=[&](int size) {
            try {
                bool cached=false;
                {
                    std::ifstream in(cachePath,std::ios::binary);
                    if(in)cached=vc::cache::load(in,settings,size,canopyPixels,bankVolume,billboardAtlas);
                }
                if(cached) {
                    bankPrepared=settings.hybridClouds;
                    billboardPrepared=bankPrepared&&vc::billboards::eligible(settings,vc::cloudRange(settings));
                    Log("Cloud schematic cache hit (static masks, volume, occupancy and far atlas).");
                } else {
                vc::canopy::Map map;map.build(size,std::uint32_t(settings.seed));
                if(settings.hybridClouds) {
                    try {bankVolume.build(settings,map);bankPrepared=true;
                        if(vc::billboards::eligible(settings,vc::cloudRange(settings))) {
                            try {billboardAtlas.build(settings,bankVolume);billboardPrepared=true;}
                            catch(const std::bad_alloc&) {billboardAtlas={};Log("Far billboard bake allocation failed; retaining full volume.");}
                        }
                    }
                    catch(const std::bad_alloc&) {ReleaseBanks();bankFailed=true;Log("3D density allocation failed; using canopy.");}
                }
                if(bankPrepared)vc::canopyVolume::project(settings,bankVolume,size,canopyPixels);
                else {
                    map.shadeWeatherLayers(settings);
                    canopyPixels[0]=std::move(map.pixels);canopyPixels[1]=std::move(map.extraPixels);
                }
                // Never publish partially generated data or replace an existing
                // valid cache unless the complete replacement was written.
                if(!settings.hybridClouds||(bankPrepared&&(!vc::billboards::eligible(settings,vc::cloudRange(settings))||billboardPrepared))) {
                    auto tmp=cachePath;tmp+=L".tmp."+std::to_wstring(GetCurrentProcessId());
                    bool written=false;
                    {
                        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
                        if(out)written=vc::cache::save(out,settings,size,canopyPixels,bankVolume,billboardAtlas);
                        out.close();written=written&&bool(out);
                    }
                    if(written&&MoveFileExW(tmp.c_str(),cachePath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
                        Log("Cloud schematic baked once and cached on disk.");
                    else {std::error_code ec;std::filesystem::remove(tmp,ec);}
                }
                }
                if(bankPrepared)canopyRelief.build(settings,bankVolume);
                canopySize=size;
                for(int i=0;i<2;++i)canopyHasPixels[i]=std::any_of(canopyPixels[i].begin(),canopyPixels[i].end(),[](const auto& p){return p.a!=0;});
                return true;
            } catch(const std::bad_alloc&) {canopyPixels={};ReleaseBanks();return false;}
        };
        const int requested=vc::canopy::textureSize(settings);
        canopyReady=build(requested);
        if(!canopyReady&&requested!=vc::canopy::EconomySize)canopyReady=build(vc::canopy::EconomySize);
        if(canopyReady)Log(canopyRelief.ready?"Canopy: shared volume underside + world-fixed relief (live coverage).":
            "Canopy: static weather masks ready (live coverage).");
    }
    return canopyReady;
}
bool EnsureCanopy() {
    auto* device=CurrentD3DDevice();
    if(!device)return false;
    if(device!=canopyDevice)ReleaseCanopyTextures();
    if(canopyUploadFailed)return false;
    if(canopyTextures[0]&&canopyTextures[1])return true;
    if(!PrepareSchematic())return false;
    D3DCAPS9 caps{};
    if(FAILED(device->GetDeviceCaps(&caps)))return false;
    canopyOptions=vc::d3d9::options(caps);if(!canopyRelief.ready)canopyOptions.dual=false;
    const DWORD linear=D3DPTFILTERCAPS_MINFLINEAR|D3DPTFILTERCAPS_MAGFLINEAR;
    canopyOptions.filter=(caps.TextureFilterCaps&linear)==linear?D3DTEXF_LINEAR:D3DTEXF_POINT;
    canopyOptions.mip=(caps.TextureFilterCaps&D3DPTFILTERCAPS_MIPFLINEAR)!=0;
    canopyDevice=device;
    try {
        D3DFORMAT format=D3DFMT_UNKNOWN;
        const bool compact=vc::d3d9::compactFormat(device,D3DRTYPE_TEXTURE,
            canopyOptions.filter==D3DTEXF_LINEAR||canopyOptions.mip);
        const auto result=vc::d3d9::uploadPair(canopyTextures,compact,format,
            [&](int i,IDirect3DTexture9** out,bool managed,D3DFORMAT f){
                return vc::d3d9::uploadCanopy(device,canopyPixels[i],UINT(canopySize),out,managed,true,f);
            });
        if(result){Log(format==D3DFMT_A8L8?"Canopy: lossless A8L8 textures.":"Canopy: RGBA texture fallback.");return true;}
    } catch(const std::bad_alloc&) {}
    ReleaseCanopyTextures();canopyDevice=device;canopyUploadFailed=true;
    Log("Canopy: native 2D texture upload failed; using classic clouds.");return false;
}

void LogD3DFailure(const char* phase,vc::d3d9::Result result) {
    char message[256];
    std::snprintf(message,sizeof(message),"3D cloud %s: %s failed (HRESULT 0x%08lX).",
        phase,result.operation,static_cast<unsigned long>(result.hr));
    Log(message);
}
bool EnsureBanks() {
    if(!settings.hybridClouds||!bankPrepared)return false;
    auto* device=CurrentD3DDevice();
    if(!device)return false;
    if(device!=bankDevice)ReleaseBankTextures();
    if(bankFailed)return false;
    if(bankPixelQuery) {
        DWORD samples=0;
        const auto hr=bankPixelQuery->GetData(&samples,sizeof(samples),0); // no FLUSH / wait
        if(hr==S_OK||FAILED(hr)||++bankQueryFrames>=120) {
            char info[160];
            if(hr==S_OK) {
                std::snprintf(info,sizeof(info),"3D cloud first draw: %lu depth-passing samples (GPU occlusion query).",
                    static_cast<unsigned long>(samples));Log(info);
            } else Log("3D cloud GPU query unavailable or timed out; submission status still logged.");
            bankPixelQuery->Release();bankPixelQuery=nullptr;
        }
    }
    if(bankTextures[0]&&bankTextures[1])return true;
    bankDevice=device;
    const auto fail=[](){ReleaseBankTextures();bankDevice=CurrentD3DDevice();bankFailed=true;Log("3D cloud bank unavailable; retaining canopy.");return false;};
    D3DCAPS9 caps{};
    D3DDEVICE_CREATION_PARAMETERS creation{};
    HRESULT hr=device->GetDeviceCaps(&caps);
    if(FAILED(hr)){LogD3DFailure("initialization",vc::d3d9::failed(hr,"GetDeviceCaps"));return fail();}
    const HRESULT creationResult=device->GetCreationParameters(&creation);
    if(FAILED(creationResult))LogD3DFailure("diagnostic",vc::d3d9::failed(creationResult,"GetCreationParameters"));
    bankOptions=vc::d3d9::options(caps);
    char info[384];
    std::snprintf(info,sizeof(info),"3D cloud device: flags=0x%08lX%s volume=%d maxExtent=%lu filters=0x%08lX textures=%lu stages=%lu ops=0x%08lX.",
        static_cast<unsigned long>(creation.BehaviorFlags),
        (creation.BehaviorFlags&D3DCREATE_PUREDEVICE)?" (pure device supported)":"",
        !!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP),static_cast<unsigned long>(caps.MaxVolumeExtent),
        static_cast<unsigned long>(caps.VolumeTextureFilterCaps),static_cast<unsigned long>(caps.MaxSimultaneousTextures),
        static_cast<unsigned long>(caps.MaxTextureBlendStages),static_cast<unsigned long>(caps.TextureOpCaps));
    Log(info);
    if(!(caps.TextureCaps&D3DPTEXTURECAPS_VOLUMEMAP)||caps.MaxVolumeExtent<16) {
        Log("3D cloud device does not advertise usable volume textures.");return fail();
    }
    // Power-of-two downsampling for older caps and allocation failures.
    // System-memory -> default works with D3D9Ex; managed fallback is only
    // attempted if the driver rejects staging/upload.
    if(!bankOptions.dual)Log("3D cloud device lacks dual-volume blending; using nearest weather endpoint.");
    if(bankOptions.filter==D3DTEXF_POINT)Log("3D cloud device lacks linear volume filtering; using point filtering.");
    const bool compact=vc::d3d9::compactFormat(device,D3DRTYPE_VOLUMETEXTURE,bankOptions.filter==D3DTEXF_LINEAR);
    for(UINT w=UINT(bankVolume.width);w>=16;w/=2) {
        if(w>caps.MaxVolumeExtent)continue;
        const UINT d=std::min(UINT(bankVolume.depth),w);
        D3DFORMAT format=D3DFMT_UNKNOWN;
        const auto result=vc::d3d9::uploadPair(bankTextures,compact,format,
            [&](int layer,IDirect3DVolumeTexture9** out,bool managed,D3DFORMAT f){
                return vc::d3d9::upload(device,bankVolume,layer,w,d,out,managed,f);
            });
        if(result) {
            bankDownsampled=w!=UINT(bankVolume.width)||d!=UINT(bankVolume.depth);
            const bool gpu=bankGpu.create(device,caps,creation.BehaviorFlags);
            std::snprintf(info,sizeof(info),"3D cloud bank ready: %lux%lux%lu %s volumes, %s, %s weather, %s filtering.",
                static_cast<unsigned long>(w),static_cast<unsigned long>(w),static_cast<unsigned long>(d),
                format==D3DFMT_A8L8?"lossless A8L8":"RGBA",
                gpu?"hardware vertex / fixed-function pixels":"CPU vertex / fixed-function pixels",
                bankOptions.dual?"interpolated":"endpoint",bankOptions.filter==D3DTEXF_LINEAR?"linear":"point");
            Log(info);return true;
        }
        LogD3DFailure("volume pair upload",result);
    }
    return fail();
}

bool EnsureBillboards() {
    if(!billboardPrepared||billboardFailed||!bankDevice)return false;
    if(billboardTextures[0]&&billboardTextures[1])return true;
    billboardOptions=canopyOptions;
    billboardOptions.dual=bankOptions.dual;
    billboardOptions.blendOp=bankOptions.blendOp;
    // Atlas gutter keeps bilinear taps in each image; don't use deep
    // atlas mips which could blend unrelated neighboring cells.
    billboardOptions.mip=false;
    try {
        D3DFORMAT format=D3DFMT_UNKNOWN;
        const bool compact=vc::d3d9::compactFormat(bankDevice,D3DRTYPE_TEXTURE,billboardOptions.filter==D3DTEXF_LINEAR);
        const auto result=vc::d3d9::uploadPair(billboardTextures,compact,format,
            [&](int i,IDirect3DTexture9** out,bool managed,D3DFORMAT f){
                return vc::d3d9::uploadCanopy(bankDevice,billboardAtlas.pixels[i],vc::billboards::AtlasSize,out,managed,false,f);
            });
        if(!result){LogD3DFailure("far billboard upload",result);throw std::bad_alloc();}
        Log(format==D3DFMT_A8L8?"Far cloud LOD: lossless A8L8 textures.":"Far cloud LOD: RGBA texture fallback.");
        const auto lod=vc::banks::lod(settings,vc::cloudRange(settings));
        char msg[160];std::snprintf(msg,sizeof(msg),"Far cloud LOD ready: volume-projected billboards, transition %.0f-%.0f.",lod.start,lod.end);Log(msg);
        return true;
    } catch(const std::bad_alloc&) {
        for(auto& t:billboardTextures){if(t)t->Release();t=nullptr;}
        billboardFailed=true;Log("Far cloud LOD unavailable; retaining full 3D distance.");return false;
    }
}

// Preload at the end of RenderWare initialization, outside a frame.
// The immutable CPU schematic remains resident through new games and
// D3D loss/reset; only the GPU resources are released on device resets.
// Old/late ASI loaders still have the lazy Ensure* path.
void PreloadClouds() {
    if(settings.renderMode!=2||!settings.targetProcClouds||settings.opacity<=0.f||settings.cumulusDensity<=0.f)return;
    const bool first=!canopyAttempted;
    if(!PrepareSchematic())return;
    if(EnsureCanopy()&&EnsureBanks())EnsureBillboards();
    if(first)Log("Cloud schematic resident in RAM; initial texture preload attempted.");
}
void ResetField() {
    field.reset(settings);
    canopyDrift={};
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
std::array<DrawCard,vc::MaxClouds*vc::MaxCards> drawList;
std::array<RwIm3DVertex,BatchVertices> vertices;
static_assert(sizeof(RwImVertexIndex)==sizeof(std::uint16_t));
// Direct projected vertices preserve the game's exact screen projection and
// depth for nearby geometry; only distant Z is clamped. Fog is independent.
bool DrawCanopy(const vc::View& view,const vc::Range& range,float hybridFade,
                const vc::weather::State& weather,const vc::canopy::Light& light,
                const D3DMATRIX& transform,const D3DVIEWPORT9& viewport,vc::RGB fog,float wx,float wy,
                const vc::Range* volumeRange,vc::d3d9::DrawState& states) {
    if(settings.opacity<=0.f||settings.cumulusDensity<=0.f||!settings.targetProcClouds)return true;
    if(canopyRelief.ready) {
        const float visibility=vc::weather::visibility(weather)*vc::weather::clearOpacity(weather);
        const float coverage=canopyOptions.dual?weather.coverage:weather.coverage>=.5f?1.f:0.f;
        if(visibility<=1.f/255.f||
           !((coverage<.999f&&canopyHasPixels[0])||(coverage>.001f&&canopyHasPixels[1])))return true;
        vc::canopyVolume::mesh(canopyReliefMesh,canopyRelief,settings,view,range,canopyDrift,wx,wy,hybridFade,volumeRange);
        if(canopyReliefMesh.indices.empty())return true;
        canopyProjected.clip.clear();canopyProjected.clip.reserve(canopyReliefMesh.vertices.size());
        for(const auto& v:canopyReliefMesh.vertices) {
            const float f=vc::cloudFogFactor(settings,v.distance);
            canopyProjected.clip.push_back(vc::d3d9::clipVertex(v.pos,v.u,v.v,0.f,v.alpha*(canopyOptions.vertexFog?1.f:f),f,transform));
        }
        // Same endpoint interpolation, luminance and fog as the actual
        // volume. One fixed-function draw even during mixed weather.
        vc::d3d9::projectMesh(canopyProjected,viewport,vc::banks::bankLight(light,weather),visibility,
            canopyReliefMesh.indices.data(),canopyReliefMesh.indices.size());
        const auto result=vc::d3d9::drawProjected(canopyDevice,{canopyTextures[0],canopyTextures[1]},
            canopyOptions,canopyProjected,coverage,false,fog,true,false,volumeRange?&states:nullptr);
        if(!result){LogD3DFailure("volume underside canopy draw",result);return false;}
        return true;
    }
    const auto weights=vc::canopy::layerWeights(settings,weather);
    const bool base=canopyTextures[0]&&canopyHasPixels[0]&&weights.base>1.f/255.f;
    const bool extra=canopyTextures[1]&&canopyHasPixels[1]&&weights.extra>1.f/255.f;
    if(!base&&!extra)return true;
    const auto mesh=vc::canopy::mesh(settings,view,range,canopyDrift,hybridFade);
    canopyProjected.clip.clear();canopyProjected.clip.reserve(mesh.vertices.size());
    for(const auto& v:mesh.vertices) {
        const float f=vc::cloudFogFactor(settings,v.distance);
        canopyProjected.clip.push_back(vc::d3d9::clipVertex(v.pos,v.u,v.v,0.f,v.alpha*(canopyOptions.vertexFog?1.f:f),f,transform));
    }
    // Both masks share geometry. Project once, cache vertex opacity, then
    // modulate only alpha for each pass (no second clipping/traversal).
    vc::d3d9::projectMesh(canopyProjected,viewport,light.bright,1.f,mesh.indices.data(),mesh.indices.size());
    if(canopyProjected.indices.empty())return true;
    static std::vector<unsigned char> alphaBase;
    const bool both=base&&extra;
    if(both) {
        alphaBase.resize(canopyProjected.vertices.size());
        for(std::size_t i=0;i<alphaBase.size();++i)alphaBase[i]=static_cast<unsigned char>(canopyProjected.vertices[i].rgba>>24);
    }
    for(int layer=0;layer<2;++layer) {
        const float alpha=layer?weights.extra:weights.base;
        if(layer?!extra:!base)continue;
        // Reuse the clipped geometry but recover each layer's alpha from the
        // original packed alpha, rather than multiplying the other layer.
        if(both||alpha!=1.f)for(std::size_t i=0;i<canopyProjected.vertices.size();++i) {
            auto& v=canopyProjected.vertices[i];
            const auto a=both?alphaBase[i]:static_cast<unsigned char>(v.rgba>>24);
            v.rgba=(v.rgba&0x00FFFFFFu)|(DWORD(Byte(a*alpha))<<24);
        }
        const auto result=vc::d3d9::drawProjected(canopyDevice,{canopyTextures[layer],canopyTextures[layer]},
            canopyOptions,canopyProjected,0.f,false,fog,true,false,(both||hybridFade>0.f)?&states:nullptr);
        if(!result){LogD3DFailure("canopy draw",result);return false;}
    }
    return true;
}

bool DrawBillboards(const vc::View& view,const vc::Range& range,const vc::weather::State& climate,
                    const vc::canopy::Light& light,const D3DMATRIX& transform,
                    const D3DVIEWPORT9& viewport,vc::RGB fog,float wx,float wy,float handover,vc::d3d9::DrawState& states) {
    const float visibility=vc::weather::visibility(climate)*vc::weather::clearOpacity(climate)*handover;
    if(visibility<=1.f/255.f)return true;
    vc::billboards::buildMesh(billboardMesh,settings,view,range,canopyDrift,billboardAtlas,wx,wy,
        billboardOptions.dual?climate.coverage:(climate.coverage>=.5f?1.f:0.f));
    if(billboardMesh.indices.empty())return true;
    billboardProjected.clip.clear();billboardProjected.clip.reserve(billboardMesh.vertices.size());
    for(const auto& v:billboardMesh.vertices)billboardProjected.clip.push_back(vc::d3d9::clipVertex(
        v.pos,v.u,v.v,v.w,v.alpha*(billboardOptions.vertexFog?1.f:v.fog),v.fog,transform));
    vc::d3d9::projectMesh(billboardProjected,viewport,vc::banks::bankLight(light,climate),visibility,
        billboardMesh.indices.data(),billboardMesh.indices.size());
    const auto result=vc::d3d9::drawProjected(bankDevice,{billboardTextures[0],billboardTextures[1]},billboardOptions,
        billboardProjected,climate.coverage,false,fog,true,false,&states);
    if(!result){LogD3DFailure("far billboard draw",result);billboardFailed=true;return false;}
    return true;
}
bool DrawBanks(RwCamera* camera,const vc::View& view,const vc::Range& range,
               const vc::weather::State& climate,const vc::canopy::Light& light,
               const D3DMATRIX& transform,const D3DMATRIX& projection,const D3DVIEWPORT9& viewport,vc::RGB fog,float handover,float playerZ,vc::d3d9::DrawState& states) {
    if(!bankDevice||!bankTextures[0]||!bankTextures[1])return false;
    auto* window=RwCameraGetViewWindow(camera);
    if(!window)return false;
    const auto* offset=RwCameraGetViewOffset(camera);
    const float wx=window->x+(offset?std::abs(offset->x):0.f),
                wy=window->y+(offset?std::abs(offset->y):0.f);
    // The canopy now approximates the same optical column as the volume.
    // Blend the actual slice opacity against that projection rather than
    // introducing a second unrelated density field in the transition.
    const float handoverVisibility=vc::weather::visibility(climate)*vc::weather::clearOpacity(climate)*(canopyRelief.ready?1.f:handover);
    if(handoverVisibility<=1.f/255.f)return true;
    const float visibility=canopyRelief.ready?handoverVisibility:handoverVisibility*handover;
    bool farReady=vc::billboards::eligible(settings,range)&&EnsureBillboards();
    const auto bankLight=vc::banks::bankLight(light,climate);
    vc::d3d9::GpuPasses passes;
    passes.stream=&bankGpu;
    if(settings.farHalfRes&&!settings.cumulusDetail)passes.farLayer=&bankFarLayer;
    bool useGpu=false;
    UINT gpuCount=0;
    const auto weather=bankOptions.dual?climate.coverage:(climate.coverage>=.5f?1.f:0.f);
    const auto* occupancy=bankDownsampled?nullptr:&bankVolume;
    try {
        if(farReady)farReady=DrawBillboards(view,range,climate,light,transform,viewport,fog,wx,wy,canopyRelief.ready?1.f:handover,states);
        if(visibility<=1.f/255.f)return true;
        useGpu=bankGpu.available()&&passes.prepare(settings,view,range,canopyDrift,farReady,
            bankOptions.vertexFog,visibility,bankLight,transform,projection,viewport);
        if(useGpu) {
            vc::banks::buildMesh(bankGpuMesh,settings,view,range,canopyDrift,wx,wy,occupancy,weather,farReady);
            if(bankGpuMesh.vertices.empty())return true;
            passes.restrictTo(bankGpuMesh);
            gpuCount=UINT(bankGpuMesh.vertices.size());
            const auto uploaded=bankGpu.uploadReusable(bankGpuMesh);
            if(!uploaded) {
                LogD3DFailure("hardware cloud upload; using CPU",uploaded);
                bankGpu.release();useGpu=false;
            }
        }
        if(!useGpu) {
            vc::banks::buildMesh(bankMesh,settings,view,range,canopyDrift,wx,wy,occupancy,weather,farReady);
            if(bankMesh.indices.empty())return true;
            bankProjected.clip.clear();bankProjected.clip.reserve(bankMesh.vertices.size());
            for(const auto& v:bankMesh.vertices)bankProjected.clip.push_back(vc::d3d9::clipVertex(
                v.pos,v.u,v.v,v.w,v.alpha*(bankOptions.vertexFog?1.f:v.fog),v.fog,transform));
            vc::d3d9::projectMesh(bankProjected,viewport,bankLight,visibility,bankMesh.indices.data(),bankMesh.indices.size());
            if(bankProjected.indices.empty())return true;
        }
    } catch(const std::bad_alloc&) {Log("3D cloud mesh allocation failed.");return false;}
    if(!bankPixelQueryAttempted) {
        bankPixelQueryAttempted=true;
        if(SUCCEEDED(bankDevice->CreateQuery(D3DQUERYTYPE_OCCLUSION,&bankPixelQuery))) {
            if(FAILED(bankPixelQuery->Issue(D3DISSUE_BEGIN))) {
                bankPixelQuery->Release();bankPixelQuery=nullptr;
            }
        }
    }
    const bool queryThisDraw=bankPixelQuery&&bankQueryFrames==0&&!bankDrawLogged;
    const auto result=useGpu?vc::d3d9::drawRaw(bankDevice,{bankTextures[0],bankTextures[1]},bankOptions,
        nullptr,gpuCount,sizeof(vc::banks::GpuVertex),0,
        nullptr,UINT(bankGpuMesh.triangleCount()),climate.coverage,bankOptions.vertexFog,
        true,false,D3DCOLOR_XRGB(vc::d3d9::byte(fog.r),vc::d3d9::byte(fog.g),vc::d3d9::byte(fog.b)),
        nullptr,nullptr,false,&passes,&states):
        vc::d3d9::drawProjected(bankDevice,{bankTextures[0],bankTextures[1]},bankOptions,
        bankProjected,climate.coverage,true,fog,true,false,&states);
    if(queryThisDraw && FAILED(bankPixelQuery->Issue(D3DISSUE_END))) {
        bankPixelQuery->Release();bankPixelQuery=nullptr;
    }
    if(!result) {
        LogD3DFailure("draw",result);
        if(useGpu) {
            // A partial draw cannot be safely repeated in the same frame.
            // Retain the volume on the proven CPU path from the next frame.
            bankGpu.release();Log("Hardware cloud draw rejected; using CPU vertex path.");return true;
        }
        return false;
    }
    if(!bankDrawLogged) {
        char info[256];
        std::snprintf(info,sizeof(info),"3D cloud draw submitted: %d world-fixed slice patches, %lu triangles, player Z=%.1f, canopy Z=%.1f, camera Z=%.1f, handover=%.2f, coverage=%.2f.",
            useGpu?bankGpuMesh.slices:bankMesh.slices,static_cast<unsigned long>(useGpu?bankGpuMesh.triangleCount():bankProjected.indices.size()/3),playerZ,vc::canopy::altitude(settings),
            view.pos.z,handover,climate.coverage);
        Log(info);bankDrawLogged=true;
    }
    return true;
}

void Flush(int& count,RwRaster* raster) {
    if (!count) return;
    RwRenderStateSet(rwRENDERSTATETEXTURERASTER,raster);
    if (RwIm3DTransform(vertices.data(),count,nullptr,rwIM3D_VERTEXXYZ|rwIM3D_VERTEXUV|rwIM3D_VERTEXRGBA)) {
        RwIm3DRenderPrimitive(rwPRIMTYPETRILIST);
        RwIm3DEnd();
    }
    count=0;
}

void Append(const vc::Card& card,const vc::CardColours& colours,int& count) {
    // Two triangles, identical UV convention to SA: top-left (0,0).
    constexpr int corners[6]={0,1,2,1,3,2};
    const auto normal=vc::unit(vc::Vec{
        card.right.y*card.up.z-card.right.z*card.up.y,
        card.right.z*card.up.x-card.right.x*card.up.z,
        card.right.x*card.up.y-card.right.y*card.up.x});
    for (int index:corners) {
        const bool right=(index&1)!=0, bottom=(index&2)!=0;
        const auto p=card.center+card.right*(right?1.f:-1.f)+card.up*(bottom?-1.f:1.f);
        const auto& colour=bottom?colours.bottom:colours.top;
        auto& v=vertices[count++];
        RwIm3DVertexSetPos(&v,p.x,p.y,p.z);
        RwIm3DVertexSetNormal(&v,normal.x,normal.y,normal.z);
        RwIm3DVertexSetU(&v,right?1.f:0.f);
        RwIm3DVertexSetV(&v,bottom?1.f:0.f);
        RwIm3DVertexSetRGBA(&v,Byte(colour.r),Byte(colour.g),Byte(colour.b),Byte(card.alpha*255.f));
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
    if (range.end<=1.f && settings.renderMode!=2) return;
    const vc::Palette palette{
        {float(cc.m_nSkyTopRed),float(cc.m_nSkyTopGreen),float(cc.m_nSkyTopBlue)},
        {float(cc.m_nSkyBottomRed),float(cc.m_nSkyBottomGreen),float(cc.m_nSkyBottomBlue)},
        {float(cc.m_nLowCloudsRed),float(cc.m_nLowCloudsGreen),float(cc.m_nLowCloudsBlue)},
        {float(cc.m_nFluffyCloudsBottomRed),float(cc.m_nFluffyCloudsBottomGreen),float(cc.m_nFluffyCloudsBottomBlue)}
    };
    const int requestedMode=settings.renderMode;
    struct RestoreMode { int mode; ~RestoreMode(){settings.renderMode=mode;} } restore{requestedMode};
    const auto frameId=CTimer::m_FrameCounter;
    const bool newFrame=!haveFrame || frameId!=lastFrame;
    const float dt=CTimer::m_UserPause || CTimer::m_CodePause ? 0.f : CTimer::ms_fTimeStep;
    const float weather=vc::weatherFactor(BadWeather(CWeather::OldWeatherType),BadWeather(CWeather::NewWeatherType),
        CWeather::InterpolationValue,CWeather::Rain);
    const auto& wind=CWeather::WindDir;
    const vc::Vec windVector=vc::Vec{wind.x,wind.y,0.f}*CWeather::Wind;
    bool modern=false;
    int desired=vc::target(settings,weather);
    auto cardRange=range;
    if (requestedMode==2) {
        if (!settings.targetProcClouds || settings.opacity<=0.f || settings.cumulusDensity<=0.f) return;
        const int index=std::clamp(CTimeCycle::m_CurrentStoredValue,0,15);
        const auto& sun=CTimeCycle::m_VectorToSun[index];
        static_assert(WEATHER_RAINY_SF==8 && WEATHER_RAINY_COUNTRYSIDE==16 && WEATHER_FOGGY_SF==9 && WEATHER_SANDSTORM_DESERT==19);
        const auto climate=vc::weather::sample({CWeather::OldWeatherType,CWeather::NewWeatherType,CWeather::InterpolationValue,
            CWeather::CloudCoverage,CWeather::Rain,CWeather::Foggyness,CWeather::Sandstorm,CWeather::ExtraSunnyness,
            CWeather::bScriptsForceRain,CWeather::LightningFlash});
        const auto light=vc::canopy::liveLighting(settings,palette,{sun.x,sun.y,sun.z},climate);
        modern=EnsureCanopy();
        if(modern && newFrame) canopyDrift.update(settings,windVector,dt);
        if(modern) {
            // Use the actual player (or their occupied vehicle), not the
            // camera height. Orbiting/looking upward cannot trigger a handover.
            const auto* player=FindPlayerEntity();
            const float playerZ=player?player->GetPosition().z:0.f;
            const float requestedBlend=player?vc::canopy::handover(settings,playerZ):0.f;
            const bool volumeReady=settings.hybridClouds&&(canopyRelief.ready||requestedBlend>1.f/255.f)&&EnsureBanks();
            const float volumeBlend=volumeReady?requestedBlend:0.f;
            const auto independent=vc::cloudRange(settings);
            const auto localVolume=canopyRelief.ready?vc::canopyVolume::volumeRange(settings,independent,playerZ,volumeBlend):independent;
            const auto* nearVolume=volumeReady&&canopyRelief.ready?&localVolume:nullptr;
            const auto& viewMatrix=*reinterpret_cast<const D3DMATRIX*>(0xC9BC80);
            const auto& projection=*reinterpret_cast<const D3DMATRIX*>(0x8E2458);
            const auto transform=vc::d3d9::multiply(viewMatrix,projection);
            D3DVIEWPORT9 viewport{};
            // GetViewport is explicitly legal on pure devices. Keep the
            // game viewport and projection; do not set either.
            auto* device=canopyDevice;
            if(device&&SUCCEEDED(device->GetViewport(&viewport))) {
                plugin::Call<0x7FC200>(); // flush RW pending state before snapshot
                const bool profiled=cloudProfiler.begin(device,Log);
                const auto captured=cloudState.capture(device);
                if(captured) {
                    bool drawn=false;
                    try {
                        const auto fog=vc::canopy::fogColour(palette);
                        vc::d3d9::DrawState states;
                        const auto* window=RwCameraGetViewWindow(camera);
                        const auto* offset=RwCameraGetViewOffset(camera);
                        const float wx=window?window->x+(offset?std::abs(offset->x):0.f):1.f,
                            wy=window?window->y+(offset?std::abs(offset->y):0.f):1.f;
                        drawn=DrawCanopy(view,independent,volumeBlend,climate,light,transform,viewport,fog,wx,wy,nearVolume,states);
                        if(drawn&&volumeReady&&!DrawBanks(camera,view,localVolume,climate,light,transform,projection,viewport,fog,volumeBlend,playerZ,states)) {
                            bankFailed=true;Log("3D cloud draw failed; retaining full canopy from next frame.");
                        }
                    } catch(const std::bad_alloc&) {Log("Cloud projection allocation failed.");}
                    const auto restored=cloudState.restore();
                    cloudProfiler.end(profiled,drawn&&bool(restored),playerZ,climate.coverage,volumeReady?localVolume.end:0.f,Log);
                    if(!restored){LogD3DFailure("restore",restored);return;}
                    if(drawn) {haveFrame=true;lastFrame=frameId;return;}
                } else cloudProfiler.end(profiled,false,playerZ,climate.coverage,0.f,Log);
            }
        }
        // Resource failure uses the classic native geometry, without
        // modifying any world fog settings.
        settings.renderMode=0;
    }
    if(cardRange.end<=1.f)return;
    Textures textures=GetTextures();
    if (!textures.count) return;

    if (newFrame) {
        field.update(settings,view,cardRange,desired,windVector,dt);
        haveFrame=true;lastFrame=frameId;
    }

    int drawCount=0;
    for (const auto& c:field.clouds) {
        if (!c.used) continue;
        const float a=vc::cloudAlpha(settings,c,view,cardRange);
        if (a<1.f/255.f || !Visible(camera,c.position,vc::bound(c))) continue;
        vc::Card cards[vc::MaxCards];
        const int count=vc::cards(settings,c,view,a,cards);
        for (int i=0;i<count;++i) {
            auto& card=cards[i];
            if (card.alpha<1.f/255.f) continue;
            const float depth=vc::viewDepth(view,card.center);
            const float radius=vc::cardBound(card);
            if (depth+radius<view.nearClip ||
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
    const vc::CloudLighting lighting(settings,palette);
    int count=0;
    RwRaster* raster=nullptr;
    for (int i=0;i<drawCount;++i) {
        const auto& d=drawList[i];
        // Batch only consecutive matching rasters: no texture regrouping that breaks alpha order.
        if (count+6>BatchVertices || raster!=d.raster) Flush(count,raster);
        raster=d.raster;
        const float distanceNorm=settings.lightingMode==1?vc::length(d.card.center-view.pos)/cardRange.end:0.f;
        Append(d.card,lighting.colours(d.card.shade,distanceNorm),count);
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
        std::error_code profileError;
        cloudProfiler.enabled=std::filesystem::exists(ModulePath()/L"VolumetricCloudsExtended.profile",profileError);
        if(cloudProfiler.enabled)Log("Cloud profile enabled: sampled CPU and GPU times (nonblocking).");
        ResetField();
        plugin::patch::RedirectCall(CloudCall,RenderClouds);
        plugin::Events::initRwEvent += PreloadClouds;
        plugin::Events::initGameEvent += ResetField;
        plugin::Events::initGameEvent += PreloadClouds;
        plugin::Events::reInitGameEvent += ResetField;
        plugin::Events::shutdownRwEvent += ResetField;
        plugin::Events::shutdownRwEvent += ReleaseCanopy;
        plugin::Events::d3dLostEvent += ReleaseBankTextures;
        plugin::Events::d3dLostEvent += ReleaseCanopyTextures;
        plugin::Events::d3dResetEvent += ReleaseBankTextures;
        plugin::Events::d3dResetEvent += ReleaseCanopyTextures;
        Log(settings.renderMode==2 ? "Procedural volumetric cloud renderer installed (SA 1.0 US, world-fixed D3D9 volume banks, shared-density canopy, far cloud LOD, independent cloud atmosphere, pure-device compatible)." : settings.lightingMode==0
            ? "Procedural volumetric cloud renderer installed (SA 1.0 US, native lighting)."
            : "Procedural volumetric cloud renderer installed (SA 1.0 US, custom lighting).");
    }
} gPlugin;
} // namespace
